import os
import sys
import shutil
import time
import hashlib
import argparse
from datetime import datetime

from .image import open_image, ImageError
from .fs.ffs import FSError
from .blkdev import BlockDeviceError

def get_md5_host(path):
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

def run_sync(args):
    # Import CLI utilities dynamically to avoid circular dependencies
    from .cli import _parse_image_arg, _combine_path, print_progress, print_transfer_stats, human_size
    
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

def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="amidisk-sync",
        description="Incrementally synchronize directories and files to/from/between Amiga disk images (rsync-style).",
    )
    ap.add_argument("src", help="source host path or image.hdf:Volume/path")
    ap.add_argument("dst", help="destination host path or image.hdf:Volume/path")
    ap.add_argument("--delete", action="store_true", help="delete extraneous files from destination")
    ap.add_argument("-n", "--dry-run", action="store_true", help="perform a trial run with no changes made")
    ap.add_argument("-u", "--update", action="store_true", help="skip files that are newer on destination")
    ap.add_argument("--checksum", action="store_true", help="compare MD5 checksums instead of size/mtime")
    ap.add_argument("--bulk", action="store_true", help="batch metadata commits for image writes (faster)")
    
    args = ap.parse_args(argv)
    try:
        return run_sync(args)
    except (ImageError, FSError, BlockDeviceError) as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
