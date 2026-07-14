# amidisk — C++ implementation

High-performance C++20 port of the amidisk toolkit. Functional parity
with the Python reference implementation (`../amidisk_python`), sharing the
same on-disk format knowledge and architecture described in
[`../doc/amiga-disk-tool-architecture.md`](../doc/amiga-disk-tool-architecture.md).
See the [project root README](../README.md) for the full support matrix.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# binary at build/bin/amidisk
```

Dependencies are fetched automatically by CMake `FetchContent`:
- [CLI11](https://github.com/CLIUtils/CLI11) v2.4.1 — command-line parsing
- [nlohmann/json](https://github.com/nlohmann/json) v3.11.3 — JSON output
- [GoogleTest](https://github.com/google/googletest) — unit tests

No manual installation of these is required — just CMake >= 3.16 and a
C++20 compiler (GCC 10+, Clang 12+, or MSVC 2019 16.11+).

### Docker (reproducible Linux build)

```sh
docker build -f docker/Dockerfile -t amidisk-linux .
docker run --rm amidisk-linux              # runs the test suite
docker run --rm amidisk-linux ./build/bin/amidisk --help
```

## CLI

The binary supports the same `VOLUME:Path/To/File` notation as the Python
tool. Run `amidisk --help` for the full list, or `amidisk <command> --help`
for details on a specific subcommand.

```sh
amidisk info IMAGE [VOL]
amidisk ls IMAGE [VOL:path] [-l]
amidisk cat IMAGE VOL:path/file
amidisk extract IMAGE VOL:path DEST [-r]
amidisk put IMAGE HOSTFILE VOL:path [-r] [--bulk] [--comment C] [--protect bits]
amidisk cp SRC IMAGE:VOL/path [-r] [--bulk]
amidisk mkdir IMAGE VOL:path [-p]
amidisk rm IMAGE VOL:path [-r]
amidisk mv IMAGE VOL:old VOL:new
amidisk check IMAGE [VOL] [--deep]

amidisk create IMAGE --size 500M [--adf] [--format LABEL] [--dostype ffs-intl]
amidisk format IMAGE LABEL [-v VOL] [-t 0x444F5303]
amidisk rdb-init IMAGE [--heads N --sectors N]
amidisk part-add IMAGE DH0 [--size 100M] [--bs 4096] [--bootable]
amidisk part-del IMAGE DH0 [--write]
amidisk fs-extract IMAGE -t 0x444F5303 DEST
amidisk fs-add IMAGE DRIVER -t 0x444F5303
amidisk bootblock IMAGE VOL BOOTCODE.bin
amidisk repair IMAGE [VOL] [--write]
amidisk rdb-scan IMAGE
amidisk rdb-rebuild IMAGE [OUTPUT] [--write]
```

All destructive commands (`repair`, `rdb-rebuild`, `part-del`) default to
a dry run and require `--write` to commit — matching the Python CLI.
The `sync` and `bench` commands are Python-only.

## Library

`libamidisk` is a static library with the same four-layer architecture as
the Python implementation:

```
L1  blkdev/   BlockDevice trait + backends
              ├── image_file   HDF / raw / ADF (sparse-friendly)
              ├── vhd          VHD fixed + dynamic read
              ├── part_blkdev  partition-scoped block device
              ├── overlay      copy-on-write (safety / dry-run)
              └── bulk_cache   write-coalescing for mass imports

L2  rdb/      RDB partition engine
              ├── blocks       RDSK / PART / FSHD / LSEG
              ├── rdisk        parse, create, partition CRUD, fs embed/extract
              └── rescue       lost-partition scanner + RDB rebuilder

L3  fs/       Filesystem engines (common Volume interface)
              ├── volume.h     VolumeInfo, Entry, CheckReport, WriteParams
              ├── ffs          OFS/FFS DOS\0-\7 (read/write/format/check/repair)
              ├── pfs3         PFS3 (read/write/format, pfs3aio structures)
              ├── sfs          SFS (read/write/format, AROS structures)
              └── util         shared helpers

    archives/ Archive streaming (tar native; lha/zip/rar/7z via host tools)
    image.*   DiskImage facade — detection, volume dispatch
    safety.*  overlay simulate / verify / commit
```

### Usage

```cpp
#include "image.h"
#include "fs/volume.h"

auto img = amidisk::DiskImage::open("work.hdf", /*read_only=*/false);
auto* vref = img->get_volume("DH0");
auto* vol  = vref->mount();              // returns amidisk::Volume*

auto info = vol->get_info();             // VolumeInfo: label, free, dostype
for (const auto& e : vol->list_dir("S"))
    std::cout << e.name_str() << "  " << e.size << "\n";

vol->makedirs("Data/Incoming");
std::vector<uint8_t> data = /* ... */;
amidisk::WriteParams wp;
wp.comment = "imported";
vol->write_file("Data/Incoming/x.bin", data, wp);
vol->rename("Data/Incoming/x.bin", "Data/x.bin");
vol->delete_path("Data/Incoming", /*recursive=*/true);

auto report = vol->check(/*deep=*/true);  // CheckReport: ok, files, dirs, errors
```

### Key types

| Type | Header | Purpose |
|---|---|---|
| `BlockDevice` | `blkdev/blkdev.h` | Abstract block I/O (read/write/flush/geometry) |
| `BulkWriteCache` | `blkdev/bulk_cache.h` | Write-back cache for bulk imports (RAII `BulkGuard`) |
| `OverlayBlockDevice` | `blkdev/overlay.h` | Copy-on-write overlay for safe simulation |
| `Volume` | `fs/volume.h` | Common interface for all filesystem engines |
| `VolumeInfo` | `fs/volume.h` | Structured volume metadata (label, free/used, dostype) |
| `Entry` | `fs/volume.h` | Directory entry (name, size, protect, comment, mtime) |
| `WriteParams` | `fs/volume.h` | Optional write metadata (protect, comment, mtime) |
| `CheckReport` | `fs/volume.h` | Filesystem check result (ok, counts, errors, warnings) |
| `RDisk` | `rdb/rdisk.h` | RDB orchestration: parse, create, partition CRUD |
| `DiskImage` | `image.h` | Top-level facade: open, detect layout, volume dispatch |

## Testing

```sh
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

The GoogleTest suite covers:
- Block devices (image file, VHD, partition, overlay)
- RDB read/write (create, add/delete partitions, fs embed/extract)
- FFS (read, modify, format, check, bitmap flush, perf features)
- PFS3 (read, write)
- SFS (read, write)
- RDB rescue (scan, rebuild)
- Archive extraction (tar, external tool delegation)
