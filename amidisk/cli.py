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


def human_size(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0


# ---------------------------------------------------------------- info
def cmd_info(args):
    with open_image(args.image) as img:
        info = img.get_info()
        if args.json:
            print(json.dumps(info, indent=2))
            return 0
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
            print("partitions:")
            for p in info["partitions"]:
                print(
                    "  %d: %-8s %-7s cyl %6d-%6d  bs=%-5d %10s  boot=%s pri=%d"
                    % (
                        p["num"], p["drv_name"], p["dos_type"],
                        p["low_cyl"], p["high_cyl"], p["block_size"],
                        human_size(p["size_bytes"]), "yes" if p["bootable"] else "no ",
                        p["boot_pri"],
                    )
                )
            if info["filesystems"]:
                print("embedded filesystems:")
                for f in info["filesystems"]:
                    print(
                        "  %d: %-7s v%-8s patch_flags=0x%x"
                        % (f["num"], f["dos_type"], f["version"], f["patch_flags"])
                    )
        print("volumes:")
        for v in img.volumes:
            try:
                vi = v.mount().get_info()
                print(
                    "  %-8s %-16r %-7s %10s free of %s"
                    % (
                        v.name + ":", vi["label"], vi["dos_type"],
                        human_size(vi["free_bytes"]),
                        human_size(vi["total_blocks"] * vi["block_size"]),
                    )
                )
            except (FSError, BlockDeviceError) as ex:
                print("  %-8s <unmountable: %s>" % (v.name + ":", ex))
    return 0


# ---------------------------------------------------------------- ls
def cmd_ls(args):
    with open_image(args.image) as img:
        vol_ref, path = img.parse_path(args.path or "")
        vol = vol_ref.mount()
        entry = vol.resolve(path)
        entries = (
            vol.list_dir(path) if entry.is_dir() else [entry]
        )
        if args.json:
            print(json.dumps([e.get_info() for e in entries], indent=2))
            return 0
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


# ---------------------------------------------------------------- cat
def cmd_cat(args):
    with open_image(args.image) as img:
        vol_ref, path = img.parse_path(args.path)
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
    with open_image(args.image) as img:
        vol_ref, path = img.parse_path(args.path)
        vol = vol_ref.mount()
        entry = vol.resolve(path)
        dest = args.dest
        if entry.is_file():
            if os.path.isdir(dest):
                dest = os.path.join(dest, _safe_name(entry.name_str()))
            _extract_file(vol, path, entry, dest)
            print("extracted %s (%d bytes)" % (dest, entry.size))
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
        for prefix, e in vol.walk(path):
            rel = prefix[len(base_prefix):].lstrip("/") if base_prefix else prefix
            host_dir = os.path.join(dest, *[_safe_name(p) for p in rel.split("/") if p])
            if e.sec_type == 2:  # dir
                os.makedirs(os.path.join(host_dir, _safe_name(e.name_str())), exist_ok=True)
            elif e.is_file():
                os.makedirs(host_dir, exist_ok=True)
                _extract_file(vol, None, e, os.path.join(host_dir, _safe_name(e.name_str())))
                count += 1
            elif e.is_link():
                print("skipping link: %s/%s" % (prefix, e.name_str()), file=sys.stderr)
        print("extracted %d files to %s" % (count, dest))
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
    with open_image(args.src_image) as src_img, open_image(args.dst_image, writable=True) as dst_img:
        src_vol_ref, src_path = src_img.parse_path(args.src_path)
        dst_vol_ref, dst_path = dst_img.parse_path(args.dst_path)
        
        src_vol = src_vol_ref.mount()
        dst_vol = dst_vol_ref.mount()
        
        start_time = time.time()
        total_bytes = 0
        file_count = 0
        
        entry = src_vol.resolve(src_path)
        if entry.is_file():
            try:
                if dst_vol.resolve(dst_path).is_dir():
                    dst_path = dst_path.rstrip("/") + "/" + entry.name_str()
            except FSError:
                pass
            
            stream = src_vol.read_file(entry)
            chk_stream = ChecksumStream(stream) if args.checksum else stream
            
            dst_vol.write_file(dst_path, chk_stream, size=entry.size, protect=entry.protect, mtime=entry.mtime())
            total_bytes += entry.size
            file_count += 1
            if args.checksum:
                print(f"copied {dst_path} (md5: {chk_stream.md5.hexdigest()})")
            else:
                print(f"copied {dst_path}")
            return 0
            
        if not args.recursive:
            print(f"error: {src_path} is a directory (use -r)", file=sys.stderr)
            return 1
            
        base_prefix = "/".join(p.decode("latin-1") for p in src_vol._split(src_path))
        base_dest = dst_path.rstrip("/")
        
        for prefix, e in src_vol.walk(src_path):
            rel = prefix[len(base_prefix):].lstrip("/") if base_prefix else prefix
            amiga_dir = base_dest + ("/" + rel if rel else "") if base_dest else rel
            
            if e.is_dir():
                dst_dir = amiga_dir + "/" + e.name_str() if amiga_dir else e.name_str()
                dst_vol.makedirs(dst_dir)
            elif e.is_file():
                if amiga_dir:
                    dst_vol.makedirs(amiga_dir)
                dst_file = amiga_dir + "/" + e.name_str() if amiga_dir else e.name_str()
                
                stream = src_vol.read_file(e)
                chk_stream = ChecksumStream(stream) if args.checksum else stream
                
                dst_vol.write_file(dst_file, chk_stream, size=e.size, protect=e.protect, mtime=e.mtime())
                total_bytes += e.size
                file_count += 1
                if args.checksum:
                    print(f"copied {dst_file} (md5: {chk_stream.md5.hexdigest()})")
                
        elapsed = time.time() - start_time
        print(f"copied {file_count} files, {total_bytes} bytes in {elapsed:.4f} seconds")
    return 0


# ---------------------------------------------------------------- put
def cmd_put(args):
    with open_image(args.image, writable=True) as img:
        vol_ref, path = img.parse_path(args.dest)
        vol = vol_ref.mount()
        protect = str_to_protect(args.protect) if args.protect else 0
        comment = args.comment.encode("latin-1") if args.comment else b""

        src = args.src
        if os.path.isfile(src):
            if not path or path.endswith("/"):
                path = (path or "") + os.path.basename(src)
            else:
                # writing onto an existing dir drops the file inside it
                try:
                    if vol.resolve(path).is_dir():
                        path = path.rstrip("/") + "/" + os.path.basename(src)
                except FSError:
                    pass
            with open(src, "rb") as fh:
                data = fh.read()
            mtime = datetime.fromtimestamp(os.path.getmtime(src))
            vol.write_file(path, data, protect=protect, comment=comment, mtime=mtime)
            print("wrote %s (%d bytes)" % (path, len(data)))
            return 0
        if os.path.isdir(src):
            if not args.recursive:
                print("error: %s is a directory (use -r)" % src, file=sys.stderr)
                return 1
            base = path.rstrip("/")
            n = 0
            for root, dirs, files in os.walk(src):
                rel = os.path.relpath(root, src)
                amiga_dir = base if rel == "." else (
                    (base + "/" if base else "") + rel.replace(os.sep, "/")
                )
                if amiga_dir:
                    vol.makedirs(amiga_dir)
                for f in files:
                    hostf = os.path.join(root, f)
                    apath = (amiga_dir + "/" if amiga_dir else "") + f
                    with open(hostf, "rb") as fh:
                        data = fh.read()
                    mtime = datetime.fromtimestamp(os.path.getmtime(hostf))
                    vol.write_file(apath, data, mtime=mtime)
                    n += 1
            print("wrote %d files under %s" % (n, base or "root"))
            return 0
        print("error: no such file: %s" % src, file=sys.stderr)
        return 1


# ---------------------------------------------------------------- mkdir / rm / mv
def cmd_mkdir(args):
    with open_image(args.image, writable=True) as img:
        vol_ref, path = img.parse_path(args.path)
        vol = vol_ref.mount()
        if args.parents:
            vol.makedirs(path)
        else:
            vol.mkdir(path)
        print("created %s" % args.path)
    return 0


def cmd_rm(args):
    with open_image(args.image, writable=True) as img:
        vol_ref, path = img.parse_path(args.path)
        vol = vol_ref.mount()
        vol.delete(path, recursive=args.recursive)
        print("deleted %s" % args.path)
    return 0


def cmd_mv(args):
    with open_image(args.image, writable=True) as img:
        vol_ref, src = img.parse_path(args.src)
        vol_ref2, dst = img.parse_path(args.dst)
        if vol_ref is not vol_ref2:
            print("error: mv works within one volume", file=sys.stderr)
            return 1
        vol = vol_ref.mount()
        vol.rename(src, dst)
        print("renamed %s -> %s" % (args.src, args.dst))
    return 0


# ---------------------------------------------------------------- check
def cmd_check(args):
    rc = 0
    with open_image(args.image) as img:
        vols = [img.get_volume(args.volume)] if args.volume else img.volumes
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
    "sfs": 0x53465300, "sfs0": 0x53465300,
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
    if args.format:
        args.volume = None
        args.label = args.format
        args.dostype = args.dostype or ("ofs" if args.adf else "ffs-intl")
        return cmd_format(args)
    return 0


def cmd_format(args):
    with open_image(args.image, writable=True) as img:
        vol_ref = img.get_volume(getattr(args, "volume", None))
        
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

    rc = 0
    base = open_blkdev(args.image)
    try:
        overlay = OverlayBlockDevice(base)
        sim = DiskImage(args.image, writable=True, blkdev=overlay)
        vols = [sim.get_volume(args.volume)] if args.volume else sim.volumes
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
    with open_image(args.image) as img:
        vol_ref, path = img.parse_path(args.path)
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
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("bench", help="benchmark file read operations")
    p.add_argument("image")
    p.add_argument("path", nargs="?", default="")
    p.add_argument("--limit-files", type=int, help="max files to read")
    p.add_argument("--limit-bytes", type=int, help="max bytes to read")
    p.add_argument("--filter", help="substring filter for file names")
    p.set_defaults(func=cmd_bench)

    p = sub.add_parser("ls", help="list a directory (VOL:Path)")
    p.add_argument("image")
    p.add_argument("path", nargs="?", default="")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("cat", help="print a file to stdout")
    p.add_argument("image")
    p.add_argument("path")
    p.set_defaults(func=cmd_cat)

    p = sub.add_parser("extract", help="copy files out of the image")
    p.add_argument("image")
    p.add_argument("path")
    p.add_argument("dest")
    p.add_argument("-r", "--recursive", action="store_true")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("put", help="copy a host file/dir into the image")
    p.add_argument("image")
    p.add_argument("src")
    p.add_argument("dest")
    p.add_argument("-r", "--recursive", action="store_true")
    p.add_argument("--comment")
    p.add_argument("--protect", help="e.g. 'hsparwed' subset like '----rwed'")
    p.set_defaults(func=cmd_put)

    p = sub.add_parser("cp", help="copy files between images (streaming)")
    p.add_argument("src_image")
    p.add_argument("src_path")
    p.add_argument("dst_image")
    p.add_argument("dst_path")
    p.add_argument("-r", "--recursive", action="store_true")
    p.add_argument("--checksum", action="store_true", help="calculate MD5 checksum while copying")
    p.set_defaults(func=cmd_cp)

    p = sub.add_parser("mkdir", help="create a directory")
    p.add_argument("image")
    p.add_argument("path")
    p.add_argument("-p", "--parents", action="store_true")
    p.set_defaults(func=cmd_mkdir)

    p = sub.add_parser("rm", help="delete a file or directory")
    p.add_argument("image")
    p.add_argument("path")
    p.add_argument("-r", "--recursive", action="store_true")
    p.set_defaults(func=cmd_rm)

    p = sub.add_parser("mv", help="rename/move within a volume")
    p.add_argument("image")
    p.add_argument("src")
    p.add_argument("dst")
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
    p.add_argument("--rdb-cyls", type=int, default=2)
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_rdb_init)

    p = sub.add_parser("part-add", help="add a partition to the RDB")
    p.add_argument("image")
    p.add_argument("name", help="drive name, e.g. DH0")
    p.add_argument("--size", help="e.g. 500M (default: all remaining space)")
    p.add_argument("--dostype", help="ofs|ffs|ffs-intl|... or hex dostype")
    p.add_argument("--bs", type=int, default=512, help="fs block size (512..32768)")
    p.add_argument("--bootable", action="store_true")
    p.add_argument("--pri", type=int, default=0, help="boot priority")
    p.add_argument("--format", metavar="LABEL", help="format the new partition")
    p.set_defaults(func=cmd_part_add)

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
    p.add_argument("--dostype", help="ofs|ffs|ofs-intl|ffs-intl or hex")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_create)

    p = sub.add_parser("format", help="format a volume (OFS/FFS)")
    p.add_argument("image")
    p.add_argument("label")
    p.add_argument("--volume", help="partition to format (drive name/index)")
    p.add_argument("--dostype", help="ofs|ffs|ofs-intl|ffs-intl or hex")
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
