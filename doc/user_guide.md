# AmigaFSTool (amidisk) User Guide

`amidisk` is a high-performance command-line utility for manipulating Amiga Hard Disk Files (HDF), Virtual Hard Disks (VHD), and Floppy Disk Images (ADF). It natively understands Amiga Rigid Disk Block (RDB) partition tables and the Amiga FastFileSystem (FFS), allowing you to interact with Amiga storage from modern systems exactly as if you were on a real Amiga.

## Table of Contents
1. [Core Concepts](#core-concepts)
2. [Creating & Formatting New Disks](#creating--formatting-new-disks)
3. [Managing Files & Directories](#managing-files--directories)
4. [Inspecting & Benchmarking](#inspecting--benchmarking)
5. [Repair & Recovery](#repair--recovery)

---

## Command Index

| Command       | Description |
|---------------|-------------|
| `info`        | Show image, RDB, and volume overview |
| `ls`          | List a directory (`VOL:Path`) |
| `cat`         | Print a file to stdout |
| `extract`     | Copy files out of the image to the host |
| `put`         | Copy a host file/dir into the image |
| `cp`          | Copy files directly between two images (streaming) |
| `mkdir`       | Create a new directory |
| `rm`          | Delete a file or directory (recursive) |
| `mv`          | Rename or move files within a volume |
| `create`      | Create a new blank image file (HDF/VHD/ADF) |
| `format`      | Format a volume with an Amiga filesystem (OFS/FFS) |
| `rdb-init`    | Write a fresh RDB partition table |
| `part-add`    | Add a partition to the RDB |
| `part-del`    | Remove a partition from the RDB |
| `check`       | Verify filesystem structures and block allocation bitmap |
| `bench`       | Benchmark file read/write operations |
| `rdb-scan`    | Scan a disk for lost partitions (overwritten RDB) |
| `rdb-rebuild` | Rebuild an overwritten RDB from found partitions |
| `repair`      | Rebuild the FFS allocation bitmap from the directory tree |
| `fs-extract`  | Extract embedded RDB filesystem drivers (like PFS3/SFS) |

---

## Core Concepts

### Path Syntax
When interacting with files inside an image, `amidisk` uses the classic Amiga `Drive:Path` or `Volume:Path` syntax. You must combine the path to your host disk image with the internal Amiga path separated by a colon (`:`).

* **Single Partition Info:** `python3 amidisk info my_image.hdf:DH0`
* **File Operations:** `python3 amidisk cat my_image.hdf:DH0/S/Startup-Sequence`
* **Directory Listing:** `python3 amidisk ls my_image.hdf:System/Devs`

> [!NOTE]
> You can reference partitions either by their RDB Drive Name (e.g. `DH0`) or by their formatted volume label (e.g. `System`). If an image only has a single partition (like an ADF floppy), you can omit the drive name and just use a colon: `my_floppy.adf:S/Startup-Sequence`.

---

## Creating & Formatting New Disks

You can build complete, ready-to-use Amiga hard drives from scratch.

### One-Shot Fast Track (Recommended)
You can create the raw image, initialize the RDB, allocate multiple partitions, and format them all simultaneously in a single command using the `--layout` parameter.

```bash
# Create a standard 4GB disk, 1 partition (DH0), fully formatted & bootable
python3 amidisk create 4G my_new_disk.hdf --layout default

# Create a custom split: 500M Bootable System partition, and fill the rest as 'Work'
python3 amidisk create 4G my_disk.hdf --layout "DH0:500M:ffs-intl:System:boot,DH1:auto:ffs-intl:Work"
```

*Syntax:* `--layout DriveName:Size:DosType:FormatLabel:Boot`
*(Separate multiple partitions with a comma. Use `auto` or `*` for size to use all remaining space).*

### Manual Step-by-Step Approach

**1. Create the Blank Image**
Creates a raw, uninitialized sparse file on your host machine.
```bash
python3 amidisk create 10G my_new_disk.hdf
```

### 2. Initialize the Rigid Disk Block (RDB)
Writes a fresh Amiga RDB partition table to block 0, calculating the optimal CHS (Cylinder/Head/Sector) geometry for the disk size.
```bash
python3 amidisk rdb-init my_new_disk.hdf
```
*(Tip: Use `--force` to wipe and re-initialize a disk that already has an RDB).*

### 3. Add Partitions
Allocates cylinders to a new partition. By default, it will use all available space.
```bash
python3 amidisk part-add my_new_disk.hdf DH0 --dos-type ffs-intl
```
You can create multiple partitions using the `--size` parameter (e.g., `--size 2G`).
*Common `--dos-type` flags:* `ffs-intl`, `ffs-dircache`, `pfs3`, `sfs`.

### 4. Format the Partition
Formats the partition with the Amiga filesystem so it is ready to accept files.
```bash
python3 amidisk format my_new_disk.hdf:DH0 "My_System_Drive"
```

---

## Managing Files & Directories

`amidisk` allows you to seamlessly push, pull, and manipulate files inside the Amiga image.

### Reading & Extracting
* **View a text file:**
  `python3 amidisk cat my_image.hdf:DH0/S/Startup-Sequence`
* **Extract a single file or directory to your host OS:**
  `python3 amidisk extract my_image.hdf:DH0/Games ./host_folder/`
* **List directory contents (supports `--json` output):**
  `python3 amidisk ls my_image.hdf:DH0/Devs`

### Writing & Modifying
* **Copy a file/directory from your host OS into the Amiga image:**
  `python3 amidisk put my_image.hdf ./my_host_app/ DH0/Apps/`
* **Direct Archive Streaming (Zero-Extraction):**
  If the source is a `.tar` or `.tar.gz` archive, `amidisk` will bypass host extraction and stream the files directly into the Amiga filesystem!
  `python3 amidisk put my_image.hdf ./games.tar DH0/Games/`
* **Create a new directory:**
  `python3 amidisk mkdir my_image.hdf:DH0/NewFolder`
* **Move or rename files inside the image:**
  `python3 amidisk mv my_image.hdf:DH0/old_name my_image.hdf:DH0/new_name`
* **Delete files or directories (recursive):**
  `python3 amidisk rm my_image.hdf:DH0/JunkFolder`

*(Note: File copying uses dynamic long-filename truncation to comply with classic Amiga OS 3.2+ LNFS limits without crashing).*

---

## Inspecting & Benchmarking

### `info`
Provides a deeply parsed, highly readable overview of the disk's geometry, RDB configuration, and filesystem statistics.
* **Global Image Info:** `python3 amidisk info my_image.hdf`
* **Specific Partition Details:** `python3 amidisk info my_image.hdf:DH0`

> [!TIP]
> The single-partition view exposes crucial hardware compatibility values like `MaxTransfer`, `Mask`, and `Buffers` read directly from the RDB `DosEnvec`.

### `check`
Verifies filesystem structures, validates the block allocation bitmap, and counts files/directories.
```bash
# Fast check (structure only)
python3 amidisk check my_image.hdf:DH0

# Deep check (traverse every single directory tree)
python3 amidisk check my_image.hdf:DH0 --deep
```

### `bench`
Benchmarks the pure read-speed of the filesystem implementation.
```bash
python3 amidisk bench my_image.hdf:DH0
```

---

## Repair & Recovery

`amidisk` contains powerful forensic tools to recover data from corrupted Amiga drives.

### RDB Recovery
If a disk's RDB partition table is overwritten or corrupted, you can scan the disk to locate lost filesystem boundaries:
```bash
python3 amidisk rdb-scan my_image.hdf
```
Once located, you can automatically rebuild the RDB based on the discovered filesystem headers:
```bash
python3 amidisk rdb-rebuild my_image.hdf
```
*(This operates in simulation mode first so you can verify the proposed partition table before writing).*

### Filesystem Repair
If the FFS allocation bitmap is corrupted (e.g., resulting in "Disk Validating" loops on a real Amiga), you can rebuild it:
```bash
python3 amidisk repair my_image.hdf:DH0
```
This commands traverses the entire directory tree, identifies all legitimately owned blocks, and completely reconstructs the bitmap from scratch.

### Filesystem Driver Extraction
If your RDB contains embedded filesystems (like PFS3 or SFS) loaded by the Kickstart ROM during boot, you can extract the raw binaries:
```bash
python3 amidisk fs-extract my_image.hdf ./output_dir/
```
