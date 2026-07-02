# amidisk — Amiga disk image toolkit

Python library + CLI for reliable read/write access to Amiga hard-disk
images (HDF), VHD images and floppy ADFs on modern systems: full RDB
partition handling, native OFS/FFS read/write for all classic dostypes,
native PFS3 and SFS readers, filesystem check/repair, and an RDB rescue
tool for disks whose partition table was overwritten.

Implements the native-engine part of `doc/amiga-disk-tool-architecture.md`.
No dependencies, Python ≥ 3.9. `pip install -e .` installs the `amidisk`
command (or run `python3 -m amidisk.cli`).

## Support matrix

| Layer | Read | Write | Notes |
|---|---|---|---|
| HDF / raw images | ✔ | ✔ | sparse-friendly |
| VHD fixed | ✔ | ✔ | as used by WinUAE |
| VHD dynamic | ✔ | – | convert to fixed to write |
| ADF (DD/HD floppies) | ✔ | ✔ | |
| RDB partition table | ✔ | ✔ | init / add / delete / rescue; FSHD+LSEG driver extraction |
| MBR-wrapped RDB (PiStorm/Emu68) | ✔ | – | auto-detected |
| OFS/FFS DOS\0–\3 | ✔ | ✔ | incl. international mode, any block size 512–32K |
| FFS DOS\4/\5 (dircache) | ✔ | ✔ | dircache chains maintained and verified |
| FFS DOS\6/\7 (long filenames) | ✔ | ✔ | LNFS: 107-char names, overflow comment blocks |
| PFS3 (PFS\3, PDS\3, AFS\1) | ✔ | ✔ | native read/write/format, ported from pfs3aio |
| SFS (SFS\0) | ✔ | ✔ | native read/write/format, ported from AROS SFS / asfs |

All engines support **streaming**: `read_file()` yields chunks and
`write_file(path, source, size=...)` accepts bytes, file-likes, or chunk
iterators, so image-to-image copies (`amidisk cp`) never buffer whole
files or touch temp files.

Validated against: volumes written by AmigaOS 3.2.3 itself (the sample
images), amitools/xdftool (differential, both directions), hst-imager's
independent C# PFS3 implementation (reads our writes and formats
bit-perfect, and writes onto our volumes), and real pfs3aio /
SmartFilesystem handler-written fixtures in `data/test/` — including
strict handler-visibility checks for SFS (objectnode registration,
hashtable chains, hash16 values verified against the real handler's
output). PFS3/SFS mutation batteries assert zero space leak and that
untouched files stay bit-identical. Caveat: SFS has no second local
implementation to differential-test against (hst-imager lacks SFS), so
the final oracle for SFS writes remains a mount under the real handler.

Test suites: `python3 tests/smoke.py`, plus
`python3 -m unittest discover -s tests -t .` (PFS3 unit tests in
`tests/pfs3/`, SFS in `tests/sfs/`; hst-imager interop tests skip unless
`HST_IMAGER` points at the binary).

## CLI

Paths use Amiga notation `VOLUME:Path/To/File`; the volume is a drive
name (`DH0`), a label (`Workbench`) or a partition index.

```sh
amidisk info IMAGE                 # containers, RDB, partitions, volumes
amidisk ls IMAGE [VOL:path] [--json]
amidisk cat IMAGE VOL:path/file
amidisk extract IMAGE VOL:path DEST [-r]     # preserves mtimes
amidisk put IMAGE HOSTFILE VOL:path [-r] [--comment C] [--protect bits]
amidisk mkdir IMAGE VOL:path [-p]
amidisk rm IMAGE VOL:path [-r]
amidisk mv IMAGE VOL:old VOL:new
amidisk check IMAGE [VOL] [--deep]           # per-filesystem validation
amidisk fs-extract IMAGE                     # dump embedded FSHD/LSEG drivers
amidisk fs-add IMAGE DRIVER --dostype pfs3   # embed a bootable m68k driver
                                             # (auto on part-add from data/drivers/)

amidisk create IMAGE --size 500M [--adf] [--format LABEL] [--dostype ffs-intl]
amidisk format IMAGE LABEL [--volume DH0] [--dostype ofs|ffs|...|0x444F5303]
amidisk rdb-init IMAGE [--heads N --sectors N]
amidisk part-add IMAGE DH0 [--size 100M] [--bs 4096] [--bootable] [--format LABEL]
amidisk part-del IMAGE DH0 [--write]
```

### Recovery tools — simulate first, write second

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

amidisk repair IMAGE [VOL] [--deep]  # FFS bitmap rebuild: shows errors/free
amidisk repair IMAGE VOL --write     # space before vs after the simulation
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

PFS3/SFS repair is intentionally not native — use the original tools
(pfsDoctor, SFSsalv); `check` still validates their structures here.

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
and `rescue`), `amidisk.fs.ffs` / `fs.pfs3` / `fs.sfs`.

## Layout

```
amidisk/
  blkdev.py       L0/L1: image containers as block devices + COW overlay
  rdb/blocks.py   RDSK/PART/FSHD/LSEG structures (amitools rdblib port)
  rdb/rdisk.py    RDB orchestration: parse, create, partition CRUD
  rdb/rescue.py   lost-partition scanner + RDB rebuilder
  fs/ffs.py       native OFS/FFS engine (read/write/format/check/repair)
  fs/pfs3.py      native PFS3 reader (pfs3aio structures)
  fs/sfs.py       native SFS reader (AROS SFS structures)
  image.py        DiskImage facade: detection, volume dispatch
  cli.py          amidisk CLI
tests/smoke.py    test suite (uses data/ and data/test/ images)
data/test/        fixture images incl. real pfs3aio/SFS-written volumes
```

## Testing

```sh
python3 tests/smoke.py
```

Write tests only ever run on temporary copies. The images in `data/`
are never modified by the test suite or by read-only CLI commands.
