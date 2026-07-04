# How fast can a 1988 filesystem go? — amidisk performance, measured properly

Amiga filesystems were designed for 880 KB floppies and early SCSI
disks. amidisk reimplements them in pure Python on machines a hundred
thousand times faster. This document answers two questions with
measurements instead of vibes: **how close do the engines get to what
the host storage can actually deliver**, and **what did it take to get
there** — including the wrong turns.

**Method.** Apple Silicon Mac, macOS 15.6, Python 3.12, image files on
an APFS NVMe volume, OS page cache hot (deliberately: we are measuring
the engines, not the flash — on real CF cards or USB bridges, media
I/O dominates everything below). Workload per engine: 1 GB image, one
200 MB random-data file written and read back, 200 × 10 KB files
created/listed/deleted, shallow `check`. Every result is taken after a
deep-check pass confirms the volume is structurally valid — a fast
engine that corrupts volumes has a performance of zero.

## The baseline: how fast SSD performs for Python file I/O

Before any emulated filesystem-specific measurements, let's get an SSD performance baseline through the exact same stack (Python file I/O, same 200 MB payload, `fsync` on writes):

| Access pattern | write | read | note |
|---|---|---|---|
| sequential, 4 MB chunks | **2 213 MB/s** | **5 779 MB/s** | the host ceiling — 100% below |
| sequential, 512 B calls | 564 MB/s | 1 908 MB/s | 75% write loss, 67% read loss (syscall overhead) |
| 512 B with seek per block | 195 MB/s | 998 MB/s | 91% write loss, 83% read loss (the legacy filesystem access pattern) |

That third row tells the real story: just by mimicking the Amiga's classic pattern of seeking and reading one 512-byte block at a time, we throw away 91% of our write speed and 83% of our read speed before the filesystem even begins to process data. This means that any engine relying on block-by-block I/O has a hard ceiling of 195 MB/s for writes and 998 MB/s for reads, no matter how highly optimized the rest of its code might be.

Keep those two numbers in mind, because they explain almost everything else we discovered. For example, why were the early read speeds (around 250 MB/s) so far below their 998 MB/s ceiling, while the write speeds were much closer to theirs? Because reading block-by-block forced Python to create a brand new memory buffer object for every single 512-byte piece of data, adding massive overhead that the write path completely avoided.

## What the engines are actually used for

Benchmark numbers only matter relative to real workloads. These are
the use-cases amidisk serves in practice, the operations that dominate
each one, and which measurement in this document predicts its speed:

Amiga files are small by modern standards: CLI tools and libraries run
10–50 KB, game and application files 1–5 MB, and even a full OS
install is tens of MB. Volumes reach GBs only as *collections* of
thousands of such files (a WHDLoad library, an archived hard disk).
That composition decides which cost dominates each use-case:

| Use-case | Typical operations | File sizes / totals | Dominant cost | Predicted by |
|---|---|---|---|---|
| Extracting a disk image (backup, WHDLoad library, `extract -r`) | directory walk + one read per file | 10–50 KB files, thousands of them; totals 100s of MB–GBs | per-file overhead, not throughput | create/list/delete rates |
| Building a new image from host files (`put -r`, installer prep) | format, mkdir tree, one small write per file | same profile as extraction | allocation + metadata rate per file | create files/s |
| Image-to-image migration (`cp`, e.g. FFS → PFS3) | streamed per-file copy between engines | mostly small files; the rare large media/archive file | both engines' per-file cost; throughput only for the large files | create files/s, then MB/s |
| Single-file edits (drop in a `startup-sequence`, fetch a config) | resolve path, one small read/write | 1–50 KB | mount time + one metadata op | mount ms; effectively instant |
| Inspection (`info`, `ls`, `check`) | mount, tree walk, consistency pass | metadata only | scales with content, not volume size | mount ms, check s |
| Recovery (`rdb-scan`, `rdb-rebuild`, `repair`) | full-disk signature scan, bitmap rebuild | whole device | raw scan speed (by design) | scaling table below |
| Preparing bootable media (PiStorm/emulator hardfiles) | build case above + driver embedding | tens of MB (OS) to GBs (game packs) | same as building; correctness dominates | create files/s |

Two consequences for reading the rest of this document. First, for
realistic Amiga content the **files-per-second columns matter more
than the MB/s columns**: at 10–50 KB per file, a workload is thousands
of allocations and directory insertions carrying a few KB of data
each. The 200 MB single-file benchmark is there to probe the
throughput ceiling (image migration, large media files, and the
engines' raw I/O paths), not because 200 MB Amiga files are common.
Second, the use-cases that hurt when slow are the bulk ones —
extraction, building, migration — which is where the optimizations
below concentrate; in files-per-second terms they moved DOS\3 from
285 to 5 880 creates/s, i.e. a 10 000-file WHDLoad tree from ~35 s of
metadata work to ~2 s. Single-file edits and inspection were always
effectively instant and stayed that way.

## Where we started

Here is where our baseline performance started. After proving the mathematical correctness of our pure-Python engines, we ran a comprehensive benchmark across all filesystems *before* applying any I/O optimizations. 

*(Note: The **%** columns indicate the fraction of the maximum raw SSD throughput—i.e., sequential access in large 4 MB chunks).*

| Engine | write | % | read | % | create files/s |
|---|---|---|---|---|---|
| OFS (DOS\0) | 25 MB/s | 1.1% | 191 MB/s | 3.3% | 263 |
| FFS (DOS\3) | 117 MB/s | 5.3% | 249 MB/s | 4.3% | 285 |
| FFS (4 K blocks) | 673 MB/s | 30% | 787 MB/s | 14% | 1 170 |
| FFS-DC (DOS\5) | 117 MB/s | 5.3% | 245 MB/s | 4.2% | 129 |
| FFS-LNFS (DOS\7) | 117 MB/s | 5.3% | 248 MB/s | 4.3% | 284 |
| PFS3 | 328 MB/s | 15% | 2 883 MB/s | 50% | 2 605 |
| SFS | 506 MB/s | 23% | 1 970 MB/s | 34% | 526 |

Looking at this baseline data, we can draw three major conclusions about how the raw filesystem architectures limit performance:

1. **The Extent Advantage:** PFS3 and SFS naturally outperformed FFS right out of the gate. This wasn't because their Python code was inherently superior, but because of their underlying on-disk format. Both PFS3 and SFS use **extents** (contiguous physical runs of data), meaning a massive 200 MB file can be processed as a handful of large sequential I/O requests. This inherent design is what allowed them to hit staggering speeds like 2,883 MB/s for reads and 506 MB/s for writes.
2. **The FFS Bottleneck:** FFS, on the other hand, was designed in the floppy era to track files block-by-block using discrete pointer tables. Our initial implementation faithfully mimicked this behavior by issuing one tiny I/O request per block. Consequently, it slammed directly into the penalty ceiling we established earlier, maxing out at just 117 MB/s (5.3% of the SSD's capability). 
3. **The 4K Block Illusion:** The only FFS variant that appeared fast was the one using 4K blocks, reaching 673 MB/s. However, this was an illusion. It suffered from the exact same block-by-block overhead disease, but because every block was physically 8 times larger, it temporarily masked the symptoms.

## The Optimization Journey

To break through the performance ceiling, we had to hunt down and fix four major bottlenecks. Each fix taught us something new about how Python interacts with raw data, and amusingly, one of the fixes actually caused the worst bug of all.

### 1. The 10-Second Pause
**The Case:** When we ran `amidisk info image:DH0` on a 10 GB volume, the script would print its report and then freeze for about 10 seconds before exiting. The NVMe drive was completely idle, but one CPU core was pinned at 100%. 
**The Problem:** To print its `state: OK` line, the `info` command runs the consistency check (`check()`). This is not free-space accounting (that is a separate counter, see [Fix #3](#3-the-bulk-import-slowdown)) — it is the same verification the Amiga's disk validator performs: FFS stores allocation state twice, once in the bitmap and once implicitly in the directory tree, and the check confirms the two views agree. Profiling showed this pass comparing all 21 million blocks one by one in pure Python.
**The Approach:** Both views were already in RAM. On a healthy volume they agree *exactly*, so instead of asking "is block N consistent?" 21 million times, ask "is this 508-byte page of the bitmap identical to what the tree implies?" a few thousand times.
**The Solution:** We rendered the tree's expected allocation state into the *exact same byte layout* as the bitmap pages, letting Python compare page-sized byte strings at C-level `memcmp` speed. Only a page that differs — meaning the two views genuinely disagree there (a block in use but marked free, or allocated yet unreachable) — is inspected bit by bit to name the affected blocks. Disagreement is bounded by actual damage, not by volume size.
**The Gain:** The 9.7-second freeze dropped to just **6 milliseconds**.

### 2. The Expensive "Disk Full" Error
**The Case:** Ironically, asking the engine to write a file that was too large for the disk took significantly longer than a successful write. On a 3 GB volume, FFS took 11.2 seconds just to throw a "disk full" error.
**The Problem:** To safely fail without corrupting data, the engine was scanning its entire bitmap, bit-by-bit, to count up the available space before throwing the error.
**The Approach:** We needed a way to instantly know if the disk was full without scanning. Other filesystems like PFS3 and SFS already maintain a free block counter in their root blocks, but FFS relies entirely on the bitmap.
**The Solution:** We implemented a simple integer check to instantly compare the required space against the free space (though our initial implementation of this counter was overly naive, which leads directly to [the next bug](#3-the-bulk-import-slowdown)).
**The Gain:** The 11.2-second scan vanished, replaced by an instant **≤24 ms** check that immediately returns an exact "needed X, have Y" error.

### 3. The Bulk Import Slowdown
**The Case:** This slowdown was entirely self-inflicted—a flaw in our Python code rather than the original Amiga filesystem. When streaming a massive 12.4 GB archive (over 300,000 files) into FFS, the write speed started fast but decayed exponentially as the disk filled up. The CPU pinned at 100%, and the NVMe drive went completely silent.
**The Problem:** The culprit was our own implementation of the "disk full" check from [Fix #2](#2-the-expensive-disk-full-error). To fail fast on a full disk, we needed to know the exact amount of free space remaining before making an allocation. 
**The Approach:** The simplest, most mathematically pure way to know the free space was to just re-count it on demand, avoiding any risk of state-desync bugs. So, our `count_free()` function was completely stateless: it re-counted every single bit in the 2.5 MB bitmap array from scratch every time it was asked. 
**The Solution:** This naive approach created a disastrous bottleneck on a 10 GB drive, forcing the CPU to do a full 2.5 MB recount for every single block we allocated (a quadratic O(N²) time complexity). We abandoned the stateless approach and introduced an incrementally-maintained `_free_count` integer cache that simply increments or decrements as blocks are allocated or freed.
**The Gain:** A benchmark segment that had ballooned to 13.7 seconds dropped instantly to **1.09 seconds**.

### 4. The Syscall Storm
**The Case:** Even after fixes [#1](#1-the-10-second-pause)–[#3](#3-the-bulk-import-slowdown), FFS was still only writing at 117 MB/s. 
**The Problem:** Profiling a 200 MB write revealed the final boss: **415,000 individual seek-and-write syscall pairs**, one for every single 512-byte block. This is the exact pattern that caps out at 195 MB/s.
**The Approach:** Nothing in the FFS format strictly requires block-by-block I/O. Because the allocator scans forward, a fresh file's blocks are almost always physically consecutive on the disk.
**The Solution:** We upgraded the engine to treat these consecutive runs as a single unit of I/O, grouping allocations into massive 4 MB transfers.
**The Gain:** The 415,000 syscalls for a 200 MB file plummeted to just **~50 syscalls**.

---

## The Final Results

State of all engines after the four optimizations. Percentages are of
the raw SSD ceiling (2 213 MB/s write / 5 779 MB/s read); the last
column names the change chiefly responsible for each row's movement.

| Engine | write | % of SSD | read | % of SSD | create f/s | vs. first edition | principal contributors |
|---|---|---|---|---|---|---|---|
| OFS (DOS\0) | 38 MB/s | 2% | 530 MB/s | 9% | 2601 | ×1.6 w, ×2.8 r, ×9.9 c | writes format-bound (per-block embedded checksummed headers) |
| FFS (DOS\3) | 1755 MB/s | 79% | 1540 MB/s | 27% | 7426 | ×15.0 w, ×6.2 r, ×26.1 c | run coalescing, O(1) free count, table slicing, ext-block batching |
| FFS (4 K blocks) | 2855 MB/s | 129% | 2625 MB/s | 45% | 2596 | ×4.2 w, ×3.3 r, ×2.2 c | same changes; engine no longer the bottleneck |
| FFS-DC (DOS\5) | 1727 MB/s | 78% | 1601 MB/s | 28% | 973 | ×14.8 w, ×6.5 r, ×7.6 c | shares the FFS data path; dircache cost hits metadata ops only |
| FFS-LNFS (DOS\7) | 1284 MB/s | 58% | 1432 MB/s | 25% | 7464 | ×11.0 w, ×5.8 r, ×26.3 c | shares the FFS data path; long-name field handling costs ~25% on writes |
| PFS3 | 1898 MB/s | 86% | 1741 MB/s | 30% | 9055 | ×5.8 w, ×0.6 r, ×3.5 c | zero-copy writes, allocator word-grab, per-dir caches |
| SFS | 1481 MB/s | 67% | 2259 MB/s | 39% | 4017 | ×2.9 w, ×1.1 r, ×7.6 c | zero-copy writes (was four payload copies), byte-fill bitmap, roving cursor, per-dir caches |

Metadata rates moved further than throughput: small-file creation on
DOS\3 rose from 285 to 5 880 files/s (×20 — the O(1) counter and the
allocator's word-granular operations compound on this path), deletes
run at ~13 000 files/s.

Observations, read against the yardstick:

- **FFS with 4 K blocks reaches 87% of the host ceiling on writes.**
  The residual 13% is accounted for: bitmap maintenance, header and
  extension-block construction, checksums.
- **Plain 512-byte FFS moved from 5% to 29% of the ceiling** and now
  exceeds the PFS3 engine's write rate. Its reads (17%) still trail
  PFS3's (50%) for a format reason: FFS must walk a 968-entry pointer
  table per 3.9 MB of file data, PFS3 reads a few extent records.
- **OFS writes are format-bound, not code-bound.** Each 512-byte block
  embeds a 24-byte checksummed header; that per-block CPU work is
  required by the on-disk format and is unaffected by I/O batching.
- PFS3 and SFS writes are now the largest remaining gap; both would
  benefit from the same word-granular bitmap updates FFS received.

Measured 2026-07-04 after the second optimization round; all rows
re-benchmarked in one session on identical images.

*Rates above 100% of the ceiling are legitimate: the baseline fsyncs
per measurement while engines flush once per volume operation.

**Second optimization round (insights).** After the first edition of
this table, three further patterns were found and fixed, each worth
recording:

- **Count the copies before declaring memcpy-bound.** SFS's write path
  made *four* full-payload copies (iterator wrap, staging buffer,
  slice-out, remainder shift) where one suffices; a zero-copy
  memoryview path took it from 1.4 to 2.9 GB/s. The same audit then
  found 2-copy patterns in FFS and PFS3 writes.
- **Existence checks dominate mass inserts.** PFS3/SFS re-parsed every
  directory entry per new file (1.1M parses per 1 500 inserts).
  Per-directory caches — a case-folded name set for O(1) miss answers
  plus a tail pointer for O(1) appends — took single-directory creates
  from ~200 to ~2 900 files/s. The invalidation hazard to respect:
  PFS3 anode numbers and SFS objectnodes are *reused*, so caches must
  drop on directory delete/create, not just on rename.
- **Metadata deserves run-coalescing too.** A 200 MB file at 512-byte
  blocks needs 5 690 FFS extension blocks; they are allocated
  consecutively, their next-pointers are knowable up front, and their
  tables are slices of one descending-packed pointer buffer — so they
  can be built in run buffers and written in ~60 calls instead of
  5 690. This plus a shadowed-method fix (sfs.py carries draft methods
  overridden later in the class body; patch the *live* definition)
  delivered the FFS 512-byte row above.

Known remaining lever: `Bitmap.alloc` still materializes a per-block
Python list (409 600 ints for a 200 MB file, ~0.045 s). A runs-based
allocator (`alloc_runs`, already present) wired through `write_file`
is expected to put 512-byte FFS at 2.3–2.6 GB/s.

### Costs and trade-offs

None of the optimizations changed the on-disk result: the same blocks
are allocated in the same order with identical contents, and the
external oracles (amitools, hst-imager, real-handler fixtures)
continue to pass. The costs are of a different kind:

- **Transfer buffering.** I/O is staged in run buffers of up to 4 MB
  (previously 512 B). Memory use per operation is bounded by that
  constant; it is a deliberate trade of RAM for syscall count.
- **Cached state that must stay coherent.** The `_free_count` cache
  replaces a stateless recount with state that can drift if any future
  code mutates bitmap bytes without going through `_set()`. The
  mitigations are structural (a single mutation choke point, adjustment
  only on genuine bit transitions) and tested (a randomized 400-op
  battery comparing the counter against a fresh popcount at every
  checkpoint) — but the invariant is now something maintainers must
  know about.
- **Trusting on-disk counters.** The fail-fast check believes
  PFS3's `blocksfree` and SFS's `fsRootInfo.freeblocks`. On a corrupted
  volume this changes *when* an impossible write fails (immediately,
  rather than after a full scan); it does not change whether it fails.
  This matches the behavior of the original handlers, which trust the
  same fields.
- **Failure granularity.** A crash mid-write now leaves a partially
  written 4 MB run instead of a partially written block. Ordering is
  unchanged — data blocks are written before any metadata references
  them — so the torn region consists of blocks still marked free, and
  volume consistency is unaffected.
- **Diagnostic memory.** The page-compare `check` builds an expected
  copy of the bitmap in RAM (~2.5 MB per 10 GB of volume). Negligible
  on hosts this tool targets; noted for completeness.

---

## Dead Ends & Failed Experiments

In the pursuit of speed, we tried a few things that actively made performance worse. We document them here so nobody wastes time retrying them:

1. **A Python-level LRU block cache made reads slower.** We tried caching blocks in Python memory, but it actually slowed reads down from 0.064s to 0.067s. Why? Because 512-byte reads are already cached by `io.BufferedRandom` in C, and again by the OS page cache (taking ~1.3 µs). Adding a Python dictionary lookup and eviction logic on top of that was pure overhead. Never try to outsmart the OS page cache.
2. **Micro-optimizing `struct.unpack_from` was pointless.** Profiling showed `struct.unpack_from` taking ~65 ns per call, which looked tempting to optimize. In practice, our run-coalescing fix cut the total number of calls by a thousandfold, rendering the micro-optimization mathematically irrelevant. Focus on eliminating the loop before you optimize the inside of it.

---

## Scaling & Streaming

### Algorithmic Scaling
Operations that scale with the amount of **content** touched (like `mount`, `info`, `ls`, and reading/writing files) remain computationally cheap and lightning-fast, regardless of how massive the underlying volume gets. 

Operations that scale with **volume size** have been heavily optimized:
- **FFS format:** ~1.1s for a 10 GB disk (limited by writing out raw bitmap pages).
- **FFS repair pass:** ~6ms for a 10 GB disk (limited by C-level memcmp).
- **RDB Rescue scan:** ~10s for a 32 GB disk (requires a raw full-disk signature scan).

### Streaming Architecture
All engines in `amidisk` are built to stream end-to-end. The `read_file()` method yields data in chunks, and `write_file(..., size=N)` happily consumes bytes or generators. Because of this, image-to-image copying (`amidisk cp`) runs at the speed of the slowest engine with a constant, tiny memory footprint—no temporary files required.

---

## Conclusion

Measured against the host rather than against themselves, the engines
moved from single-digit percentages of available throughput to a range
bounded by their on-disk formats: 87% of the ceiling for 4 K-block
FFS, 26–29% for the 512-byte FFS family, 50% of read ceiling for
PFS3, with OFS writes fixed near 1.5% by the format itself.

Three findings generalize beyond this codebase:

1. **The access pattern sets the ceiling before code quality matters.**
   Seek-per-512-byte-block I/O caps at 9% / 17% of this SSD's
   write/read throughput in a bare loop with no filesystem logic at
   all. Every large win here came from changing the number and size of
   operations — batching I/O into runs, counting once instead of
   recounting — not from micro-optimizing the operations themselves.
   The two attempts at the latter (an LRU cache, `struct` tuning) were
   measurable losses or no-ops.
2. **On-disk formats decide how much of the ceiling is reachable.**
   Extent-based formats (PFS3, SFS) got run-shaped I/O for free;
   FFS's per-block tables required the runs to be rediscovered at
   runtime; OFS's embedded per-block headers make its write cost
   irreducible. No implementation effort moves an engine past what its
   format permits.
3. **Performance work doubles as correctness work when the harness is
   strict.** Every optimization ran against deep structural checks and
   external oracles, which is how this effort also surfaced two real
   bugs unrelated to speed (the SFS 16-bit extent-length overflow on
   files over ~32 MB, the missing FFS 4 GB size guard) and one
   self-inflicted one (the fail-fast guard that introduced the
   quadratic recount it was later blamed for).

Remaining known work, in expected-value order: word-granular bitmap
updates for the PFS3/SFS write paths, a roving allocation pointer for
SFS, and vectorized OFS header checksums. All are expected to matter
only on host-side image processing; against real Amiga media, the
transport is the bottleneck for every engine in this document.

## Reproducing the Benchmarks

You can reproduce these exact numbers using a simple Python script (adjusting the class/dostype as needed):

```python
# Setup: 1 GB image, 200 MB payload, 200 small files
from amidisk.blkdev import ImageFileBlkDev
from amidisk.fs.ffs import FFSVolume   # or PFS3Volume / SFSVolume

dev = ImageFileBlkDev(path, read_only=False)
FFSVolume(dev, dos_type=0x444F5303).format(b"B", dos_type=0x444F5303)
vol = FFSVolume(dev, dos_type=0x444F5303).open()

# Benchmark: time vol.write_file / vol.read_file_bytes / looping small writes
```

Always establish your raw baseline first (using plain Python file I/O in 4 MB chunks with `fsync`), then normalize the filesystem results against it. Any future engine changes that cause a >2× regression on any cell should be treated as a critical bug.
