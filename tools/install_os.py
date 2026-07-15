#!/usr/bin/env python3
"""OS Install Tool — create bootable Amiga hard disk images from OS sources.

Standalone script that uses the amidisk library to orchestrate:
  1. Create disk image (or open existing)
  2. Partition + format
  3. Stream OS files from ADFs, tar archives, or host directories
  4. Install bootblock + embed filesystem driver
  5. Verify

Usage:
  python3 tools/install_os.py PROFILE.json OUTPUT.hdf [options]
  python3 tools/install_os.py PROFILE.json EXISTING.hdf:DH0 [options]
  python3 tools/install_os.py --source DIR/ --dostype ffs-intl OUTPUT.hdf [options]
"""

import argparse
import fnmatch
import json
import os
import struct
import sys
import time
from datetime import datetime

# Resolve amidisk package (development install or PYTHONPATH)
_AMIDISK_SRC = os.path.join(os.path.dirname(__file__), "..", "amidisk_python", "src")
if os.path.isdir(_AMIDISK_SRC):
    sys.path.insert(0, _AMIDISK_SRC)

from amidisk.blkdev import open_blkdev
from amidisk.image import DiskImage, open_image, engine_for_dostype
from amidisk.rdb import RDisk
from amidisk.fs.ffs import FFSVolume, FSError
from amidisk.tarreader import open_tar

# Path to the data directory (bootblocks + drivers)
_DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "data")
_BOOTBLOCKS_DIR = os.path.join(_DATA_DIR, "bootblocks")
_DRIVERS_DIR = os.path.join(_DATA_DIR, "drivers")

# Dostype name -> value mapping (mirrors cli.py DOS_TYPES)
DOS_TYPES = {
    "ofs": 0x444F5300, "dos0": 0x444F5300,
    "ffs": 0x444F5301, "dos1": 0x444F5301,
    "ofs-intl": 0x444F5302, "dos2": 0x444F5302,
    "ffs-intl": 0x444F5303, "dos3": 0x444F5303,
    "ofs-dc": 0x444F5304, "dos4": 0x444F5304,
    "ffs-dc": 0x444F5305, "dos5": 0x444F5305,
    "ofs-intl-lnfs": 0x444F5306, "dos6": 0x444F5306,
    "ffs-intl-lnfs": 0x444F5307, "dos7": 0x444F5307,
    "sfs": 0x53465300, "sfs0": 0x53465300,
    "pfs3": 0x50465303, "pds3": 0x50445303,
}


def parse_dostype(text, default=0x444F5303):
    if not text:
        return default
    dt = DOS_TYPES.get(text.lower())
    if dt is not None:
        return dt
    return int(text, 0)


def parse_size(text):
    text = text.strip().lower()
    mult = 1
    for suffix, m in (("k", 1024), ("m", 1024**2), ("g", 1024**3)):
        if text.endswith(suffix):
            mult = m
            text = text[:-1]
            break
    return int(float(text) * mult)


def human_size(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0

# -----------------------------------------------------------------------
# Progress reporting
# -----------------------------------------------------------------------

_last_t = 0.0
_last_bytes = 0


def print_progress(current_bytes, total_bytes, current_file=""):
    global _last_t, _last_bytes
    step = max(262144, total_bytes // 500)
    if current_bytes < total_bytes and (current_bytes - _last_bytes < step):
        return
    now = time.time()
    if current_bytes < total_bytes and (now - _last_t < 0.20):
        return
    _last_t = now
    _last_bytes = current_bytes
    percent = (current_bytes / total_bytes) * 100 if total_bytes > 0 else 0
    bar_len = 30
    filled = int((percent / 100) * bar_len) if percent <= 100 else bar_len
    bar = '#' * filled + '-' * (bar_len - filled)
    sys.stdout.write("\x1b[2K\r[%s] %5.1f%% (%s / %s) %s" % (
        bar, percent, human_size(current_bytes), human_size(total_bytes),
        current_file[:40]))
    sys.stdout.flush()


# -----------------------------------------------------------------------
# Exclusion matching
# -----------------------------------------------------------------------

def is_excluded(amiga_path, patterns):
    """Check if an Amiga path matches any exclusion glob pattern."""
    for pat in patterns:
        if fnmatch.fnmatch(amiga_path, pat):
            return True
        # Also check without leading slash
        if amiga_path.startswith("/") and fnmatch.fnmatch(amiga_path[1:], pat):
            return True
    return False


def truncate_name(name, max_len):
    """Truncate a filename to fit Amiga name length limits."""
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

# -----------------------------------------------------------------------
# Profile loading
# -----------------------------------------------------------------------

DEFAULT_PROFILE = {
    "name": "ad-hoc",
    "description": "",
    "disk": {"size": "2G", "heads": 16, "sectors": 63},
    "partition": {
        "name": "DH0",
        "dostype": "ffs-intl",
        "block_size": 4096,
        "bootable": True,
        "label": "System",
        "size": "*",
    },
    "sources": [],
    "exclusions": [],
    "bootblock": "2x3x",
    "filesystem_driver": "auto",
}


def load_profile(path):
    """Load a JSON profile, merging with defaults."""
    with open(path) as fh:
        prof = json.load(fh)
    # Deep-merge with defaults so missing fields are filled
    merged = json.loads(json.dumps(DEFAULT_PROFILE))
    for key, val in prof.items():
        if isinstance(val, dict) and isinstance(merged.get(key), dict):
            merged[key].update(val)
        else:
            merged[key] = val
    # Resolve source paths relative to the profile file's directory
    base_dir = os.path.dirname(os.path.abspath(path))
    resolved = []
    for src in merged.get("sources", []):
        if os.path.isabs(src):
            resolved.append(src)
        else:
            resolved.append(os.path.join(base_dir, src))
    merged["sources"] = resolved
    return merged


def apply_overrides(profile, args):
    """Apply CLI overrides on top of the loaded profile."""
    if getattr(args, "source", None):
        profile["sources"] = [args.source]
    if getattr(args, "size", None):
        profile["disk"]["size"] = args.size
    if getattr(args, "dostype", None):
        profile["partition"]["dostype"] = args.dostype
    if getattr(args, "block_size", None):
        profile["partition"]["block_size"] = args.block_size
    if getattr(args, "label", None):
        profile["partition"]["label"] = args.label
    if getattr(args, "bootblock", None):
        if args.bootblock.lower() == "none":
            profile["bootblock"] = None
        else:
            profile["bootblock"] = args.bootblock
    if getattr(args, "no_driver", False):
        profile["filesystem_driver"] = None
    return profile

# -----------------------------------------------------------------------
# Target preparation
# -----------------------------------------------------------------------

def prepare_new_image(image_path, profile, dry_run=False, force=False):
    """Create a new disk image with RDB, partition, and filesystem.

    Returns (img, vol_ref, dst_vol) ready for file copying.
    """
    if os.path.exists(image_path) and not force:
        print("error: %s exists (use --force)" % image_path, file=sys.stderr)
        return None, None, None

    disk = profile["disk"]
    part = profile["partition"]
    size = parse_size(disk["size"])
    size -= size % 512

    if dry_run:
        print("[dry-run] would create %s (%s)" % (image_path, human_size(size)))
        print("[dry-run] would init RDB (heads=%d, sectors=%d)" % (
            disk.get("heads", 16), disk.get("sectors", 63)))
        print("[dry-run] would add partition %s (%s, %s, bs=%d)" % (
            part["name"], part["dostype"], part.get("size", "*"),
            part.get("block_size", 512)))
        print("[dry-run] would format as %r" % part["label"])
        return None, None, None

    # 1. Create blank image
    with open(image_path, "wb") as fh:
        fh.truncate(size)
    print("created %s (%s)" % (image_path, human_size(size)))

    # 2. Init RDB
    dev = open_blkdev(image_path, read_only=False)
    rd = RDisk.create(
        dev,
        sectors=disk.get("sectors", 63),
        heads=disk.get("heads", 0),
    )
    print("initialized RDB")

    # 3. Add partition
    dos_type = parse_dostype(part["dostype"])
    bs = part.get("block_size", 512)
    size_bytes = None
    if part.get("size", "*") not in ("*", "auto"):
        size_bytes = parse_size(part["size"])
    part_obj = rd.add_partition(
        part["name"],
        size_bytes=size_bytes,
        dos_type=dos_type,
        sec_per_blk=bs // 512,
        bootable=part.get("bootable", False),
    )
    pi = part_obj.get_info()
    print("added %s: %s, %s, bs=%d" % (
        pi["drv_name"], human_size(pi["size_bytes"]), pi["dos_type"], bs))
    dev.close()

    # 4. Format
    img = DiskImage(image_path, writable=True)
    vol_ref = img.get_volume(part["name"])
    engine = engine_for_dostype(dos_type)
    vol = vol_ref.raw_volume(engine)
    vol.format(part["label"].encode("latin-1"), dos_type=dos_type)
    mounted = vol.open()
    print("formatted %s as %r (%s)" % (vol_ref.name, part["label"], pi["dos_type"]))

    # 5. Embed driver if needed
    _maybe_embed_driver(img.rdisk, dos_type, profile)

    return img, vol_ref, mounted


def prepare_existing_image(image_path, vol_name, profile, dry_run=False):
    """Open an existing image and optionally reformat the target partition.

    Returns (img, vol_ref, dst_vol) ready for file copying.
    """
    if dry_run:
        print("[dry-run] would open %s and install to %s" % (image_path, vol_name))
        return None, None, None

    img = DiskImage(image_path, writable=True)
    vol_ref = img.get_volume(vol_name)
    if not vol_ref:
        print("error: no volume %r in image" % vol_name, file=sys.stderr)
        img.close()
        return None, None, None

    part = profile["partition"]
    dos_type = parse_dostype(part["dostype"])

    # Reformat the partition
    engine = engine_for_dostype(dos_type)
    vol = vol_ref.raw_volume(engine)
    vol.format(part["label"].encode("latin-1"), dos_type=dos_type)
    mounted = vol.open()
    print("reformatted %s as %r" % (vol_ref.name, part["label"]))

    # Embed driver if needed
    _maybe_embed_driver(img.rdisk, dos_type, profile)

    return img, vol_ref, mounted

# -----------------------------------------------------------------------
# Driver embedding
# -----------------------------------------------------------------------

def _maybe_embed_driver(rdisk, dos_type, profile):
    """Embed a filesystem driver into the RDB if the dostype needs one."""
    driver_spec = profile.get("filesystem_driver", "auto")
    if not driver_spec:
        return

    # DOS\x types are handled by the ROM — no driver needed
    tag3 = dos_type >> 8
    if tag3 == 0x444F53:
        return

    # Check if a driver is already embedded
    try:
        for fs in rdisk.get_filesystems():
            if fs.fshd_blk.dos_type == dos_type:
                return  # already have one
    except Exception:
        pass

    if driver_spec == "auto":
        # Look up in the driver collection
        manifest_path = os.path.join(_DRIVERS_DIR, "manifest.json")
        if not os.path.isfile(manifest_path):
            print("warning: no driver collection found at %s" % _DRIVERS_DIR,
                  file=sys.stderr)
            return
        with open(manifest_path) as fh:
            manifest = json.load(fh)
        for d in manifest.get("drivers", []):
            if any(int(t, 0) == dos_type for t in d.get("dostypes", [])):
                driver_path = os.path.join(_DRIVERS_DIR, d["file"])
                ver = tuple(int(x) for x in d.get("version", "0.0").split("."))[:2]
                break
        else:
            print("warning: no driver for %s in collection" %
                  _dostype_str(dos_type), file=sys.stderr)
            return
    else:
        driver_path = driver_spec
        ver = None

    if not os.path.isfile(driver_path):
        print("warning: driver not found: %s" % driver_path, file=sys.stderr)
        return

    with open(driver_path, "rb") as fh:
        data = fh.read()
    try:
        fs = rdisk.add_filesystem(data, dos_type, version=ver, patch_flags=0x180)
        print("embedded %s driver v%s (%d bytes)" % (
            _dostype_str(dos_type),
            "%d.%d" % ver if ver else "?", len(data)))
    except Exception as ex:
        print("warning: could not embed driver: %s" % ex, file=sys.stderr)


def _dostype_str(dt):
    """Render a dostype as DOS\\x or PFS\\3 etc."""
    b = struct.pack(">I", dt)
    try:
        return b.decode("ascii")
    except Exception:
        return "0x%08X" % dt

# -----------------------------------------------------------------------
# Source iterators
# -----------------------------------------------------------------------

def iter_tar_source(path):
    """Yield (amiga_path, is_dir, size, stream_fn) from a tar archive."""
    with open_tar(path) as tar:
        for member in tar:
            name = member.name
            if name.startswith("./"):
                name = name[2:]
            elif name == ".":
                continue
            if not name:
                continue
            # Skip macOS AppleDouble and PaxHeader
            basename = name.split("/")[-1]
            if basename.startswith("._") or "/._" in name:
                continue
            if "PaxHeader" in name:
                continue

            if member.isdir:
                yield (name, True, 0, None)
            elif member.isfile:
                yield (name, False, member.size, member.chunks)


def iter_adf_source(path):
    """Yield (amiga_path, is_dir, size, stream_fn) from an ADF image."""
    img = open_image(path)
    try:
        vol_ref = img.volumes[0]
        vol = vol_ref.mount()
        for prefix, entry in vol.walk("/"):
            name = prefix + "/" + entry.name_str() if prefix else entry.name_str()
            name = name.lstrip("/")
            if entry.is_dir():
                yield (name, True, 0, None)
            elif entry.is_file():
                # Capture entry for the lambda
                e = entry
                yield (name, False, e.size, lambda e=e: vol.read_file(e))
    finally:
        img.blkdev.close()


def iter_directory_source(path):
    """Yield (amiga_path, is_dir, size, stream_fn) from a host directory."""
    for root, dirs, files in os.walk(path):
        rel = os.path.relpath(root, path)
        if rel != ".":
            amiga_dir = rel.replace(os.sep, "/")
            yield (amiga_dir, True, 0, None)

        for f in files:
            hostf = os.path.join(root, f)
            rel_dir = os.path.relpath(root, path)
            if rel_dir == ".":
                amiga_path = f
            else:
                amiga_path = os.path.join(rel_dir, f).replace(os.sep, "/")
            size = os.path.getsize(hostf)

            def make_stream(hp=hostf):
                return _file_chunks(hp)

            yield (amiga_path, False, size, make_stream)


def _file_chunks(path, chunk_size=1 << 20):
    """Generator that yields chunks from a host file."""
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(chunk_size)
            if not chunk:
                break
            yield chunk


def detect_source_type(path):
    """Auto-detect source type by extension/content."""
    if os.path.isdir(path):
        return "directory"
    lower = path.lower()
    if lower.endswith(".adf") or lower.endswith(".img"):
        return "adf"
    if lower.endswith(".tar"):
        return "tar"
    # Try archive handlers
    from amidisk.archives import get_handler
    handler = get_handler(path)
    if handler:
        return "archive"
    # Default: treat as single file
    return "file"

# -----------------------------------------------------------------------
# Bootblock installation
# -----------------------------------------------------------------------

def install_bootblock(vol_ref, dos_type, bootblock_spec, dry_run=False):
    """Install a standard or custom bootblock to the partition."""
    if not bootblock_spec:
        return

    if dry_run:
        print("[dry-run] would install bootblock (%s)" % bootblock_spec)
        return

    # Resolve bootblock payload
    if bootblock_spec == "1x":
        payload_path = os.path.join(_BOOTBLOCKS_DIR, "std_boot_1x.bin")
    elif bootblock_spec == "2x3x":
        payload_path = os.path.join(_BOOTBLOCKS_DIR, "std_boot_2x3x.bin")
    else:
        payload_path = bootblock_spec

    if not os.path.isfile(payload_path):
        print("warning: bootblock not found: %s" % payload_path, file=sys.stderr)
        return

    with open(payload_path, "rb") as fh:
        payload = fh.read()

    vol = vol_ref.raw_volume()
    bb_size = vol.dev.block_bytes * 2  # 2 blocks

    if len(payload) > bb_size - 12:
        print("warning: bootcode too large (%d > %d bytes), skipping" % (
            len(payload), bb_size - 12), file=sys.stderr)
        return

    # Build full bootblock: signature(4) + checksum(4) + root(4) + payload + pad
    bb = bytearray(bb_size)
    struct.pack_into(">I", bb, 0, dos_type)      # signature
    struct.pack_into(">I", bb, 4, 0)               # checksum placeholder
    struct.pack_into(">I", bb, 8, 0)               # root block pointer
    bb[12:12 + len(payload)] = payload              # boot code

    # Compute checksum: additive carry wraparound, skip offset 4
    acc = 0
    for i in range(0, bb_size, 4):
        if i == 4:
            continue
        acc += struct.unpack_from(">I", bb, i)[0]
        if acc > 0xFFFFFFFF:
            acc = (acc & 0xFFFFFFFF) + 1
    checksum = (~acc) & 0xFFFFFFFF
    struct.pack_into(">I", bb, 4, checksum)

    vol.dev.write(0, bytes(bb))
    print("installed bootblock (%d bytes, %s)" % (len(payload), _dostype_str(dos_type)))


# -----------------------------------------------------------------------
# Streaming pipeline
# -----------------------------------------------------------------------

def stream_sources(dst_vol, sources, exclusions, dry_run=False):
    """Stream all sources into the target volume using bulk mode.

    Returns (file_count, dir_count, total_bytes).
    """
    file_count = 0
    dir_count = 0
    total_bytes = 0

    if dry_run:
        for src in sources:
            stype = detect_source_type(src)
            print("[dry-run] source: %s (%s)" % (src, stype))
        return 0, 0, 0

    # Calculate total size for progress (best-effort)
    total_size = 0
    for src in sources:
        stype = detect_source_type(src)
        if stype == "directory":
            for root, _, files in os.walk(src):
                for f in files:
                    total_size += os.path.getsize(os.path.join(root, f))
        elif stype == "file":
            total_size += os.path.getsize(src)

    import contextlib
    bulk_ctx = (dst_vol.bulk() if hasattr(dst_vol, "bulk")
                else contextlib.nullcontext())
    with bulk_ctx:
        for src in sources:
            stype = detect_source_type(src)
            print("streaming %s (%s)..." % (src, stype))

            if stype == "tar":
                iterator = iter_tar_source(src)
            elif stype == "adf":
                iterator = iter_adf_source(src)
            elif stype == "directory":
                iterator = iter_directory_source(src)
            else:
                # Single file: stream as a single write
                fname = truncate_name(os.path.basename(src),
                                      getattr(dst_vol, "max_name_len", 30))
                size = os.path.getsize(src)
                if not is_excluded(fname, exclusions):
                    dst_vol.write_file(fname, _file_chunks(src), size=size)
                    file_count += 1
                    total_bytes += size
                continue

            made_dirs = set()
            max_len = getattr(dst_vol, "max_name_len", 30)

            for amiga_path, is_dir, size, stream_fn in iterator:
                if is_excluded(amiga_path, exclusions):
                    continue

                # Truncate path components to fit Amiga name limits
                parts = [truncate_name(p, max_len) for p in amiga_path.split("/")]
                amiga_path = "/".join(parts)

                if is_dir:
                    if amiga_path not in made_dirs:
                        dst_vol.makedirs(amiga_path)
                        made_dirs.add(amiga_path)
                        dir_count += 1
                else:
                    # Ensure parent dir exists
                    parent = "/".join(amiga_path.split("/")[:-1])
                    if parent and parent not in made_dirs:
                        dst_vol.makedirs(parent)
                        made_dirs.add(parent)

                    dst_vol.write_file(amiga_path, stream_fn(), size=size)
                    file_count += 1
                    total_bytes += size

                    if total_size > 0:
                        print_progress(total_bytes, total_size,
                                       amiga_path.split("/")[-1])

    # Final progress update
    if total_size > 0:
        global _last_t
        _last_t = 0
        print_progress(total_bytes, total_bytes, "")
    sys.stdout.write("\n")

    return file_count, dir_count, total_bytes

# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="install_os.py",
        description="Create bootable Amiga hard disk images from OS sources.",
    )
    ap.add_argument("profile", nargs="?", help="JSON profile file (omit with --source)")
    ap.add_argument("output", help="Output image (new.hdf or existing.hdf:DH0)")
    ap.add_argument("--source", help="Override sources: directory/tar/adf path")
    ap.add_argument("--size", help="Override disk size (e.g. 4G)")
    ap.add_argument("--dostype", help="Override dostype (ffs-intl, pfs3, ...)")
    ap.add_argument("--block-size", type=int, help="Override FS block size")
    ap.add_argument("--label", help="Override volume label")
    ap.add_argument("--bootblock", help="Override bootblock (1x, 2x3x, path, or none)")
    ap.add_argument("--no-driver", action="store_true", help="Skip driver embedding")
    ap.add_argument("--dry-run", action="store_true", help="Show plan without writing")
    ap.add_argument("--force", action="store_true", help="Overwrite existing image")
    args = ap.parse_args(argv)

    # Load profile
    if args.profile:
        profile = load_profile(args.profile)
    elif args.source:
        profile = json.loads(json.dumps(DEFAULT_PROFILE))
    else:
        ap.error("either PROFILE.json or --source is required")

    profile = apply_overrides(profile, args)

    if not profile.get("sources"):
        print("error: no sources specified (use profile 'sources' or --source)",
              file=sys.stderr)
        return 1

    # Validate sources exist
    for src in profile["sources"]:
        if not os.path.exists(src):
            print("error: source not found: %s" % src, file=sys.stderr)
            return 1

    # Parse output: new image vs existing image:volume
    output = args.output
    if ":" in output and not (len(output) > 1 and output[1] == ":" and output[0].isalpha()):
        # existing.hdf:DH0
        idx = output.rfind(":")
        image_path = output[:idx]
        vol_name = output[idx + 1:]
        if not os.path.isfile(image_path):
            print("error: image not found: %s" % image_path, file=sys.stderr)
            return 1
        existing_mode = True
    else:
        image_path = output
        vol_name = None
        existing_mode = False

    print("=== OS Install: %s ===" % profile.get("name", "ad-hoc"))
    if profile.get("description"):
        print(profile["description"])
    print()

    start_time = time.time()

    # 1. Prepare target
    if existing_mode:
        img, vol_ref, dst_vol = prepare_existing_image(
            image_path, vol_name, profile, dry_run=args.dry_run)
    else:
        img, vol_ref, dst_vol = prepare_new_image(
            image_path, profile, dry_run=args.dry_run, force=args.force)

    if args.dry_run:
        print()
        stream_sources(None, profile["sources"], profile.get("exclusions", []),
                       dry_run=True)
        print("\n[dry-run] would install bootblock + verify")
        return 0

    if img is None:
        return 1

    # 2. Stream sources
    print()
    file_count, dir_count, total_bytes = stream_sources(
        dst_vol, profile["sources"], profile.get("exclusions", []))

    # 3. Install bootblock
    dos_type = parse_dostype(profile["partition"]["dostype"])
    install_bootblock(vol_ref, dos_type, profile.get("bootblock"))

    # 4. Verify
    print("\nverifying...")
    rep = dst_vol.check(deep=False)

    # 5. Summary
    elapsed = time.time() - start_time
    vi = dst_vol.get_info()
    total_space = vi["total_blocks"] * vi["block_size"]
    free_space = vi["free_bytes"]

    print()
    print("=" * 50)
    print("Install complete: %s" % profile.get("name", "ad-hoc"))
    print("  files:     %d" % file_count)
    print("  dirs:      %d" % dir_count)
    print("  bytes:     %s" % human_size(total_bytes))
    print("  time:      %.1f s" % elapsed)
    print("  speed:     %.1f MB/s" % (total_bytes / 1024 / 1024 / max(elapsed, 0.01)))
    print("  free:      %s of %s" % (human_size(free_space), human_size(total_space)))
    print("  check:     %s" % ("OK" if rep.get("ok") else "ERRORS"))
    if rep.get("files"):
        print("             %d files, %d dirs verified" % (
            rep.get("files", 0), rep.get("dirs", 0)))
    print("=" * 50)

    img.close()
    return 0 if rep.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
