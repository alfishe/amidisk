# amidisk — Amiga disk image toolkit

Reliable read/write access to Amiga hard-disk images (HDF), VHD images and
floppy ADFs on modern systems: full RDB partition handling, native OFS/FFS
read/write for all classic dostypes, native PFS3 and SFS read/write,
filesystem check/repair, and an RDB rescue tool for disks whose partition
table was overwritten.

The project ships **two implementations** of the same toolkit:

| Implementation | Language | Role |
|---|---|---|
| **`amidisk_python`** | Python >= 3.9, zero dependencies | Reference implementation -- feature-complete, used as the spec |
| **`amidisk_cpp`** | C++20 | High-performance port -- functional parity with the Python engine |

Both share the same architecture and on-disk format knowledge, described in
[`doc/amiga-disk-tool-architecture.md`](doc/amiga-disk-tool-architecture.md).

## Support matrix

| Layer | Read | Write | Notes |
|---|---|---|---|
| HDF / raw images | Y | Y | sparse-friendly |
| VHD fixed | Y | Y | as used by WinUAE |
| VHD dynamic | Y | - | convert to fixed to write |
| ADF (DD/HD floppies) | Y | Y | |
| RDB partition table | Y | Y | init / add / delete / rescue; FSHD+LSEG driver extraction |
| MBR-wrapped RDB (PiStorm/Emu68) | Y | - | auto-detected |
| OFS/FFS DOS\0-\3 | Y | Y | incl. international mode, any block size 512-32K |
| FFS DOS\4/\5 (dircache) | Y | Y | dircache chains maintained and verified |
| FFS DOS\6/\7 (long filenames) | Y | Y | LNFS: 107-char names, overflow comment blocks |
| PFS3 (PFS\3, PDS\3, AFS\1) | Y | Y | native read/write/format, ported from pfs3aio |
| SFS (SFS\0) | Y | Y | native read/write/format, ported from AROS SFS / asfs |

All engines support **streaming**: `read_file()` yields chunks and
`write_file(path, source, size=...)` accepts bytes, file-likes, or chunk
iterators, so image-to-image copies (`cp`) never buffer whole files or touch
temp files.

**Bulk mode** (`put -r --bulk`, `cp --bulk`) batches metadata commits during
mass imports: bitmap flushes and volume-date updates happen every 1024
operations instead of every file, and on FFS the volume is marked *not
validated* (the AmigaOS convention) for the duration. Default behavior is
unchanged. The trade: if the process dies mid-bulk, up to one batch of
allocations exists only in memory -- `check` detects the state and
`repair --write` reconstructs it on FFS; on PFS3 use pfsDoctor or re-import.

**Archive extraction**: both implementations can stream `.tar` natively and
delegate `.lha`/`.zip`/`.rar`/`.7z` to the corresponding host tools when
installed (the archive is streamed directly into the Amiga volume -- no temp
files, no double-copy).

Validated against: volumes written by AmigaOS 3.2.3 itself (the sample
images), amitools/xdftool (differential, both directions), hst-imager's
independent C# PFS3 implementation (reads our writes and formats
bit-perfect, and writes onto our volumes), and real pfs3aio /
SmartFilesystem handler-written fixtures in `data/test/` -- including
strict handler-visibility checks for SFS (objectnode registration,
hashtable chains, hash16 values verified against the real handler's
output). PFS3/SFS mutation batteries assert zero space leak and that
untouched files stay bit-identical. Caveat: SFS has no second local
implementation to differential-test against (hst-imager lacks SFS), so
the final oracle for SFS writes remains a mount under the real handler.

## How this differs from amitools (xdftool / rdbtool)

[amitools](https://github.com/cnvogelg/amitools) is the reference for RDB
block structures — amidisk's RDB layer is a port of amitools' rdblib.
Beyond that shared foundation, amidisk adds:

| Capability | amitools | amidisk |
|---|---|---|
| **PFS3 write/format** | read-only | native read/write/format |
| **SFS write/format** | not supported | native read/write/format |
| **RDB rescue** | not available | lost-partition scan + simulated rebuild + commit |
| **FFS repair** | not available | bitmap rebuild with dry-run simulation |
| **Safety model** | writes go direct to file | all destructive ops simulate in COW overlay first; `--write` commits verified blocks with backup + read-back compare |
| **MBR-wrapped RDB** | not detected | auto-detected (PiStorm / Emu68) |
| **VHD** | HDF/raw only | fixed read/write, dynamic read |
| **Streaming** | buffers whole files | chunk-based read/write; `cp` never buffers a full file |
| **Bulk import** | one-file-at-a-time metadata flush | batched bitmap/volume-date commits every 1024 ops |
| **Archive extraction** | not available | streams `.tar` natively + delegates `.lha`/`.zip`/`.rar`/`.7z` directly into volumes |
| **Incremental sync** | not available | rsync-style `sync` (Python) |
| **C++ engine** | Python only | dual Python + C++20 with functional parity |
| **Bootblock install** | not available | `bootblock` command + bundled standard bootblocks |

amitools has features amidisk does not: **vamos** (run Amiga m68k
binaries), **hunktool** / **typetool** / **xmstool**, and more mature
floppy/ADF creation tooling. The two projects are complementary —
amidisk focuses on filesystem-level read/write/repair/rescue for hard
disk images, while amitools covers a broader Amiga-development toolchain.

## Python vs C++ performance

Both implementations share the same on-disk format knowledge and
algorithms (run-coalesced I/O, bulk metadata commits, zero-copy writes).
The Python engine is the reference and has received the deepest
optimization work (documented in
[`doc/filesystem-performance.md`](doc/filesystem-performance.md)).
The C++ engine targets the same algorithms with native overhead.

**CLI-level comparison** (Mac Studio M1, internal NVMe SSD, 500 MB HDF,
FFS DOS\3, 201 files — median of 3):

| Operation | Python CLI | C++ CLI | C++ advantage (times) |
|---|---|---|---|
| **Startup (interpreter / process init)** | ~300 ms | ~5 ms | 60× |
| **Write 50 MB file** (`put`) | 610 ms (86 MB/s) | 250 ms (210 MB/s) | 2.4× |
| **Read 50 MB file** (`extract`) | 400 ms (125 MB/s) | 130 ms (385 MB/s) | 3.1× |
| **Copy 200 small files** (`cp -r --bulk`) | 384 ms (520 f/s) | 78 ms (2 560 files/s) | 4.9× |
| **List directory** (`ls`, 200 entries) | 15 ms | 4 ms | 3.8× |
| **Consistency check** (`check`, 201 files) | 396 ms | 15 ms | 26× |

**Key takeaways:**

- **Single-file edits and inspection are effectively instant in both** —
  the difference only matters at scale (bulk imports, full-disk checks).
- **The C++ advantage grows with metadata density.** Large-file
  throughput is 2–3× (both are I/O-bound on the device); small-file
  creation is ~5× (CPU-bound allocation + directory updates); `check`
  is 26× (native `memcmp` page-compare vs Python byte-by-byte).
- **Startup time is the hidden multiplier for scripting.** A loop of
  200 individual `put` calls launches 200 processes: ~60 s in Python,
  ~16 s in C++. Use `cp -r` (single process, streaming) to avoid this.
- **Python catches up at the high end.** The optimized Python engine
  reaches 80–100% of the device ceiling for large reads on extent-based
  formats (PFS3, SFS) — see the performance doc for the full analysis.

**Real-world bulk import** — streaming a 6 GB tar (238 544 files) into an
8 GB FFS 4 K-block partition on Mac Studio M1 / NVMe
(`cp archive.tar IMAGE:DH0/ --bulk`):

| | Python CLI | C++ CLI |
|---|---|---|
| **Wall time** | 22.9 s | 15.9 s |
| **Throughput** | 262 MB/s | 370 MB/s |
| **Files/s** | 10 402 | 14 965 |
| **C++ advantage** | — | **1.4×** |

Both images verified structurally identical: 238 544 files, 7.4 GB used,
check OK. At this scale the engines converge — the workload is
I/O-bound on the device, not CPU-bound on the interpreter.

**When to use which:**

| Scenario | Recommended |
|---|---|
| Batch processing, CI pipelines, 10 000+ file imports | **C++** (5–26× faster) |
| Scripting / prototyping / library embedding | **Python** (zero deps, pip install) |
| Recovery & rescue (`rdb-scan`, `rdb-rebuild`) | **Either** (same algorithms, same results) |
| Reading existing images (inspection, extraction) | **Either** (both saturate I/O on large files) |
| `sync` and `bench` commands | **Python** (not yet in C++ CLI) |

## Building

### Python

```sh
pip install -e ./amidisk_python
# or run without installing:
PYTHONPATH=amidisk_python/src python3 -m amidisk.cli
```

### C++

```sh
cd amidisk_cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# binary at amidisk_cpp/build/bin/amidisk
```

Dependencies (CLI11 v2.4.1, nlohmann/json v3.11.3, GoogleTest) are fetched
automatically by CMake `FetchContent` -- no manual installation required.

**Docker** (reproducible Linux build):

```sh
cd amidisk_cpp
docker build -f docker/Dockerfile -t amidisk-linux .
docker run --rm amidisk-linux              # runs the test suite
docker run --rm amidisk-linux ./build/bin/amidisk --help
```

## CLI

Paths use Amiga notation `VOLUME:Path/To/File`; the volume is a drive
name (`DH0`), a label (`Workbench`) or a partition index.

### Command reference (shared between both implementations)

```sh
amidisk info IMAGE [VOL]             # containers, RDB, partitions, volumes
amidisk ls IMAGE [VOL:path] [-l]
amidisk cat IMAGE VOL:path/file
amidisk extract IMAGE VOL:path DEST [-r]     # preserves mtimes
amidisk put IMAGE HOSTFILE VOL:path [-r] [--bulk] [--comment C] [--protect bits]
amidisk cp SRC IMAGE:VOL/path [-r] [--bulk]  # archive extraction or file/dir copy
amidisk mkdir IMAGE VOL:path [-p]
amidisk rm IMAGE VOL:path [-r]
amidisk mv IMAGE VOL:old VOL:new
amidisk check IMAGE [VOL] [--deep]           # per-filesystem validation

amidisk create IMAGE --size 500M [--adf] [--format LABEL] [--dostype ffs-intl]
amidisk format IMAGE LABEL [--volume DH0] [--dostype ofs|ffs|...|0x444F5303]
amidisk rdb-init IMAGE [--heads N --sectors N]
amidisk part-add IMAGE DH0 [--size 100M] [--bs 4096] [--bootable] [--format LABEL]
amidisk part-del IMAGE DH0 [--write]
amidisk fs-extract IMAGE                     # dump embedded FSHD/LSEG drivers
amidisk fs-add IMAGE DRIVER --dostype pfs3   # embed a bootable m68k driver
amidisk bootblock IMAGE VOL BOOTCODE.bin     # install custom bootblock
amidisk repair IMAGE [VOL] [--deep] [--write] # FFS bitmap rebuild
```

**Python-only commands** (not yet wired into the C++ CLI):

```sh
amidisk sync SRCDIR IMAGE:VOL/dest [--delete] [--dry-run]   # rsync-style sync
amidisk bench IMAGE [VOL] [--limit-files N] [--limit-bytes SIZE]  # read benchmark
```

The C++ CLI uses CLI11's built-in `--help` on every subcommand.

### Recovery tools -- simulate first, write second

All dangerous operations run against an in-RAM copy-on-write overlay,
verify the outcome there, show you the evidence, and only touch the
image with `--write` (committing the verified blocks with a backup of
the old RDB area and a read-back compare):

```sh
amidisk rdb-scan IMAGE               # find lost partitions (orphaned PART
                                     # blocks, FFS/PFS3/SFS root anchors)
amidisk rdb-rebuild IMAGE [--deep]   # simulate the rebuilt RDB, mount every
                                     # recovered volume, run checks, list root
                                     # catalogs and fill/free sizes
amidisk rdb-rebuild IMAGE --write    # same + commit (backup + verify)
```

Example dry-run output of a rescue on a disk whose first 16 sectors were
zeroed:

```
before: image kind 'blank', 0 mountable volume(s)
evidence found:
  ffs   @sector 126          104.9 MB  DOS\3   'System'  (size estimated)
  ffs   @sector 204939       209.6 MB  DOS\3   'Work'    (size estimated)
simulated result (1.5 KB of changes, nothing written yet):
  DH0:  'System'  DOS\3  1018.5 KB used /  99.0 MB free of 100.0 MB -- check OK
      root (1 entries): T/
  DH1:  'Work'    DOS\3  1004.0 KB used / 198.9 MB free of 199.9 MB -- check OK
      root (1 entries): T/
dry run complete: all volumes verified in simulation. Use --write to apply.
```

PFS3/SFS repair is intentionally not native -- use the original tools
(pfsDoctor, SFSsalv); `check` still validates their structures here.

All destructive commands (`repair`, `rdb-rebuild`, `part-del`) default to
a **dry run** and require `--write` to commit — uniformly across both
implementations.

## Library

### Python

```python
from amidisk import open_image

with open_image("data/WB.vhd") as img:          # writable=True to write
    vol = img.get_volume("Workbench").mount()          # by name/label/index
    print(vol.get_info())                              # label, free, dostype...
    for e in vol.list_dir("S"):
        print(e.name_str(), e.size, e.protect_str(), e.mtime())
    data = vol.read_file_bytes("S/startup-sequence")

with open_image("work.hdf", writable=True) as img:
    vol = img.get_volume("DH0").mount()
    vol.makedirs("Data/Incoming")
    vol.write_file("Data/Incoming/x.bin", data, comment=b"imported")
    vol.rename("Data/Incoming/x.bin", "Data/x.bin")
    vol.delete("Data/Incoming")
    report = vol.check(deep=True)                      # structure + bitmap
```

Lower layers are importable on their own: `amidisk.blkdev`
(`ImageFileBlkDev`, `VHDBlkDev`, `PartBlockDevice`, `OverlayBlockDevice`),
`amidisk.rdb` (`RDisk`, block classes -- a port of amitools' rdblib --
and `rescue`), `amidisk.fs.ffs` / `fs.pfs3` / `fs.sfs`.

### C++

```cpp
#include "image.h"
#include "fs/volume.h"

auto img = amidisk::DiskImage::open("work.hdf", /*read_only=*/false);
auto* vref = img->get_volume("DH0");
auto* vol  = vref->mount();              // returns amidisk::Volume*

auto info = vol->get_info();             // label, free/used, dostype...
for (const auto& e : vol->list_dir("S"))
    std::cout << e.name_str() << "  " << e.size << "\n";

vol->makedirs("Data/Incoming");
std::vector<uint8_t> data = /* ... */;
amidisk::WriteParams wp;
wp.comment = "imported";
vol->write_file("Data/Incoming/x.bin", data, wp);
vol->rename("Data/Incoming/x.bin", "Data/x.bin");
vol->delete_path("Data/Incoming", /*recursive=*/true);

auto report = vol->check(/*deep=*/true);
```

The C++ library (`libamidisk`) is a static library with the same layered
architecture as Python: `BlockDevice` trait (`ImageFileBlkDev`,
`VHDBlkDev`, `PartBlockDevice`, `OverlayBlockDevice`, `BulkWriteCache`),
`RDisk` / `rescue`, and `FFSVolume` / `PFS3Volume` / `SFSVolume` engines
all behind a common `Volume` interface.

## Project layout

```
amidisk/
|-- amidisk_python/                Python reference implementation
|   |-- pyproject.toml             package metadata + 'amidisk' entrypoint
|   |-- src/amidisk/
|   |   |-- blkdev.py              L0/L1: image containers + COW overlay
|   |   |-- rdb/                   RDSK/PART/FSHD/LSEG + rescue
|   |   |-- fs/ffs.py              native OFS/FFS (read/write/format/check/repair)
|   |   |-- fs/pfs3.py             native PFS3 (pfs3aio structures)
|   |   |-- fs/sfs.py              native SFS (AROS SFS structures)
|   |   |-- archives/              tar (native) + lha/zip/rar/7z (host-tool delegation)
|   |   |-- sync.py                rsync-style incremental sync
|   |   |-- image.py               DiskImage facade: detection, volume dispatch
|   |   `-- cli.py                 amidisk CLI (reference command set)
|   `-- tests/                     smoke + unit tests
|
|-- amidisk_cpp/                   C++ port (C++20, functional parity)
|   |-- CMakeLists.txt             CMake build (CLI11 + nlohmann/json via FetchContent)
|   |-- docker/Dockerfile          reproducible Linux build + test
|   |-- src/
|   |   |-- blkdev/                BlockDevice, image_file, vhd, part_blkdev, overlay, bulk_cache
|   |   |-- rdb/                   blocks, rdisk, rescue
|   |   |-- fs/                    ffs, pfs3, sfs, volume.h (common interface), util
|   |   |-- archives/              tar (native) + lha/zip/rar/7z (host-tool delegation)
|   |   |-- image.cpp/h            DiskImage facade
|   |   |-- safety.cpp/h           overlay simulate/verify/commit workflow
|   |   |-- cli_util.cpp/h         human_size, parse_size, truncate_name, progress
|   |   |-- output.cpp/h           formatted output helpers
|   |   `-- main.cpp               amidisk CLI (uses CLI11)
|   `-- tests/                     GoogleTest suite
|
|-- data/
|   |-- test/                      fixture images (real handler-written volumes)
|   |-- drivers/                   pfs3aio, FastFileSystem, SmartFilesystem
|   `-- bootblocks/                standard bootblocks (1x, 2x3x)
|
`-- doc/                           architecture, performance, user guide
```

## Testing

### Python

```sh
python3 amidisk_python/tests/smoke.py
# or the full unit suite:
PYTHONPATH=amidisk_python/src python3 -m unittest discover -s amidisk_python/tests -t amidisk_python
```

### C++

```sh
cd amidisk_cpp && cmake --build build && ctest --test-dir build --output-on-failure
```

The C++ test suite (GoogleTest) covers: block devices, RDB read/write,
FFS read/modify/format/check, PFS3 read/write, SFS read/write, RDB
rescue, archive extraction, bitmap flushing, and performance features.

Write tests only ever run on temporary copies. The images in `data/`
are never modified by the test suite or by read-only CLI commands.
