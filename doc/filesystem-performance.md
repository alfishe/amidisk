# amidisk filesystem performance

Measured 2026-07-02 on an Apple Silicon Mac (macOS 15.6, Python 3.12,
image files on APFS, OS page cache hot). Numbers characterize the
*relative* cost of the native Python engines, not device throughput —
on real media, I/O dominates. Benchmark: 1 GB image per engine,
one 200 MB random-data file (write + read back), 200 × 10 KB files
in one directory (create / list / delete), shallow `check`.

## Read / write throughput and metadata rates

| Engine | format | mount | write MB/s | read MB/s | create f/s | list 200 | check | delete f/s |
|---|---|---|---|---|---|---|---|---|
| OFS (DOS\0) | 0.11 s | 6 ms | 25 | 191 | 263 | 1 ms | 0.30 s | 13 245 |
| FFS (DOS\3) | 0.11 s | 6 ms | 117 | 249 | 285 | 1 ms | 0.29 s | 13 675 |
| FFS (DOS\3, 4 K blocks) | 0.01 s | 1 ms | 673 | 787 | 1 170 | 2 ms | 0.05 s | 4 455 |
| FFS-DC (DOS\5) | 0.12 s | 5 ms | 117 | 245 | 129 | 1 ms | 0.29 s | 231 |
| FFS-LNFS (DOS\7) | 0.11 s | 6 ms | 117 | 248 | 284 | 2 ms | 0.29 s | 12 871 |
| PFS3 | 0.01 s | <1 ms | 328 | 2 883 | 2 605 | <1 ms | 1 ms | 2 191 |
| SFS | 0.02 s | <1 ms | 506 | 1 970 | 526 | 1 ms | 8 ms | 2 284 |

At 10 GB: FFS format 1.1 s (bitmap pages), PFS3 0.08 s, SFS 0.21 s;
mount stays in milliseconds for all engines.

### Why the numbers look the way they do

- **OFS writes are slow by design**: every 512-byte data block carries a
  24-byte header and checksum built per block in Python (~25 MB/s).
  FFS writes raw blocks (~117 MB/s); with 4 K blocks the per-block count
  drops 8× (~673 MB/s).
- **PFS3/SFS are fastest** because both use extent/run allocation: a
  contiguous 200 MB file is a handful of (start, length) runs, so reads
  become a few large sequential I/Os (GB/s from page cache) instead of
  per-block table walks. PFS3 metadata rates benefit from 1 KB reserved
  blocks holding many dir entries.
- **Dircache (DOS\5) trades mutation speed for Amiga-side listing
  speed**: every create/delete rebuilds the directory's cache chain
  (129 create/s, 231 delete/s vs ~285/13 000 on DOS\3). Prefer DOS\3 or
  DOS\7 unless the volume is for a machine that benefits from dircache.
- **FFS delete is extremely fast** (~13 000 f/s) because freeing is
  bitmap bit-sets; PFS3/SFS also update extent/anode structures.
- SFS large files are split into chained extents of at most 65 535
  blocks (u16 run length, ~32 MB at 512-byte blocks) — matching the
  on-disk format; throughput is unaffected.

## Scaling: content-proportional vs size-proportional

Operations that scale with **content** (files/dirs/data touched), not
volume size — cheap even on huge volumes: mount, `info`, `ls`,
read/write/delete, PFS3 and SFS `check`.

Operations that scale with **volume size**:

| Operation | cost | 10 GB example |
|---|---|---|
| FFS format | bitmap page creation | 1.1 s |
| FFS `check`/`repair` bitmap pass | page-compare (memcmp) per bitmap page | 6 ms |
| `rdb-scan` (rescue) | full-disk signature scan, by design | ~10 s/32 GB |

Two size-proportional hot spots were found and fixed while producing
this document:

| Path | before | after |
|---|---|---|
| FFS `check` bitmap consistency (10 GB, 21 M blocks) | 9.7 s (per-block Python loop) | 6 ms (page compare, bit-drill only on damage) |
| disk-full error, FFS (3 GB) | 11.2 s (scanned whole bitmap) | 24 ms (fail fast from free counter) |
| disk-full error, PFS3 (3 GB) | 12.6 s | <1 ms |
| disk-full error, SFS (3 GB) | 1.1 s | <1 ms |

Allocators also now skip fully-allocated 32-block words at word-compare
speed (FFS, PFS3), so nearly-full volumes don't degrade to per-bit
scanning.

## Streaming

All engines stream: `read_file()` yields block-sized chunks and
`write_file(..., size=N)` consumes iterators/file-likes, so
image-to-image copies (`amidisk cp`) run at the min of the two engines'
throughput with constant memory and no temp files.

## Reproducing

The benchmark script lives in this document's history; the quick
version:

```sh
python3 - <<'EOF'
# see tests/ for the harness pieces; 1 GB image, 200 MB payload,
# 200 small files -- prints one row per engine
EOF
```

Re-run after engine changes and update the table; treat >2× regressions
on any cell as a bug.
