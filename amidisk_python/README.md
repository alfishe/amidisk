# amidisk — Python library + CLI

Python reference implementation of the amidisk toolkit: reliable
read/write access to Amiga hard-disk images (HDF), VHD images and floppy
ADFs on modern systems — full RDB partition handling, native OFS/FFS
read/write for all classic dostypes, native PFS3 and SFS read/write,
filesystem check/repair, and an RDB rescue tool for disks whose partition
table was overwritten.

This is the **reference implementation** — it defines the feature set and
on-disk semantics that the C++ port (`../amidisk_cpp`) matches. See the
[project root README](../README.md) for the full support matrix,
architecture overview, and dual-implementation details.

No external dependencies, Python >= 3.9.

## Install

```sh
pip install -e .                     # installs the 'amidisk' command
# or run without installing:
PYTHONPATH=src python3 -m amidisk.cli
```

## CLI

Paths use Amiga notation `VOLUME:Path/To/File`; the volume is a drive
name (`DH0`), a label (`Workbench`) or a partition index.

```sh
amidisk info IMAGE [VOL]             # containers, RDB, partitions, volumes
amidisk ls IMAGE [VOL:path] [-l]
amidisk cat IMAGE VOL:path/file
amidisk extract IMAGE VOL:path DEST [-r]     # preserves mtimes
amidisk put IMAGE HOSTFILE VOL:path [-r] [--bulk] [--comment C] [--protect bits]
amidisk cp SRC IMAGE:VOL/path [-r] [--bulk]  # archive extraction or file/dir copy
amidisk sync SRCDIR IMAGE:VOL/dest [--delete] [--dry-run]   # rsync-style sync
amidisk mkdir IMAGE VOL:path [-p]
amidisk rm IMAGE VOL:path [-r]
amidisk mv IMAGE VOL:old VOL:new
amidisk check IMAGE [VOL] [--deep]           # per-filesystem validation
amidisk bench IMAGE [VOL] [--limit-files N]  # read benchmark

amidisk create IMAGE --size 500M [--adf] [--format LABEL] [--dostype ffs-intl]
amidisk format IMAGE LABEL [--volume DH0] [--dostype ofs|ffs|...|0x444F5303]
amidisk rdb-init IMAGE [--heads N --sectors N]
amidisk part-add IMAGE DH0 [--size 100M] [--bs 4096] [--bootable] [--format LABEL]
amidisk part-del IMAGE DH0
amidisk fs-extract IMAGE                     # dump embedded FSHD/LSEG drivers
amidisk fs-add IMAGE DRIVER --dostype pfs3   # embed a bootable m68k driver
amidisk bootblock IMAGE VOL BOOTCODE.bin     # install custom bootblock
amidisk repair IMAGE [VOL] [--deep] [--write]
```

### Recovery tools — simulate first, write second

All dangerous operations run against an in-RAM copy-on-write overlay,
verify the outcome there, show you the evidence, and only touch the
image with `--write`:

```sh
amidisk rdb-scan IMAGE               # find lost partitions
amidisk rdb-rebuild IMAGE [--deep]   # simulate the rebuilt RDB
amidisk rdb-rebuild IMAGE --write    # commit (backup + verify)
amidisk repair IMAGE [VOL] [--deep]  # FFS bitmap rebuild
```

## Library

```python
from amidisk import open_image

with open_image("data/OS-3.2.3.vhd") as img:          # writable=True to write
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
`amidisk.rdb` (`RDisk`, block classes — a port of amitools' rdblib —
and `rescue`), `amidisk.fs.ffs` / `fs.pfs3` / `fs.sfs`,
`amidisk.sync`, `amidisk.tarreader`.

## Layout

```
amidisk_python/
  pyproject.toml             package metadata + 'amidisk' entrypoint
  src/amidisk/
    blkdev.py                L0/L1: image containers as block devices + COW overlay
    rdb/blocks.py            RDSK/PART/FSHD/LSEG structures (amitools rdblib port)
    rdb/rdisk.py             RDB orchestration: parse, create, partition CRUD
    rdb/rescue.py            lost-partition scanner + RDB rebuilder
    fs/ffs.py                native OFS/FFS engine (read/write/format/check/repair)
    fs/pfs3.py               native PFS3 engine (pfs3aio structures)
    fs/sfs.py                native SFS engine (AROS SFS structures)
    archives/                tar (native reader) + lha/zip/rar/7z (host-tool delegation)
    sync.py                  rsync-style incremental sync
    tarreader.py             streaming tar reader
    image.py                 DiskImage facade: detection, volume dispatch
    cli.py                   amidisk CLI
  tests/
    smoke.py                 integration test suite (uses ../../data/ images)
```

## Testing

```sh
python3 tests/smoke.py
# full unit suite:
PYTHONPATH=src python3 -m unittest discover -s tests -t .
```

PFS3 unit tests in `tests/pfs3/`, SFS in `tests/sfs/`. hst-imager interop
tests skip unless the `HST_IMAGER` environment variable points at the binary.

Write tests only ever run on temporary copies. The images in `../../data/`
are never modified by the test suite or by read-only CLI commands.
