# OS Install Tool — Design Document

## Overview

A standalone Python script (`tools/install_os.py`) that creates bootable
Amiga hard disk images from OS source material (ADF floppies, tar archives,
or host directories). The tool is **profile-driven** — JSON profiles define
per-OS-build metadata including source files, exclusions, filesystem
settings, bootblock, and driver embedding.

The tool imports the amidisk library and orchestrates existing primitives
(create, rdb-init, part-add, format, bulk copy, bootblock, fs-add). It does
not modify the library or CLI.

## Why a standalone script (not a CLI command)

1. OS installs are inherently version-specific — different ADF sets,
   different files to exclude, different dostypes/bootblocks per OS version
2. Profiles are easy to version, fork, and customize without touching the
   core library
3. The amidisk library already has every primitive needed; the installer is
   pure orchestration
4. A standalone script carries its own metadata directory (`tools/profiles/`)
   without bloating the library

## File layout

```
doc/os-install-design.md     # This document
tools/
  install_os.py              # Standalone installer script
  profiles/                   # OS build profiles (JSON)
    example_profile.json      # Documented example / template
```

## Profile schema

Profiles are JSON files in `tools/profiles/`. Example:

```json
{
  "name": "AmigaOS 3.2.3",
  "description": "Clean OS install from ADF set",

  "disk": {
    "size": "2G",
    "heads": 16,
    "sectors": 63
  },

  "partition": {
    "name": "DH0",
    "dostype": "ffs-intl",
    "block_size": 4096,
    "bootable": true,
    "label": "System",
    "size": "*"
  },

  "sources": [
    "Install-Workbench.adf",
    "Extras.adf",
    "os_content.tar"
  ],

  "exclusions": [
    "**/.info",
    "Install/**"
  ],

  "bootblock": "2x3x",
  "filesystem_driver": "auto"
}
```

### Field reference

| Field | Type | Description |
|---|---|---|
| `name` | string | Human-readable name for the build |
| `description` | string | Short description |
| `disk.size` | string | Image size (e.g. `"2G"`, `"500M"`) |
| `disk.heads` | int | Disk geometry heads (default 16) |
| `disk.sectors` | int | Disk geometry sectors per track (default 63) |
| `partition.name` | string | Drive name (e.g. `"DH0"`) |
| `partition.dostype` | string | `ffs-intl`, `pfs3`, `sfs`, or hex `0x444F5303` |
| `partition.block_size` | int | FS block size in bytes (512–32768, multiple of 512) |
| `partition.bootable` | bool | Mark partition bootable in RDB |
| `partition.label` | string | Volume label after formatting |
| `partition.size` | string | `"*"` for rest of disk, or `"500M"` |
| `sources` | array | Source files/dirs; auto-detected by extension |
| `exclusions` | array | Glob patterns matched against Amiga paths |
| `bootblock` | string | `"1x"`, `"2x3x"`, path to binary, or `null` to skip |
| `filesystem_driver` | string | `"auto"`, explicit path, or `null` to skip |

### Source auto-detection

- `.adf` files → opened as floppy images, all files streamed to target
- `.tar` files → streamed via amidisk's tarreader (libarchive or pure-Python)
- Directories → walked via `os.walk()`, files streamed individually
- Other archives (`.lha`, `.zip`) → delegated to amidisk archive handlers

## CLI interface

```
python3 tools/install_os.py PROFILE.json OUTPUT.hdf [options]
python3 tools/install_os.py PROFILE.json EXISTING.hdf:DH0 [options]
python3 tools/install_os.py --source DIR/ --dostype ffs-intl OUTPUT.hdf [options]
```

### Options

| Flag | Description |
|---|---|
| `--source PATH` | Override profile sources; directory/tar/adf (no profile needed) |
| `--size SIZE` | Override disk size (e.g. `4G`) |
| `--dostype TYPE` | Override partition dostype |
| `--block-size N` | Override FS block size |
| `--label LABEL` | Override volume label |
| `--bootblock PATH` | Override bootblock (`1x`, `2x3x`, path, or `none`) |
| `--no-driver` | Skip auto driver embedding |
| `--dry-run` | Show what would be done, copy nothing |
| `--force` | Overwrite existing output image |

## Pipeline

### 1. Target preparation

Two modes based on the output argument:

- **New image** (`OUTPUT.hdf`, no colon): create file, init RDB, add
  partition, format. Steps: `open(path, 'wb').truncate(size)` →
  `RDisk.create(dev)` → `rdisk.add_partition(...)` → `FFSVolume.format(...)`

- **Existing image** (`EXISTING.hdf:DH0`, with colon): open image, locate
  partition by name, optionally reformat. Steps: `DiskImage(path, writable)`
  → `img.get_volume(name)` → optionally `vol.format(...)`

### 2. Source streaming

A unified iterator yields `(amiga_path, is_dir, size, stream_fn)` tuples
from any source type. For each file:

1. Check against exclusion patterns (fnmatch on Amiga path)
2. Create parent dirs on target via `dst_vol.makedirs()`
3. Stream source → target via `dst_vol.write_file(path, stream, size=N)`
4. All within a `with dst_vol.bulk():` context for batched metadata commits

### 3. Bootblock installation

After all files are copied:

1. Read bootblock payload from `data/bootblocks/std_boot_2x3x.bin` (or
   custom path)
2. Build full 1024-byte bootblock: 4-byte dostype signature + 4-byte
   checksum placeholder + 4-byte root pointer + payload + zero padding
3. Compute checksum (additive carry wraparound, complement)
4. Write to blocks 0–1 of the partition

### 4. Driver embedding

If the partition's dostype is not a standard DOS\\x variant, embed the
appropriate filesystem handler:

1. Look up driver in `data/drivers/manifest.json` by dostype
2. Read the hunk executable binary
3. Embed via `rdisk.add_filesystem(data, dos_type, version)`

### 5. Verification

- Run `vol.check()` on the target partition
- Print summary: files copied, dirs created, bytes transferred, time
  elapsed, free space remaining, check status

## Library reuse

| Capability | amidisk function |
|---|---|
| Create blank image | `open(path, 'wb').truncate(size)` |
| Init RDB | `amidisk.rdb.RDisk.create(dev, ...)` |
| Add partition | `rdisk.add_partition(name, ...)` |
| Format volume | `FFSVolume(dev).format(label, dos_type)` |
| Open ADF source | `amidisk.image.open_image(adf_path)` |
| Stream from ADF | `vol.read_file(entry)` → generator |
| Stream from tar | `amidisk.tarreader.open_tar(path)` |
| Write file | `vol.write_file(path, stream, size=N)` |
| Bulk mode | `with vol.bulk(): ...` |
| Bootblock install | write blocks 0–1 with signature + checksum |
| Driver embed | `rdisk.add_filesystem(data, dos_type, version)` |
| Check | `vol.check()` |

## Out of scope

- C++ port (Python-only for now)
- Multi-partition installs (single partition per invocation)
- ADF creation (the tool reads ADFs, does not create them)
- Post-install prefs/Startup-sequence customization
- GUI
