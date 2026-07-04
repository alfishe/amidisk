"""amidisk command line interface.

Paths inside images use Amiga notation: VOLUME:Path/To/File where VOLUME
is a drive name (DH0), a volume label (Workbench) or a partition index.
Single-volume images (ADF, bare HDF) may omit the volume prefix.
"""

import argparse
import hashlib
import json
import os
import sys
import time
from datetime import datetime

from .image import open_image, ImageError
from .fs.ffs import FSError
from .fs.util import str_to_protect
from .blkdev import BlockDeviceError


def print_progress(current_bytes, total_bytes, current_file=""):
    """Prints a dynamic, overwriting progress bar to stdout."""
    percent = (current_bytes / total_bytes) * 100 if total_bytes > 0 else 0
    
    import time
    now = time.time()
    last_t = getattr(print_progress, "_last_t", 0)
    
    # Throttle: print at most every 250ms, or on completion
    if current_bytes < total_bytes and (now - last_t < 0.25):
        return
        
    print_progress._last_t = now

    bar_len = 30
    filled = int((percent / 100) * bar_len) if percent <= 100 else bar_len
    bar = '#' * filled + '-' * (bar_len - filled)
    
    sys.stdout.write("\x1b[2K\r[%s] %5.1f%% (%s / %s) %s" % (
        bar, percent, human_size(current_bytes), human_size(total_bytes), current_file[:40]
    ))
    sys.stdout.flush()

def print_transfer_stats(file_count, dir_count, total_bytes, elapsed):
    sys.stdout.write("\x1b[2K\r")
    if elapsed <= 0: elapsed = 0.001
    speed = (total_bytes / 1024.0 / 1024.0) / elapsed
    print("Transferred: %d files, %d dirs" % (file_count, dir_count))
    print("Total size:  %s" % human_size(total_bytes))
    print("Time taken:  %.2f seconds" % elapsed)
    print("Avg speed:   %.2f MB/s" % speed)



def human_size(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0


def _parse_image_arg(image_arg, vol_arg=None):
    """
    Parses 'path/to/image:Volume' syntax, returning (path, volume).
    Falls back to an explicit volume argument if provided.
    """
    if ":" in image_arg and not os.path.exists(image_arg):
        path, vol = image_arg.rsplit(":", 1)
        if os.path.exists(path):
            return path, vol
        path, vol = image_arg.split(":", 1)
        return path, vol
    return image_arg, vol_arg or ""


def _combine_path(vol_name, path):
    """Combines a parsed volume name and an optional path back into a volume:path string."""
    path = path or ""
    if not path:
        if "/" in vol_name and ":" not in vol_name:
            v, p = vol_name.split("/", 1)
            return v + ":" + p
        return vol_name + ":" if vol_name else ""
    if ":" not in path and vol_name:
        return vol_name + ":" + path
    return path


# ---------------------------------------------------------------- info
def cmd_info(args):
    img_path, vol_name = _parse_image_arg(args.image)

    with open_image(img_path) as img:
        info = img.get_info()
        
        # Detailed single partition info
        if vol_name:
            v = img.get_volume(vol_name)
            if args.json:
                print(json.dumps(v.part_blk.get_info() if hasattr(v, "part_blk") else {}, indent=2))
                return 0
            
            print("\n[ RDB Partition Configuration ]")
            print("  drive:      %s" % v.name)
            if v.partition:
                p = v.partition.get_info()
                print("  dos type:   %s" % p.get("dos_type", "Unknown"))
                print("  size:       %s (cylinders %d to %d)" % (
                    human_size(p.get("size_bytes", 0)),
                    p.get("low_cyl", 0),
                    p.get("high_cyl", 0)
                ))
                boot_str = "Yes (Priority: %d)" % p.get("boot_pri", 0) if p.get("bootable") else "No"
                print("  boot:       %s" % boot_str)
                print("  blksize:    %d bytes" % p.get("block_size", 512))
                print("  max trans:  0x%08X" % p.get("max_transfer", 0))
                print("  mask:       0x%08X" % p.get("mask", 0))
                print("  buffers:    %d" % p.get("num_buffer", 0))
                
            print("\n[ Filesystem Statistics ]")
            try:
                vol = v.mount()
                vi = vol.get_info()
                total_bytes = vi["total_blocks"] * vi["block_size"]
                used_bytes = total_bytes - vi["free_bytes"]
                print("  label:      %r" % vi["label"])
                print("  used:       %s" % human_size(used_bytes))
                print("  free:       %s" % human_size(vi["free_bytes"]))
                
                if p and "bb_chksum_match" in p:
                    match = p["bb_chksum_match"]
                    got = p["bb_got_chksum"]
                    exp = p["bb_expected_chksum"]
                    bb_id = p.get("bb_id", 0)
                    from amidisk.rdb.blocks import dos_type_to_str
                    id_str = dos_type_to_str(bb_id) if bb_id else "Unknown"
                    if match:
                        print("  bootblock:  Custom Executable (chksum OK: 0x%08X)" % got)
                    else:
                        if got == 0:
                            print("  bootblock:  Standard (ID: %s, no custom bootcode)" % id_str)
                        else:
                            print("  bootblock:  INVALID CHECKSUM (ID: %s, got: 0x%08X, exp: 0x%08X)" % (id_str, got, exp))
                
                sys.stdout.write("  Scanning...")
                sys.stdout.flush()
                rep = vol.check(deep=args.deep)
                sys.stdout.write("\x1b[2K\r") # clear the scanning line
                
                print("  files:      %d" % rep["files"])
                print("  dirs:       %d" % rep["dirs"])
                status = "OK" if rep["ok"] else "ERRORS"
                print("  state:      %s" % status)
            except (FSError, BlockDeviceError) as ex:
                print("error: cannot mount %s: %s" % (v.name, ex))
            return 0

        # General image info
        if args.json:
            print(json.dumps(info, indent=2))
            return 0
            
        print("\n[ Global Image Info ]")
        print("image:  %s" % info["path"])
        print("kind:   %s  (%s)" % (info["kind"], human_size(info["size_bytes"])))
        if "rdb" in info:
            r = info["rdb"]
            print(
                "disk:   %s %s %s  chs=%d/%d/%d  rdb@block %d"
                % (
                    r["disk_vendor"], r["disk_product"], r["disk_revision"],
                    r["cylinders"], r["heads"], r["sectors"], r["rdb_block"],
                )
            )
            print("\n[ RDB Partition Configuration ]")
            for p in info["partitions"]:
                boot_str = "Yes (Priority: %d)" % p["boot_pri"] if p["bootable"] else "No"
                print("  %-8s %-32s Size: %-10s Cyls: %d to %d" % (
                    p["drv_name"] + ":", p["dos_type"], human_size(p["size_bytes"]), p["low_cyl"], p["high_cyl"]
                ))
                print("           Bootable: %-22s BlockSize: %-5d   Buffers: %d" % (
                    boot_str, p["block_size"], p.get("num_buffer", 0)
                ))
                print("           MaxTransfer: 0x%08X        Mask: 0x%08X" % (
                    p["max_transfer"], p["mask"]
                ))
                
                if "bb_chksum_match" in p:
                    match = p["bb_chksum_match"]
                    got = p["bb_got_chksum"]
                    exp = p["bb_expected_chksum"]
                    bb_id = p.get("bb_id", 0)
                    
                    from amidisk.rdb.blocks import dos_type_to_str
                    id_str = dos_type_to_str(bb_id) if bb_id else "Unknown"
                    
                    if match:
                        print("           Bootblock: Custom Executable (chksum OK: 0x%08X)" % got)
                    else:
                        if got == 0:
                            print("           Bootblock: Standard (ID: %s, no custom bootcode)" % id_str)
                        else:
                            print("           Bootblock: INVALID CHECKSUM (ID: %s, got: 0x%08X, expected: 0x%08X)" % (id_str, got, exp))
                print("") # separator between partitions
            if info["filesystems"]:
                print("\n[ RDB Embedded Filesystems ]")
                for f in info["filesystems"]:
                    print(
                        "  %d: %-7s v%-8s patch_flags=0x%x"
                        % (f["num"], f["dos_type"], f["version"], f["patch_flags"])
                    )
        print("\n[ Filesystem Statistics ]")
        # same data source as --json: the per-volume section of get_info()
        for entry in info.get("volumes", []):
            name = entry["name"]
            if "volume" in entry:
                vi = entry["volume"]
                print(
                    "  %-8s %-16r %-7s %10s free of %s"
                    % (
                        name + ":", vi["label"], vi["dos_type"],
                        human_size(vi["free_bytes"]),
                        human_size(vi["total_blocks"] * vi["block_size"]),
                    )
                )
            else:
                print("  %-8s <unmountable: %s>" % (name + ":", entry.get("error", "?")))
                ident = entry.get("identify")
                if ident:
                    print(
                        "           declared type %s, %s, first block '%s'"
                        % (
                            ident["declared_dos_type"] or "unknown",
                            human_size(ident["size_bytes"]),
                            ident["boot_magic"],
                        )
                    )
                    for g in ident["guesses"]:
                        print("           detected: %s" % g)
    return 0


# ---------------------------------------------------------------- ls
def cmd_ls(args):
    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path) as img:
        vol_ref, path = img.parse_path(_combine_path(vol_name, ""))
        vol = vol_ref.mount()
        entry = vol.resolve(path)
        entries = (
            vol.list_dir(path) if entry.is_dir() else [entry]
        )
        if not entries:
            print("(empty)")
            return 0
            
        if args.json:
            print(json.dumps([e.get_info() for e in entries], indent=2))
        else:
            for e in entries:
                i = e.get_info()
                size = "" if i["size"] is None else str(i["size"])
                name = i["name"] + ("/" if e.is_dir() else "")
                comment = ("  ; " + i["comment"]) if i["comment"] else ""
                print(
                    "%s %10s  %s  %s%s"
                    % (i["protect"], size, i["mtime"], name, comment)
                )
    return 0


def cmd_cat(args):
    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path) as img:
        vol_ref, path = img.parse_path(_combine_path(vol_name, ""))
        vol = vol_ref.mount()
        out = sys.stdout.buffer
        for chunk in vol.read_file(path):
            out.write(chunk)
        out.flush()
    return 0


# ---------------------------------------------------------------- extract
def _extract_file(vol, amiga_path, entry, dest):
    with open(dest, "wb") as fh:
        for chunk in vol.read_file(entry):
            fh.write(chunk)
    ts = entry.mtime().timestamp()
    os.utime(dest, (ts, ts))


def _safe_name(name):
    return name.replace("/", "_").replace("\x00", "_")


def cmd_extract(args):
    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path) as img:
        vol_ref, path = img.parse_path(_combine_path(vol_name, args.path))
        vol = vol_ref.mount()
        entry = vol.resolve(path)
        dest = args.dest
        if entry.is_file():
            if os.path.isdir(dest):
                dest = os.path.join(dest, _safe_name(entry.name_str()))
            start_time = time.time()
            _extract_file(vol, path, entry, dest)
            elapsed = time.time() - start_time
            print_transfer_stats(1, 0, entry.size, elapsed)
            return 0
        if not args.recursive:
            print("error: %s is a directory (use -r)" % (path or "volume root"),
                  file=sys.stderr)
            return 1
        base_prefix = "/".join(
            p.decode("latin-1") for p in vol._split(path)
        )
        os.makedirs(dest, exist_ok=True)
        count = 0
        dir_count = 0
        total_bytes = 0
        start_time = time.time()
        for prefix, e in vol.walk(path):
            rel = prefix[len(base_prefix):].lstrip("/") if base_prefix else prefix
            host_dir = os.path.join(dest, *[_safe_name(p) for p in rel.split("/") if p])
            if e.sec_type == 2:  # dir
                os.makedirs(os.path.join(host_dir, _safe_name(e.name_str())), exist_ok=True)
                dir_count += 1
            elif e.is_file():
                os.makedirs(host_dir, exist_ok=True)
                _extract_file(vol, None, e, os.path.join(host_dir, _safe_name(e.name_str())))
                count += 1
                total_bytes += e.size
                print_progress(total_bytes, total_bytes, e.name_str())
            elif e.is_link():
                print("\nskipping link: %s/%s" % (prefix, e.name_str()), file=sys.stderr)
                
        elapsed = time.time() - start_time
        print_transfer_stats(count, dir_count, total_bytes, elapsed)
    return 0


# ---------------------------------------------------------------- cp
class ChecksumStream:
    def __init__(self, iterator):
        self.iterator = iterator
        self.md5 = hashlib.md5()
        
    def __iter__(self):
        return self
        
    def __next__(self):
        try:
            chunk = next(self.iterator)
            self.md5.update(chunk)
            return chunk
        except StopIteration:
            raise

def cmd_cp(args):
    img1, vol1 = _parse_image_arg(args.src)
    img2, vol2 = _parse_image_arg(args.dst)
    
    # Check modes
    # If no volume was parsed for the source (meaning it's a host path),
    # and the destination is an image, route to cmd_put.
    if not vol1 and vol2:
        args.image = args.dst
        args.dest = "" # Empty string instead of None to avoid legacy SCP logic
        return cmd_put(args)
        
    # If the source is an image and the destination is a host path, route to cmd_extract.
    if vol1 and not vol2:
        args.image = args.src
        args.path = "" # Empty string for _combine_path
        args.dest = args.dst
        return cmd_extract(args)
        
    if not vol1 and not vol2:
        print("error: standard host-to-host transfer. Please use your operating system's 'cp' command.", file=sys.stderr)
        return 1
        
    # Otherwise, image-to-image streaming
    with open_image(img1) as src_img, open_image(img2, writable=True) as dst_img:
        src_vol_ref, src_path = src_img.parse_path(_combine_path(vol1, ""))
        dst_vol_ref, dst_path = dst_img.parse_path(_combine_path(vol2, ""))
        
        src_vol = src_vol_ref.mount()
        dst_vol = dst_vol_ref.mount()
        
        start_time = time.time()
        total_bytes = 0
        file_count = 0
        dir_count = 0
        
        entry = src_vol.resolve(src_path)
        if entry.is_file():
            try:
                if dst_vol.resolve(dst_path).is_dir():
                    dst_path = dst_path.rstrip("/") + "/" + entry.name_str()
            except FSError:
                pass
            
            stream = src_vol.read_file(entry)
            chk_stream = ChecksumStream(stream) if getattr(args, "checksum", False) else stream
            
            dst_vol.write_file(dst_path, chk_stream, size=entry.size, protect=entry.protect, mtime=entry.mtime())
            total_bytes += entry.size
            file_count += 1
            if getattr(args, "checksum", False):
                print(f"copied {dst_path} (md5: {chk_stream.md5.hexdigest()})")
            else:
                print(f"copied {dst_path}")
            return 0
            
        if not args.recursive:
            print(f"error: {src_path} is a directory (use -r)", file=sys.stderr)
            return 1
            
        base_prefix = "/".join(p.decode("latin-1") for p in src_vol._split(src_path))
        base_dest = dst_path.rstrip("/")
        
        import contextlib as _ctxlib
        bulk_ctx = (dst_vol.bulk() if getattr(args, "bulk", False)
                    and hasattr(dst_vol, "bulk") else _ctxlib.nullcontext())
        with bulk_ctx:
         for prefix, e in src_vol.walk(src_path):
            rel = prefix[len(base_prefix):].lstrip("/") if base_prefix else prefix
            amiga_dir = base_dest + ("/" + rel if rel else "") if base_dest else rel
            
            if e.is_dir():
                dst_dir = amiga_dir + "/" + e.name_str() if amiga_dir else e.name_str()
                dst_vol.makedirs(dst_dir)
                dir_count += 1
            elif e.is_file():
                if amiga_dir:
                    dst_vol.makedirs(amiga_dir)
                dst_path_full = amiga_dir + "/" + e.name_str() if amiga_dir else e.name_str()
                stream = src_vol.read_file(e)
                dst_vol.write_file(dst_path_full, stream, size=e.size, protect=e.protect, mtime=e.mtime())
                total_bytes += e.size
                file_count += 1
                print_progress(total_bytes, total_bytes, e.name_str())
                
        elapsed = time.time() - start_time
        print_transfer_stats(file_count, dir_count, total_bytes, elapsed)
    return 0


def get_md5_host(path):
    import hashlib
    h = hashlib.md5()
    try:
        with open(path, "rb") as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                h.update(chunk)
    except Exception:
        return ""
    return h.hexdigest()

def get_md5_amiga(vol, path):
    import hashlib
    h = hashlib.md5()
    try:
        entry = vol.resolve(path)
        for chunk in vol.read_file(entry):
            h.update(chunk)
    except Exception:
        return ""
    return h.hexdigest()

def walk_host(base_path):
    items = {}
    if not os.path.exists(base_path):
        return items
    if os.path.isfile(base_path):
        items[""] = {
            "rel_path": "",
            "is_dir": False,
            "size": os.path.getsize(base_path),
            "mtime": datetime.fromtimestamp(os.path.getmtime(base_path))
        }
        return items
    for root, dirs, files in os.walk(base_path):
        for d in dirs:
            host_path = os.path.join(root, d)
            rel = os.path.relpath(host_path, base_path).replace(os.sep, "/")
            items[rel] = {
                "rel_path": rel,
                "is_dir": True,
                "size": 0,
                "mtime": datetime.fromtimestamp(os.path.getmtime(host_path))
            }
        for f in files:
            host_path = os.path.join(root, f)
            rel = os.path.relpath(host_path, base_path).replace(os.sep, "/")
            items[rel] = {
                "rel_path": rel,
                "is_dir": False,
                "size": os.path.getsize(host_path),
                "mtime": datetime.fromtimestamp(os.path.getmtime(host_path))
            }
    return items

def walk_amiga(vol, base_path):
    items = {}
    try:
        root_entry = vol.resolve(base_path)
    except FSError:
        return items
        
    if root_entry.is_file():
        items[""] = {
            "rel_path": "",
            "is_dir": False,
            "size": root_entry.size,
            "mtime": root_entry.mtime()
        }
        return items
        
    base_parts = vol._split(base_path)
    base_prefix = "/".join(p.decode("latin-1") for p in base_parts)
    
    for prefix, e in vol.walk(base_path):
        rel = prefix[len(base_prefix):].lstrip("/") if base_prefix else prefix
        if e.is_dir():
            d_rel = (rel + "/" + e.name_str()) if rel else e.name_str()
            items[d_rel] = {
                "rel_path": d_rel,
                "is_dir": True,
                "size": 0,
                "mtime": e.mtime()
            }
        elif e.is_file():
            f_rel = (rel + "/" + e.name_str()) if rel else e.name_str()
            items[f_rel] = {
                "rel_path": f_rel,
                "is_dir": False,
                "size": e.size,
                "mtime": e.mtime()
            }
    return items

def should_update(src_info, dst_info, args, src_is_image, src_vol, dst_is_image, dst_vol, img1, img2):
    if args.checksum:
        if src_is_image:
            spath = src_info.get("_full_path") or src_info["rel_path"]
            src_md5 = get_md5_amiga(src_vol, spath)
        else:
            spath = os.path.join(img1, src_info["rel_path"]) if src_info["rel_path"] else img1
            src_md5 = get_md5_host(spath)
            
        if dst_is_image:
            dpath = dst_info.get("_full_path") or dst_info["rel_path"]
            dst_md5 = get_md5_amiga(dst_vol, dpath)
        else:
            dpath = os.path.join(img2, dst_info["rel_path"]) if dst_info["rel_path"] else img2
            dst_md5 = get_md5_host(dpath)
            
        return src_md5 != dst_md5
        
    if src_info["size"] != dst_info["size"]:
        return True
        
    if args.update:
        return src_info["mtime"] > dst_info["mtime"]
        
    src_t = int(src_info["mtime"].timestamp())
    dst_t = int(dst_info["mtime"].timestamp())
    return src_t != dst_t

def cmd_sync(args):
    img1, vol1 = _parse_image_arg(args.src)
    img2, vol2 = _parse_image_arg(args.dst)
    
    if not vol1 and not vol2:
        print("error: standard host-to-host sync. Please use rsync or other native tools.", file=sys.stderr)
        return 1
        
    src_is_image = bool(vol1)
    dst_is_image = bool(vol2)
    
    src_img = None
    dst_img = None
    
    try:
        if src_is_image:
            src_img = open_image(img1)
            src_vol_ref, src_path = src_img.parse_path(_combine_path(vol1, ""))
            src_vol = src_vol_ref.mount()
            src_items = walk_amiga(src_vol, src_path)
            for rel in src_items:
                src_items[rel]["_full_path"] = src_path.rstrip("/") + "/" + rel if rel else src_path
        else:
            src_vol = None
            src_items = walk_host(img1)
            
        if not src_items:
            print("error: source path not found or empty.", file=sys.stderr)
            return 1
            
        if dst_is_image:
            dst_img = open_image(img2, writable=not args.dry_run)
            dst_vol_ref, dst_path = dst_img.parse_path(_combine_path(vol2, ""))
            dst_vol = dst_vol_ref.mount()
            dst_items = walk_amiga(dst_vol, dst_path)
            for rel in dst_items:
                dst_items[rel]["_full_path"] = dst_path.rstrip("/") + "/" + rel if rel else dst_path
        else:
            dst_vol = None
            dst_items = walk_host(img2)
            
        to_delete = []
        to_create_dirs = []
        to_copy_files = []
        
        # 1. Identify deletions
        if args.delete:
            for rel, dst_info in sorted(dst_items.items(), key=lambda x: x[0], reverse=True):
                if rel not in src_items:
                    to_delete.append((rel, dst_info))
                    
        # 2. Identify updates and creations
        for rel, src_info in sorted(src_items.items(), key=lambda x: x[0]):
            if not rel:
                # Single file sync
                if rel not in dst_items:
                    to_copy_files.append((rel, src_info, "create"))
                else:
                    dst_info = dst_items[rel]
                    if should_update(src_info, dst_info, args, src_is_image, src_vol, dst_is_image, dst_vol, img1, img2):
                        to_copy_files.append((rel, src_info, "update"))
                continue
                
            if src_info["is_dir"]:
                if rel not in dst_items:
                    to_create_dirs.append((rel, src_info))
                elif not dst_items[rel]["is_dir"]:
                    to_delete.append((rel, dst_items[rel]))
                    to_create_dirs.append((rel, src_info))
            else:
                if rel not in dst_items:
                    to_copy_files.append((rel, src_info, "create"))
                elif dst_items[rel]["is_dir"]:
                    to_delete.append((rel, dst_items[rel]))
                    to_copy_files.append((rel, src_info, "create"))
                else:
                    dst_info = dst_items[rel]
                    if should_update(src_info, dst_info, args, src_is_image, src_vol, dst_is_image, dst_vol, img1, img2):
                        to_copy_files.append((rel, src_info, "update"))
                        
        # 3. Perform Deletions
        deleted_count = 0
        for rel, info in to_delete:
            if args.dry_run:
                print(f"[DRY RUN] delete {'directory' if info['is_dir'] else 'file'}: {rel}")
                deleted_count += 1
                continue
                
            if dst_is_image:
                apath = dst_path.rstrip("/") + "/" + rel if rel else dst_path
                dst_vol.delete(apath, recursive=True)
                print(f"deleted from image: {rel}")
            else:
                hpath = os.path.join(img2, rel) if rel else img2
                if info["is_dir"]:
                    shutil.rmtree(hpath, ignore_errors=True)
                else:
                    os.remove(hpath)
                print(f"deleted from host: {rel}")
            deleted_count += 1
            
        # 4. Perform Dir Creations
        created_dirs_count = 0
        for rel, info in to_create_dirs:
            if args.dry_run:
                print(f"[DRY RUN] create directory: {rel}")
                created_dirs_count += 1
                continue
                
            if dst_is_image:
                apath = dst_path.rstrip("/") + "/" + rel if rel else dst_path
                dst_vol.makedirs(apath)
                print(f"created directory in image: {rel}")
            else:
                hpath = os.path.join(img2, rel) if rel else img2
                os.makedirs(hpath, exist_ok=True)
                print(f"created directory on host: {rel}")
            created_dirs_count += 1
            
        # 5. Perform File Copies
        copied_files_count = 0
        total_copied_bytes = 0
        
        import contextlib as _ctxlib
        bulk_ctx = (dst_vol.bulk() if dst_is_image and getattr(args, "bulk", False)
                    and hasattr(dst_vol, "bulk") and not args.dry_run else _ctxlib.nullcontext())
                    
        start_time = time.time()
        
        with bulk_ctx:
            for rel, info, op in to_copy_files:
                if args.dry_run:
                    print(f"[DRY RUN] {op} file: {rel} ({human_size(info['size'])})")
                    copied_files_count += 1
                    total_copied_bytes += info["size"]
                    continue
                    
                if src_is_image and dst_is_image:
                    spath = src_path.rstrip("/") + "/" + rel if rel else src_path
                    apath = dst_path.rstrip("/") + "/" + rel if rel else dst_path
                    src_entry = src_vol.resolve(spath)
                    stream = src_vol.read_file(src_entry)
                    dst_vol.write_file(
                        apath, stream, size=src_entry.size,
                        protect=src_entry.protect, mtime=src_entry.mtime()
                    )
                    print(f"synced: {rel} ({human_size(src_entry.size)})")
                    copied_files_count += 1
                    total_copied_bytes += src_entry.size
                    
                elif src_is_image and not dst_is_image:
                    spath = src_path.rstrip("/") + "/" + rel if rel else src_path
                    hpath = os.path.join(img2, rel) if rel else img2
                    src_entry = src_vol.resolve(spath)
                    os.makedirs(os.path.dirname(hpath), exist_ok=True)
                    with open(hpath, "wb") as fh:
                        for chunk in src_vol.read_file(src_entry):
                            fh.write(chunk)
                    mtime_ts = src_entry.mtime().timestamp()
                    os.utime(hpath, (mtime_ts, mtime_ts))
                    print(f"synced: {rel} ({human_size(src_entry.size)})")
                    copied_files_count += 1
                    total_copied_bytes += src_entry.size
                    
                elif not src_is_image and dst_is_image:
                    spath = os.path.join(img1, rel) if rel else img1
                    apath = dst_path.rstrip("/") + "/" + rel if rel else dst_path
                    parent_part = "/".join(apath.split("/")[:-1])
                    if parent_part:
                        dst_vol.makedirs(parent_part)
                    with open(spath, "rb") as fh:
                        mtime = datetime.fromtimestamp(os.path.getmtime(spath))
                        size = os.path.getsize(spath)
                        dst_vol.write_file(
                            apath, fh, size=size, mtime=mtime
                        )
                    print(f"synced: {rel} ({human_size(size)})")
                    copied_files_count += 1
                    total_copied_bytes += size
                    
        elapsed = time.time() - start_time
        
        if args.dry_run:
            print(f"\n[DRY RUN] Summary: would sync {copied_files_count} files ({human_size(total_copied_bytes)}), create {created_dirs_count} directories, delete {deleted_count} items.")
        else:
            print_transfer_stats(copied_files_count, created_dirs_count, total_copied_bytes, elapsed)
            if deleted_count:
                print(f"Deleted {deleted_count} extraneous items from destination.")
                
    finally:
        if src_img:
            src_img.close()
        if dst_img:
            dst_img.close()
            
    return 0


def truncate_name(name, max_len):
    if len(name) <= max_len:
        return name
    base, ext = os.path.splitext(name)
    if len(ext) >= max_len:
        return name[:max_len]
    allowed_base_len = max_len - len(ext) - 3
    if allowed_base_len < 2:
        return name[:max_len]
    left = allowed_base_len // 2 + (allowed_base_len % 2)
    right = allowed_base_len // 2
    return base[:left] + "..." + base[-right:] + ext

def _parse_scp_target(target):
    """Splits 'image.hdf:DH0:path' into ('image.hdf', 'DH0:path') safely."""
    for i in range(len(target)):
        if target[i] == ":":
            candidate = target[:i]
            if os.path.exists(candidate):
                return candidate, target[i+1:]
    return None, None

def cmd_put(args):
    if args.dest is None:
        img1, dest1 = _parse_scp_target(args.src)
        img2, dest2 = _parse_scp_target(args.image)
        
        if img1 is not None:
            # put src image:dest
            real_src = args.image
            args.image = img1
            args.dest = dest1
            args.src = real_src
        elif img2 is not None:
            # put image:dest src
            args.image = img2
            args.dest = dest2
        else:
            print("error: invalid syntax or image not found. Use 'put image src dest' or 'put src image:dest'", file=sys.stderr)
            return 1

    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path, writable=True) as img:
        vol_ref, path = img.parse_path(_combine_path(vol_name, args.dest))
        vol = vol_ref.mount()
        protect = str_to_protect(args.protect) if args.protect else 0
        comment = args.comment.encode("latin-1") if args.comment else b""

        max_len = getattr(vol, "max_name_len", 30)

        src = args.src
        
        from .archives import get_handler
        handler = get_handler(src)
        if handler:
            if not handler.test_archive():
                return 1
            base = path.rstrip("/")
            try:
                start_time = time.time()
                import contextlib as _ctxlib
                bulk_ctx = (vol.bulk() if getattr(args, "bulk", False)
                            and hasattr(vol, "bulk") else _ctxlib.nullcontext())
                with bulk_ctx:
                    n_files, n_dirs, size = handler.stream_to_volume(
                        vol, base, truncate_name, max_len, protect, comment
                    )
                elapsed = time.time() - start_time
                print_transfer_stats(n_files, n_dirs, size, elapsed)
                return 0
            except Exception:
                return 1

        if os.path.isfile(src):
            basename = truncate_name(os.path.basename(src), max_len)
            if not path or path.endswith("/"):
                path = (path or "") + basename
            else:
                # writing onto an existing dir drops the file inside it
                try:
                    if vol.resolve(path).is_dir():
                        path = path.rstrip("/") + "/" + basename
                except FSError:
                    pass
            with open(src, "rb") as fh:
                mtime = datetime.fromtimestamp(os.path.getmtime(src))
                size = os.path.getsize(src)
                vol.write_file(path, fh, size=size, protect=protect, comment=comment, mtime=mtime)
            print("wrote %s (%d bytes)" % (path, size))
            return 0
        if os.path.isdir(src):
            if not args.recursive:
                print("error: %s is a directory (use -r)" % src, file=sys.stderr)
                return 1
            base = path.rstrip("/")
            n = 0
            
            # Pre-calculate total bytes for progress reporting
            total_expected_bytes = 0
            for root, dirs, files in os.walk(src):
                for f in files:
                    total_expected_bytes += os.path.getsize(os.path.join(root, f))
                    
            total_bytes = 0
            import contextlib as _ctxlib
            bulk_ctx = (vol.bulk() if getattr(args, "bulk", False)
                        and hasattr(vol, "bulk") else _ctxlib.nullcontext())
            with bulk_ctx:
              for root, dirs, files in os.walk(src):
                dirs[:] = [d for d in dirs]
                rel = os.path.relpath(root, src)
                if rel != ".":
                    parts = [truncate_name(p, max_len) for p in rel.split(os.sep)]
                    rel_amiga = "/".join(parts)
                else:
                    rel_amiga = "."

                amiga_dir = base if rel_amiga == "." else (
                    (base + "/" if base else "") + rel_amiga
                )
                if amiga_dir:
                    vol.makedirs(amiga_dir)
                for f in files:
                    hostf = os.path.join(root, f)
                    truncated_f = truncate_name(f, max_len)
                    apath = (amiga_dir + "/" if amiga_dir else "") + truncated_f
                    with open(hostf, "rb") as fh:
                        mtime = datetime.fromtimestamp(os.path.getmtime(hostf))
                        size = os.path.getsize(hostf)
                        vol.write_file(apath, fh, size=size, protect=protect, comment=comment, mtime=mtime)
                    n += 1
                    total_bytes += size
                    print_progress(total_bytes, total_expected_bytes, truncated_f)
            
            print("") # lock progress bar
            print("wrote %d files under %s" % (n, base or "root"))
            return 0
        print("error: no such file: %s" % src, file=sys.stderr)
        return 1


# ---------------------------------------------------------------- mkdir / rm / mv
def cmd_mkdir(args):
    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path, writable=True) as img:
        vol_ref, path = img.parse_path(_combine_path(vol_name, ""))
        vol = vol_ref.mount()
        if args.parents:
            vol.makedirs(path)
        else:
            vol.mkdir(path)
        print("created %s" % args.image)
    return 0


def cmd_rm(args):
    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path, writable=True) as img:
        vol_ref, path = img.parse_path(_combine_path(vol_name, ""))
        vol = vol_ref.mount()
        vol.delete(path, recursive=args.recursive)
        print("deleted %s" % args.image)
    return 0


def cmd_mv(args):
    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path, writable=True) as img:
        vol_ref, src = img.parse_path(_combine_path(vol_name, args.src))
        vol_ref2, dst = img.parse_path(_combine_path(vol_name, args.dst))
        if vol_ref is not vol_ref2:
            print("error: mv works within one volume", file=sys.stderr)
            return 1
        vol = vol_ref.mount()
        vol.rename(src, dst)
        print("renamed %s -> %s" % (args.src, args.dst))
    return 0


# ---------------------------------------------------------------- check
def cmd_check(args):
    img_path, vol_name = _parse_image_arg(args.image, getattr(args, "volume", None))
    rc = 0
    with open_image(img_path) as img:
        vols = [img.get_volume(vol_name)] if vol_name else img.volumes
        for v in vols:
            try:
                vol = v.mount()
            except (FSError, BlockDeviceError) as ex:
                print("%s: cannot mount: %s" % (v.name, ex))
                rc = 1
                continue
            rep = vol.check(deep=args.deep)
            status = "OK" if rep["ok"] else "ERRORS"
            used = (
                "%d blocks used" % rep["used_blocks"]
                if rep.get("used_blocks") is not None
                else "structure only"
            )
            print(
                "%s (%s): %s  -- %d files, %d dirs, %s"
                % (v.name, vol.label, status, rep["files"], rep["dirs"], used)
            )
            for w in rep["warnings"]:
                print("  warning: %s" % w)
            for e in rep["errors"]:
                print("  error: %s" % e)
            if not rep["ok"]:
                rc = 1
    return rc


# ---------------------------------------------------------------- create / format
DOS_TYPES = {  # accepted --dostype spellings
    "ofs": 0x444F5300, "dos0": 0x444F5300,
    "ffs": 0x444F5301, "dos1": 0x444F5301,
    "ofs-intl": 0x444F5302, "dos2": 0x444F5302,
    "ffs-intl": 0x444F5303, "dos3": 0x444F5303,
    "ofs-dc": 0x444F5304, "dos4": 0x444F5304,
    "ffs-dc": 0x444F5305, "dos5": 0x444F5305,
    "ofs-intl-lnfs": 0x444F5306, "dos6": 0x444F5306,
    "ffs-intl-lnfs": 0x444F5307, "dos7": 0x444F5307,
    "sfs": 0x53465300, "sfs0": 0x53465300,
    "sfs2": 0x53465302,
    "pfs0": 0x50465300,
    "pfs1": 0x50465301,
    "pfs2": 0x50465302,
    "pfs3": 0x50465303, "pds3": 0x50445303,
    "pfs3-modern": 0x50465333,
    "jxfs": 0x4a584604,
    "swap": 0x53574150,
}


def parse_size(text):
    text = text.strip().lower()
    mult = 1
    for suffix, m in (("k", 1024), ("m", 1024**2), ("g", 1024**3)):
        if text.endswith(suffix):
            mult = m
            text = text[:-1]
            break
    return int(float(text) * mult)


def cmd_create(args):
    if os.path.exists(args.image) and not args.force:
        print("error: %s exists (use --force)" % args.image, file=sys.stderr)
        return 1
    if args.adf:
        size = 901120
    else:
        size = parse_size(args.size)
        size -= size % 512
        if size < 512 * 1024:
            print("error: minimum image size is 512K", file=sys.stderr)
            return 1
    with open(args.image, "wb") as fh:
        fh.truncate(size)  # sparse where the host filesystem supports it
    print("created %s (%s)" % (args.image, human_size(size)))
    
    if getattr(args, "layout", None):
        layout_str = args.layout
        if layout_str.lower() == "default":
            layout_str = "DH0:*:ffs-intl:Empty:boot"
            
        from .rdb import RDisk
        from .blkdev import open_blkdev
        
        # 1. Initialize RDB
        dev = open_blkdev(args.image, read_only=False)
        rd = RDisk.create(dev)
        i = rd.get_info()
        print("initialized RDB: %s partitionable" % human_size((i["hi_cylinder"] - i["lo_cylinder"] + 1) * i["cyl_blocks"] * i["block_bytes"]))
        dev.close()
        
        # 2. Add partitions
        parts = layout_str.split(",")
        for p in parts:
            if not p.strip(): continue
            fields = p.split(":")
            
            name = fields[0]
            size_str = fields[1] if len(fields) > 1 else "*"
            dostype_str = fields[2] if len(fields) > 2 else "ffs-intl"
            label = fields[3] if len(fields) > 3 else None
            bootable = False
            
            if len(fields) > 4 and fields[4].lower() == "boot":
                bootable = True
            elif label and label.lower() == "boot":
                bootable = True
                label = None
                
            if label == "":
                label = None
                
            size_bytes = None
            if size_str != "*" and size_str.lower() != "auto":
                size_bytes = parse_size(size_str)
                
            # Add partition via cmd_part_add logic natively
            with open_image(args.image, writable=True) as img:
                dos_type = _parse_dostype(dostype_str)
                part = img.rdisk.add_partition(
                    name,
                    size_bytes=size_bytes,
                    dos_type=dos_type,
                    sec_per_blk=1,
                    bootable=bootable,
                    boot_pri=0,
                )
                pi = part.get_info()
                print("added %s: %s, %s" % (pi["drv_name"], human_size(pi["size_bytes"]), pi["dos_type"]))
            
            # Format partition if requested
            if label:
                f_args = argparse.Namespace(
                    image=args.image, volume=name, label=label, dostype=None
                )
                cmd_format(f_args)
        return 0

    if args.format:
        args.volume = None
        args.label = args.format
        args.dostype = args.dostype or ("ofs" if args.adf else "ffs-intl")
        return cmd_format(args)
    return 0


def cmd_bootblock(args):
    img_path, vol_name = _parse_image_arg(args.image, getattr(args, "volume", None))
    with open_image(img_path, writable=True) as img:
        vol_ref = img.get_volume(vol_name)
        
        with open(args.bootcode, "rb") as f:
            bootcode = f.read()
            
        vol = vol_ref.raw_volume()
            
        # Standard Amiga bootblock is 2 blocks (1024 bytes for 512b sectors)
        bb_size = vol.dev.block_bytes * 2
        if len(bootcode) > bb_size:
            print("error: bootcode is too large (%d > %d bytes)" % (len(bootcode), bb_size), file=sys.stderr)
            return 1
            
        # Pad with zeros
        padded = bootcode.ljust(bb_size, b'\x00')
        
        # Check if the partition already has a DOS type, if the bootcode lacks DOS\x we might warn.
        vol.dev.write(0, padded)
        
        print("installed bootblock (%d bytes) to %s" % (len(bootcode), vol_ref.name))
    return 0


def cmd_format(args):
    img_path, vol_name = _parse_image_arg(args.image, getattr(args, "volume", None))
    with open_image(img_path, writable=True) as img:
        vol_ref = img.get_volume(vol_name)
        
        dos_type = None
        if getattr(args, "dostype", None):
            dt = args.dostype
            dos_type = DOS_TYPES.get(dt.lower())
            if dos_type is None:
                try:
                    dos_type = int(dt, 0)
                except ValueError:
                    print("error: unknown dostype %r (use: %s)"
                          % (dt, ", ".join(sorted(DOS_TYPES))), file=sys.stderr)
                    return 1
        else:
            if vol_ref.partition is not None:
                dos_type = vol_ref.partition.dos_env.dos_type
            else:
                dos_type = DOS_TYPES["ffs-intl"]

        from .image import engine_for_dostype
        vol = vol_ref.raw_volume(engine_for_dostype(dos_type))
        vol.format(args.label.encode("latin-1"), dos_type=dos_type)
        info = vol.open().get_info()
        print("formatted %s as %r (%s, %s free)" % (
            vol_ref.name, info["label"], info["dos_type"],
            human_size(info["free_bytes"])))
    return 0


# ---------------------------------------------------------------- rdb commands
def _parse_dostype(text, default=0x444F5303):
    if not text:
        return default
    dt = DOS_TYPES.get(text.lower())
    if dt is not None:
        return dt
    return int(text, 0)


def cmd_rdb_init(args):
    from .rdb import RDisk
    from .blkdev import open_blkdev

    dev = open_blkdev(args.image, read_only=False)
    try:
        if RDisk.peek(dev) is not None and not args.force:
            print("error: image already has an RDB (use --force)", file=sys.stderr)
            return 1
        rd = RDisk.create(
            dev, sectors=args.sectors, heads=args.heads, rdb_cyls=args.rdb_cyls
        )
        i = rd.get_info()
        print(
            "initialized RDB: %d cylinders x %d blocks (%s partitionable)"
            % (
                i["cylinders"], i["cyl_blocks"],
                human_size((i["hi_cylinder"] - i["lo_cylinder"] + 1)
                           * i["cyl_blocks"] * i["block_bytes"]),
            )
        )
    finally:
        dev.close()
    return 0


def cmd_part_add(args):
    with open_image(args.image, writable=True) as img:
        if not img.rdisk:
            print("error: image has no RDB (run rdb-init first)", file=sys.stderr)
            return 1
        dos_type = _parse_dostype(args.dostype)
        bs = args.bs
        if bs % 512 or bs > 32768:
            print("error: block size must be a multiple of 512", file=sys.stderr)
            return 1
        part = img.rdisk.add_partition(
            args.name,
            size_bytes=parse_size(args.size) if args.size else None,
            dos_type=dos_type,
            sec_per_blk=bs // 512,
            bootable=args.bootable,
            boot_pri=args.pri,
        )
        i = part.get_info()
        print(
            "added %s: cyl %d-%d, %s, %s bs=%d"
            % (
                i["drv_name"], i["low_cyl"], i["high_cyl"],
                human_size(i["size_bytes"]), i["dos_type"], i["block_size"],
            )
        )
        # a dostype the ROM cannot serve needs its handler embedded in the
        # RDB; pull one from the driver collection automatically
        if (dos_type >> 8) != 0x444F53 and not getattr(args, "no_auto_fs", False):
            have = any(f.fshd_blk.dos_type == dos_type
                       for f in img.rdisk.get_filesystems())
            if not have:
                found = _collection_lookup(dos_type)
                if found:
                    dpath, ver, dname = found
                    with open(dpath, "rb") as fh:
                        ddata = fh.read()
                    try:
                        fs = _embed_driver(img.rdisk, ddata, dos_type, ver)
                        print("embedded %s driver %s v%s from collection"
                              % (fs.get_dos_type_str(), dname,
                                 fs.get_version_string()))
                    except Exception as ex:
                        print("warning: could not embed driver: %s" % ex,
                              file=sys.stderr)
                else:
                    print("note: no %s driver in the RDB or collection -- the "
                          "partition will not boot on a real machine "
                          "(use fs-add)" % i["dos_type"])
        if args.format:
            args.volume = args.name
            args.label = args.format
            args.dostype = None
            img.close()
            # reopen cleanly for the format path
            f_args = argparse.Namespace(
                image=args.image, volume=args.name, label=args.format,
                dostype="0x%08X" % dos_type,
            )
            return cmd_format(f_args)
    return 0


def cmd_part_del(args):
    from .blkdev import open_blkdev, OverlayBlockDevice
    from .image import DiskImage

    base = open_blkdev(args.image)
    try:
        overlay = OverlayBlockDevice(base)
        sim = DiskImage(args.image, writable=True, blkdev=overlay)
        if not sim.rdisk:
            print("error: image has no RDB", file=sys.stderr)
            return 1
        target = sim.rdisk.find_partition_by_drive_name(args.name)
        if target is None:
            print("error: no partition %r" % args.name, file=sys.stderr)
            return 1
        i = target.get_info()
        print(
            "will delete %s: cyl %d-%d, %s, %s (partition entry only; "
            "data blocks stay on disk and rdb-scan can recover them)"
            % (i["drv_name"], i["low_cyl"], i["high_cyl"],
               human_size(i["size_bytes"]), i["dos_type"])
        )
        sim.rdisk.delete_partition(args.name)
        sim2 = DiskImage(args.image, writable=True, blkdev=overlay)
        print("simulated result -- remaining volumes:")
        lines, all_ok = _volume_report(sim2, list_root=False)
        for ln in lines:
            print(ln)
        if not lines:
            print("  (none)")
        if not all_ok:
            print("simulation shows problems -- NOT safe to write", file=sys.stderr)
            return 1
        if not args.write:
            print("dry run complete. Use --write to apply.")
            return 0
        return 0 if _commit_overlay(overlay, args.image) else 1
    finally:
        base.close()


# ---------------------------------------------------------------- simulation
def _volume_report(img, list_root=True, deep=False):
    """Inspect every volume of a (possibly simulated) image; returns
    (lines, all_ok). Shows label, fill/free, check result and a sample of
    the root catalog -- the evidence a user needs before trusting a fix."""
    lines = []
    all_ok = True
    for v in img.volumes:
        try:
            vol = v.mount()
            info = vol.get_info()
        except (FSError, BlockDeviceError) as ex:
            lines.append("  %-8s UNMOUNTABLE: %s" % (v.name + ":", ex))
            all_ok = False
            continue
        total_b = info["total_blocks"] * info["block_size"]
        used_b = info.get("used_blocks", 0) * info["block_size"]
        rep = vol.check(deep=deep)
        ok = "check OK" if rep["ok"] else "CHECK FAILED (%d errors)" % len(rep["errors"])
        all_ok &= rep["ok"]
        lines.append(
            "  %-8s %-16r %-7s %9s used / %9s free of %9s -- %d files, %d dirs -- %s"
            % (
                v.name + ":", info["label"], info["dos_type"],
                human_size(used_b), human_size(info["free_bytes"]),
                human_size(total_b), rep["files"], rep["dirs"], ok,
            )
        )
        for e in rep["errors"][:3]:
            lines.append("      error: %s" % e)
        if list_root:
            try:
                entries = vol.list_dir("")
                names = [
                    e.name_str() + ("/" if e.is_dir() else "") for e in entries
                ]
                sample = " ".join(names[:10]) + (" ..." if len(names) > 10 else "")
                lines.append("      root (%d entries): %s" % (len(names), sample))
            except (FSError, BlockDeviceError) as ex:
                lines.append("      root unreadable: %s" % ex)
                all_ok = False
    return lines, all_ok


def _commit_overlay(overlay, path, backup_path=None, backup_secs=2048):
    """Apply a verified overlay to the real image (with RDB-area backup
    and read-back verification)."""
    from .blkdev import open_blkdev

    target = open_blkdev(path, read_only=False)
    try:
        if backup_path:
            area = min(target.num_blocks, backup_secs)
            with open(backup_path, "wb") as fh:
                fh.write(target.read(0, area))
            print("backup of first %d sectors saved to %s" % (area, backup_path))
        overlay.commit(target)
        bad = overlay.verify_committed(target)
        if bad:
            print(
                "error: read-back verification FAILED on %d blocks!" % len(bad),
                file=sys.stderr,
            )
            return False
        print(
            "committed %s in %d changed blocks, read-back verified"
            % (human_size(overlay.dirty_bytes()), len(overlay.dirty_blocks()))
        )
        return True
    finally:
        target.close()


# ---------------------------------------------------------------- rdb rescue
def cmd_rdb_scan(args):
    from .blkdev import open_blkdev
    from .rdb import rescue, RDisk

    dev = open_blkdev(args.image)
    try:
        if RDisk.peek(dev) is not None:
            print("note: image still has a valid RDB; scan continues anyway")

        def progress(done, total):
            if args.progress:
                pct = done * 100 // total
                print("\r  scanning... %d%%" % pct, end="", file=sys.stderr)

        cands, notes = rescue.scan(dev, progress=progress)
        if args.progress:
            print("", file=sys.stderr)
        for n in notes:
            print("note: %s" % n)
        if not cands:
            return 1
        print("recovered partition candidates:")
        for c in cands:
            print("  " + c.describe())
        print("run 'amidisk rdb-rebuild %s --write' to rebuild the RDB" % args.image)
    finally:
        dev.close()
    return 0


def cmd_rdb_rebuild(args):
    from .blkdev import open_blkdev, OverlayBlockDevice
    from .rdb import rescue
    from .image import DiskImage

    base = open_blkdev(args.image)  # base is never written directly
    try:
        # state before the fix
        before = DiskImage(args.image, blkdev=base)
        print("before: image kind '%s', %d mountable volume(s)" % (
            before.kind,
            sum(1 for v in before.volumes if v.label()),
        ))

        cands, notes = rescue.scan(base)
        for n in notes:
            print("note: %s" % n)
        if not cands:
            print("error: no partitions found to rebuild", file=sys.stderr)
            return 1
        print("evidence found:")
        for c in cands:
            print("  " + c.describe())

        # simulate the full rebuild on a RAM overlay and inspect the result
        overlay = OverlayBlockDevice(base)
        rescue.rebuild(overlay, cands, backup_path=None)
        sim = DiskImage(args.image, writable=True, blkdev=overlay)
        print("simulated result (%s of changes, nothing written yet):"
              % human_size(overlay.dirty_bytes()))
        lines, all_ok = _volume_report(sim, deep=args.deep)
        for ln in lines:
            print(ln)
        if not all_ok:
            print("simulation shows problems -- NOT safe to write", file=sys.stderr)
            return 1
        if not args.write:
            print("dry run complete: all volumes verified in simulation. "
                  "Use --write to apply.")
            return 0
        backup = args.backup or (args.image + ".rdb-backup.bin")
        if not _commit_overlay(overlay, args.image, backup_path=backup):
            return 1
        # final proof on the real image
        with open_image(args.image) as img:
            lines, all_ok = _volume_report(img, list_root=False)
            print("post-commit verification:")
            for ln in lines:
                print(ln)
        return 0 if all_ok else 1
    finally:
        base.close()


# ---------------------------------------------------------------- repair
def cmd_repair(args):
    from .blkdev import open_blkdev, OverlayBlockDevice
    from .image import DiskImage

    img_path, vol_name = _parse_image_arg(args.image, getattr(args, "volume", None))
    rc = 0
    base = open_blkdev(img_path)
    try:
        overlay = OverlayBlockDevice(base)
        sim = DiskImage(img_path, writable=True, blkdev=overlay)
        vols = [sim.get_volume(vol_name)] if vol_name else sim.volumes
        touched = False
        for v in vols:
            try:
                vol = v.mount()
            except (FSError, BlockDeviceError) as ex:
                print("%s: cannot mount: %s" % (v.name, ex))
                rc = 1
                continue
            if not hasattr(vol, "repair"):
                print(
                    "%s (%s): %s volumes are read-only here -- use the original "
                    "handler (or SFSsalv/pfsDoctor) to repair"
                    % (v.name, vol.label, vol.get_info().get("filesystem", "?"))
                )
                continue
            before = vol.check()
            free_before = vol.get_info()["free_bytes"]
            rep = vol.repair(apply=True)  # applies to the overlay only
            after = vol.check(deep=args.deep)
            free_after = vol.get_info()["free_bytes"]
            touched = touched or bool(rep["alloc_missing"] or rep["lost_blocks"])
            print(
                "%s (%s): %d blocks in tree, %d in-use-but-free fixed, "
                "%d lost blocks reclaimed"
                % (v.name, vol.label, rep["used_blocks"],
                   rep["alloc_missing"], rep["lost_blocks"])
            )
            print(
                "  check: %d errors, %d warnings before -> %d errors, %d warnings "
                "after (simulated); free space %s -> %s"
                % (
                    len(before["errors"]), len(before["warnings"]),
                    len(after["errors"]), len(after["warnings"]),
                    human_size(free_before), human_size(free_after),
                )
            )
            for p in rep["problems"]:
                print("  unreadable: %s" % p)
                rc = 1
            if not after["ok"]:
                print("  simulation still has errors -- NOT safe to write")
                rc = 1
        if rc == 0 and touched and args.write:
            if not _commit_overlay(overlay, args.image):
                return 1
        elif touched and not args.write:
            print(
                "dry run complete (%s of changes simulated and verified). "
                "Use --write to apply." % human_size(overlay.dirty_bytes())
            )
        elif not touched:
            print("nothing to repair")
    finally:
        base.close()
    return rc


# ---------------------------------------------------------------- fs-add
def _driver_collection():
    """Locate the driver collection (manifest.json + hunk binaries)."""
    for cand in (os.environ.get("AMIDISK_DRIVERS"),
                 os.path.join("data", "drivers")):
        if cand and os.path.isfile(os.path.join(cand, "manifest.json")):
            return cand
    return None


def _collection_lookup(dos_type):
    """Find a driver for dos_type in the collection; returns
    (path, version_tuple, name) or None."""
    coll = _driver_collection()
    if not coll:
        return None
    with open(os.path.join(coll, "manifest.json")) as fh:
        manifest = json.load(fh)
    for d in manifest.get("drivers", []):
        if any(int(t, 0) == dos_type for t in d.get("dostypes", [])):
            ver = tuple(int(x) for x in d.get("version", "0.0").split("."))[:2]
            return os.path.join(coll, d["file"]), ver, d.get("name", d["file"])
    return None


def _parse_hunk_version(data):
    """Extract (version, revision) from a $VER: string, if present."""
    import re

    i = data.find(b"$VER:")
    if i < 0:
        return None
    m = re.search(rb"(\d+)\.(\d+)", data[i : i + 128])
    return (int(m.group(1)), int(m.group(2))) if m else None


def _embed_driver(rdisk, data, dos_type, version=None, patch_flags=0x180):
    if data[0:4] != b"\x00\x00\x03\xf3":
        raise FSError("not an Amiga hunk executable (missing 0x3F3 magic)")
    ver = version or _parse_hunk_version(data) or (0, 0)
    fs = rdisk.add_filesystem(data, dos_type, version=ver,
                              patch_flags=patch_flags)
    return fs


def cmd_fs_add(args):
    from .rdb.rdisk import RDiskError

    with open_image(args.image, writable=True) as img:
        if not img.rdisk:
            print("error: image has no RDB (run rdb-init first)", file=sys.stderr)
            return 1
        dos_type = _parse_dostype(args.dostype, default=None)
        if dos_type is None:
            print("error: --dostype is required", file=sys.stderr)
            return 1
        with open(args.driver, "rb") as fh:
            data = fh.read()
        version = None
        if args.version:
            version = tuple(int(x) for x in args.version.split("."))[:2]
        try:
            fs = _embed_driver(img.rdisk, data, dos_type, version,
                               int(args.patch_flags, 0))
        except (RDiskError, FSError) as ex:
            print("error: %s" % ex, file=sys.stderr)
            return 1
        print("embedded %s v%s (%d bytes, %d LSEG blocks)" % (
            fs.get_dos_type_str(), fs.get_version_string(), len(data),
            (len(data) + 491) // 492))
    return 0


# ---------------------------------------------------------------- fs-extract
def cmd_fs_extract(args):
    with open_image(args.image) as img:
        if not img.rdisk:
            print("error: image has no RDB", file=sys.stderr)
            return 1
        fss = img.rdisk.get_filesystems()
        if not fss:
            print("error: RDB carries no embedded filesystems", file=sys.stderr)
            return 1
        for fs in fss:
            if args.num is not None and fs.num != args.num:
                continue
            data = fs.get_data()
            name = args.out or "%s_v%s.fs" % (
                fs.get_dos_type_str().replace("\\", "_"), fs.get_version_string()
            )
            with open(name, "wb") as fh:
                fh.write(data)
            print("wrote %s (%d bytes, %s v%s)" % (
                name, len(data), fs.get_dos_type_str(), fs.get_version_string()))
    return 0


def cmd_bench(args):
    img_path, vol_name = _parse_image_arg(args.image)
    with open_image(img_path) as img:
        vol_ref, path = img.parse_path(_combine_path(vol_name, args.path))
        vol = vol_ref.mount()
        
        start_time = time.time()
        total_bytes = 0
        file_count = 0
        
        print(f"Benchmarking read on {vol.get_info()['label']}...")
        
        for prefix, e in vol.walk(path):
            if e.is_file():
                if args.filter and args.filter not in e.name_str():
                    continue
                    
                if args.limit_files and file_count >= args.limit_files:
                    break
                    
                try:
                    for chunk in vol.read_file(e):
                        total_bytes += len(chunk)
                        if args.limit_bytes and total_bytes >= args.limit_bytes:
                            break
                except Exception as exc:
                    print(f"error reading {prefix}/{e.name_str()}: {exc}", file=sys.stderr)
                    
                file_count += 1
                if args.limit_bytes and total_bytes >= args.limit_bytes:
                    break
                    
        elapsed = time.time() - start_time
        print(f"Read {file_count} files, {total_bytes} bytes in {elapsed:.4f} seconds.")
        if elapsed > 0:
            print(f"Throughput: {total_bytes / elapsed / 1024 / 1024:.2f} MB/s")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="amidisk",
        description="Read/write Amiga HDF/VHD/ADF disk images (RDB + OFS/FFS).",
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="show image, RDB and volume overview")
    p.add_argument("image")
    p.add_argument("--json", action="store_true")
    p.add_argument("--deep", action="store_true", help="scan all directories to count files/dirs")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("bench", help="benchmark file read operations")
    p.add_argument("image")
    p.add_argument("path", nargs="?", default="")
    p.add_argument("--limit-files", type=int, help="max files to read")
    p.add_argument("--limit-bytes", type=int, help="max bytes to read")
    p.add_argument("--filter", help="substring filter for file names")
    p.set_defaults(func=cmd_bench)

    p = sub.add_parser("ls", help="list directory contents")
    p.add_argument("image", help="image.hdf:Volume[/path]")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("cat", help="output file contents to stdout")
    p.add_argument("image", help="image.hdf:Volume/path")
    p.set_defaults(func=cmd_cat)

    p = sub.add_parser("extract", help="extract files to host filesystem")
    p.add_argument("image", help="image.hdf:Volume/path")
    p.add_argument("dest", nargs="?", help="host destination path (optional)")
    p.add_argument("-r", "--recursive", action="store_true", help="extract directories recursively")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("put", help="copy a host file/dir into the image")
    p.add_argument("image")
    p.add_argument("src")
    p.add_argument("dest", nargs="?", help="optional if SCP-style syntax used")
    p.add_argument("--protect", help="e.g. 'hsparwed' subset like '----rwed'")
    p.add_argument("--comment", help="file comment")
    p.add_argument("--bulk", action="store_true",
                   help="batch metadata commits for mass copies (faster; "
                        "a crash mid-run may need `repair --write`)")
    p.set_defaults(func=cmd_put)

    p = sub.add_parser("cp", help="copy files (host <-> image or image <-> image)")
    p.add_argument("--bulk", action="store_true",
                   help="batch metadata commits for mass copies (faster; "
                        "a crash mid-run may need `repair --write`)")
    p.add_argument("src", help="source host path or image.hdf:Volume/path")
    p.add_argument("dst", help="destination host path or image.hdf:Volume/path")
    p.add_argument("-r", "--recursive", action="store_true")
    p.add_argument("--checksum", action="store_true", help="calculate MD5 checksum while copying")
    p.add_argument("--comment", help="file comment (host -> image only)")
    p.add_argument("--protect", help="e.g. 'hsparwed' subset like '----rwed' (host -> image only)")
    p.set_defaults(func=cmd_cp)

    p = sub.add_parser("mkdir", help="create a directory")
    p.add_argument("image", help="image.hdf:Volume/path")
    p.add_argument("-p", "--parents", action="store_true")
    p.set_defaults(func=cmd_mkdir)

    p = sub.add_parser("rm", help="remove files or directories")
    p.add_argument("image", help="image.hdf:Volume/path")
    p.add_argument("-r", "--recursive", action="store_true")
    p.add_argument("-f", "--force", action="store_true")
    p.set_defaults(func=cmd_rm)

    p = sub.add_parser("mv", help="rename a file or directory")
    p.add_argument("src", help="image.hdf:Volume/src_path")
    p.add_argument("dst", help="image.hdf:Volume/dst_path (or just dst_path if same volume)")
    p.set_defaults(func=cmd_mv)

    p = sub.add_parser("check", help="verify filesystem structures and bitmap")
    p.add_argument("image")
    p.add_argument("volume", nargs="?")
    p.add_argument("--deep", action="store_true", help="also verify OFS data blocks")
    p.set_defaults(func=cmd_check)

    p = sub.add_parser("rdb-init", help="write a fresh RDB partition table")
    p.add_argument("image")
    p.add_argument("--sectors", type=int, default=63)
    p.add_argument("--heads", type=int)
    p.add_argument("--rdb-cyls", type=int, default=None)
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_rdb_init)

    p = sub.add_parser("part-add", help="add a partition to the RDB")
    p.add_argument("image")
    p.add_argument("name", help="drive name, e.g. DH0")
    p.add_argument("--size", help="e.g. 500M (default: all remaining space)")
    p.add_argument("--dostype", help="ofs|ffs|ffs-intl|... or a raw hex 32-bit identifier (e.g. 0x444f5303 for DOS\\3)")
    p.add_argument("--bs", type=int, default=512, help="fs block size (512..32768)")
    p.add_argument("--bootable", action="store_true")
    p.add_argument("--pri", type=int, default=0, help="boot priority")
    p.add_argument("--format", metavar="LABEL", help="format the new partition")
    p.add_argument("--no-auto-fs", action="store_true",
                   help="do not auto-embed a driver from the collection")
    p.set_defaults(func=cmd_part_add)

    p = sub.add_parser(
        "fs-add",
        help="embed a filesystem driver (hunk binary) into the RDB (FSHD+LSEG)",
    )
    p.add_argument("image")
    p.add_argument("driver", help="path to the driver, e.g. pfs3aio")
    p.add_argument("--dostype", required=True, help="pfs3|sfs|... or a raw hex 32-bit identifier (e.g. 0x50465303 for PFS\\3)")
    p.add_argument("--version", help="M.R override (default: parse $VER)")
    p.add_argument("--patch-flags", default="0x180")
    p.set_defaults(func=cmd_fs_add)

    p = sub.add_parser("part-del", help="remove a partition from the RDB (simulates first)")
    p.add_argument("image")
    p.add_argument("name")
    p.add_argument("--write", action="store_true", help="apply (default: dry run)")
    p.set_defaults(func=cmd_part_del)

    p = sub.add_parser("create", help="create a new blank image (HDF or ADF)")
    p.add_argument("image")
    p.add_argument("--size", default="10M", help="image size, e.g. 100M, 1G")
    p.add_argument("--adf", action="store_true", help="880K floppy image")
    p.add_argument("--format", metavar="LABEL", help="format after creating")
    p.add_argument("--dostype", help="ofs|ffs|ofs-intl|ffs-intl or a raw hex 32-bit identifier (e.g. 0x444f5303 for DOS\\3)")
    p.add_argument("--layout", help="Init RDB (e.g. DH0:*:ffs:System:boot) or 'default'")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_create)

    p = sub.add_parser("bootblock", help="install a custom bootblock to a partition")
    p.add_argument("image", help="e.g. disk.hdf:DH0")
    p.add_argument("bootcode", help="path to bootblock binary")
    p.add_argument("--volume", help="partition to install to (drive name/index)")
    p.set_defaults(func=cmd_bootblock)

    p = sub.add_parser("format", help="format a volume (OFS/FFS)")
    p.add_argument("image")
    p.add_argument("label")
    p.add_argument("--volume", help="partition to format (drive name/index)")
    p.add_argument("--dostype", help="ofs|ffs|ofs-intl|ffs-intl or a raw hex 32-bit identifier (e.g. 0x444f5303 for DOS\\3)")
    p.set_defaults(func=cmd_format)

    p = sub.add_parser("rdb-scan", help="scan a disk for lost partitions (overwritten RDB)")
    p.add_argument("image")
    p.add_argument("--progress", action="store_true")
    p.set_defaults(func=cmd_rdb_scan)

    p = sub.add_parser(
        "rdb-rebuild",
        help="rebuild an overwritten RDB from found partitions (simulates + verifies first)",
    )
    p.add_argument("image")
    p.add_argument("--write", action="store_true", help="apply (default: dry run)")
    p.add_argument("--backup", help="backup file for the old RDB area")
    p.add_argument("--deep", action="store_true", help="read every file during verification")
    p.set_defaults(func=cmd_rdb_rebuild)

    p = sub.add_parser(
        "repair",
        help="rebuild the FFS allocation bitmap from the tree (simulates + verifies first)",
    )
    p.add_argument("image")
    p.add_argument("volume", nargs="?")
    p.add_argument("--write", action="store_true", help="apply fixes (default: dry run)")
    p.add_argument("--deep", action="store_true", help="read every file during verification")
    p.set_defaults(func=cmd_repair)

    p = sub.add_parser("fs-extract", help="extract embedded RDB filesystem drivers")
    p.add_argument("image")
    p.add_argument("--num", type=int)
    p.add_argument("--out")
    p.set_defaults(func=cmd_fs_extract)

    p = sub.add_parser("sync", help="incrementally synchronize directories (rsync-style)")
    p.add_argument("src", help="source host path or image.hdf:Volume/path")
    p.add_argument("dst", help="destination host path or image.hdf:Volume/path")
    p.add_argument("--delete", action="store_true", help="delete extraneous files from destination")
    p.add_argument("-n", "--dry-run", action="store_true", help="perform a trial run with no changes made")
    p.add_argument("-u", "--update", action="store_true", help="skip files that are newer on destination")
    p.add_argument("--checksum", action="store_true", help="compare MD5 checksums instead of size/mtime")
    p.add_argument("--bulk", action="store_true", help="batch metadata commits for image writes (faster)")
    p.set_defaults(func=cmd_sync)

    p = sub.add_parser("help", help="show detailed help for a specific command (or 'all')")
    p.add_argument("command", nargs="?", help="command name, or 'all' for every command")
    
    def run_help(args):
        if args.command == "all":
            ap.print_help()
            for name, parser in sub.choices.items():
                if name != "help":
                    print(f"\n{'='*60}\nCommand: {name}\n{'='*60}")
                    parser.print_help()
            return 0
        elif args.command:
            if args.command in sub.choices:
                sub.choices[args.command].print_help()
                return 0
            else:
                print(f"error: unknown command '{args.command}'", file=sys.stderr)
                return 1
        else:
            ap.print_help()
            return 0
            
    p.set_defaults(func=run_help)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except (ImageError, FSError, BlockDeviceError) as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 1
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
