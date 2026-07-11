# The C++ rewrite: from 30 seconds to 7 — a study in I/O strategy

The Python amidisk engine already pushed Amiga filesystems to 50-85% of
host I/O ceiling. Then came the question: what happens when we rewrite
it in C++?

The answer was not what we expected.

**TL;DR:** C++ alone delivered 2.3x. Switching to POSIX `pwrite` added
another 2x. Memory-mapped I/O squeezed out a final 25%. The 5x total
speedup came from *how* we talked to the kernel, not from eliminating
Python overhead.

## The workload

A stress test designed to expose every bottleneck at once:

- **Input:** 6.0 GB tar archive containing a WHDLoad library
- **Output:** 27 GB FFS-LNFS partition (4K blocks, DOS\7)
- **Content:** 238,544 files across 12,620 directories
- **Operation:** Extract entire archive, verify filesystem integrity

This workload hits directory creation (12,620 makedirs), small file
writes (median file ~25 KB), bitmap allocation pressure (27 GB of
blocks to track), and metadata consistency (the final `check()` pass
must find zero errors). Any corruption, any off-by-one, any
checksum miscalculation fails the test.

## The baseline: Python at 35 seconds

The Python engine — after seven rounds of optimization documented in
[filesystem-performance.md](filesystem-performance.md) — completes this
workload in 34.8 seconds:

| Metric | Value |
|--------|-------|
| Time | 34.77s |
| User CPU | 21.80s |
| System CPU | 8.90s |
| Memory | 280 MB |
| Files/second | ~6,860 |

The user/system split tells the story: 63% of wall time is spent in
Python userspace — interpreter overhead, object allocation, GC
pressure. The kernel sees only 8.9 seconds of actual I/O work.

This is where C++ should shine.

## First attempt: C++ with fstream — 2.3x faster

The initial C++ port used `std::fstream` for I/O — the portable,
batteries-included choice:

```cpp
class ImageFile : public BlockDevice {
    mutable std::fstream file_;
    
    void read(uint64_t offset, std::span<uint8_t> buf) const override {
        file_.seekg(offset);
        file_.read(reinterpret_cast<char*>(buf.data()), buf.size());
    }
    
    void write(uint64_t offset, std::span<const uint8_t> buf) override {
        file_.seekp(offset);
        file_.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
};
```

Results:

| Metric | Value | vs Python |
|--------|-------|-----------|
| Time | 15.4s (avg) | **2.3x faster** |
| Best run | 14.84s | — |
| User CPU | 3.6s | 6x less |
| System CPU | 10.0s | +12% |
| Memory | 280 MB | same |

The Python interpreter tax is gone — user CPU dropped from 21.8s to
3.6s. But system time *increased*. The fstream abstraction, with its
internal buffering, stream state management, and exception checking,
costs more kernel transitions than raw Python file I/O.

Still: 2.3x faster, fully portable, zero platform-specific code. For
many projects this would be the end of the story.

It wasn't the end of ours.

## Second attempt: POSIX pwrite — 4.5x faster

The fstream abstraction does work we don't need. It maintains separate
read and write positions. It buffers in userspace. It synchronizes
access for thread safety. For a single-threaded tool writing to
known offsets, this is pure overhead.

POSIX `pwrite()` is the minimal interface: one syscall, one offset, one
buffer, done.

```cpp
void write(uint64_t offset, std::span<const uint8_t> buf) override {
    const uint8_t* ptr = buf.data();
    size_t remaining = buf.size();
    while (remaining > 0) {
        ssize_t written = ::pwrite(fd_, ptr, remaining, offset);
        if (written < 0) throw BlockDeviceError(strerror(errno));
        offset += written;
        ptr += written;
        remaining -= written;
    }
}
```

Results:

| Metric | Value | vs Python | vs fstream |
|--------|-------|-----------|------------|
| Time | 9.0s (avg) | **3.9x faster** | **1.7x faster** |
| Best run | 7.70s | 4.5x | 1.9x |
| User CPU | 1.9s | 11x less | 1.9x less |
| System CPU | 5.8s | 35% less | 42% less |
| Memory | 270 MB | same | same |

The improvement is almost entirely in reduced syscall overhead. User
CPU halved (1.9s vs 3.6s) because we're not managing stream state.
System CPU dropped 42% because each write is a single kernel transition
instead of buffer-flush-sync gymnastics.

This is the "fast but still portable to any POSIX system" sweet spot.

## Third attempt: Memory-mapped I/O — 5.1x faster

For fully pre-allocated images (like the 27 GB HDF we're writing to),
there's one more level: map the entire file into virtual memory and let
the kernel handle I/O.

```cpp
void* mmap_base_;
size_t mmap_size_;

void write(uint64_t offset, std::span<const uint8_t> buf) override {
    std::memcpy(static_cast<uint8_t*>(mmap_base_) + offset, 
                buf.data(), buf.size());
}
```

No syscalls at all. Writes become `memcpy`. The kernel's page fault
handler and writeback daemon do the actual disk I/O asynchronously,
coalesced, optimally scheduled.

Results:

| Metric | Value | vs Python | vs pwrite |
|--------|-------|-----------|-----------|
| Time | 7.2s (avg) | **4.8x faster** | **1.25x faster** |
| Best run | 6.83s | 5.1x | 1.13x |
| User CPU | 1.9s | same | same |
| System CPU | 3.9s | 56% less | 33% less |
| Memory | **6.5 GB** | 23x more | 24x more |

The kernel is now doing I/O on its own schedule, grouping dirty pages,
issuing large sequential writes. System CPU dropped another 33% despite
the same user CPU — fewer interrupts, better batching.

The cost is memory: mmap requires the entire 27 GB file to be mapped,
which means 6.5 GB of resident pages at peak (the active working set).
This is the right trade-off for batch operations on powerful
workstations; it would be the wrong choice on a Raspberry Pi importing
to CF card.

## Summary: what each layer contributed

| Strategy | Best Time | Speedup | User CPU | Sys CPU | Memory | Portability |
|----------|-----------|---------|----------|---------|--------|-------------|
| Python | 34.77s | 1.0x | 21.8s | 8.9s | 280 MB | Everywhere |
| C++ fstream | 14.84s | 2.3x | 3.6s | 10.0s | 280 MB | Everywhere |
| C++ POSIX | 7.70s | 4.5x | 1.9s | 5.8s | 270 MB | POSIX (Unix/macOS/WSL) |
| C++ mmap | 6.83s | 5.1x | 1.9s | 3.9s | 6.5 GB | POSIX + pre-allocated images |

The waterfall of wins:

1. **Python → C++ fstream (2.3x):** Interpreter elimination, static
   dispatch, direct memory management. The "free lunch" of compilation.

2. **fstream → POSIX pwrite (1.9x):** Removed buffering abstraction
   overhead. One syscall per write, no stream state, no userspace
   buffer management.

3. **POSIX → mmap (1.25x):** Eliminated syscalls entirely. Kernel I/O
   scheduling, write coalescing, optimal device utilization.

4. **BulkWriteCache optimization (~5%):** Reusable flush buffer, span-
   based entries avoiding copies during sort, run coalescing reducing
   device ops.

## The bugs we found along the way

Performance work has a side effect: it exercises code paths that casual
testing never touches. Three correctness bugs surfaced during this
optimization pass:

### 1. Bitmap checksum at wrong offset (500+ MB volumes)

FFS file headers store their checksum at longword 5. Bitmap pages store
theirs at longword 0. Our `fix_checksum()` defaulted to offset 5.

On volumes under ~500 MB, the bitmap fits in a single page and the bug
was masked by how allocation patterns happened to fall. Above 500 MB,
the extended bitmap pages would pass checksum verification (the
*existing* checksum at long 0 was correct from format time) but become
corrupted when we "fixed" them by writing a new checksum to long 5.

**Symptom:** `check()` reported phantom free-vs-allocated mismatches
on large volumes.

**Fix:** `fix_checksum(0)` for bitmap pages, `fix_checksum(5)` for
everything else.

### 2. Expected-array indexing across bitmap pages

The consistency checker builds an "expected" bitmap from the directory
tree and compares it against the on-disk bitmap. The indexing math for
multi-page bitmaps was wrong:

```cpp
// WRONG: byte_idx without accounting for page boundaries
uint32_t byte_idx = (blk - 2) / 8;

// CORRECT: compute page and offset separately
uint32_t idx = blk - reserved_;
uint32_t pi = idx / bits_per_page_;
uint32_t off = idx % bits_per_page_;
uint32_t byte_idx = (1 + off / 32) * 4 + (3 - (off % 32) / 8);
```

**Symptom:** False positives ("block X in use but marked free") on any
volume with more than one bitmap page.

**Fix:** Proper page-boundary-aware index calculation matching the
Python reference.

### 3. Extension chain walking read wrong field

The `entry_blocks()` function, used during file deletion to find all
blocks belonging to a file, was reading longword 1 (parent pointer)
instead of longword -2 (extension chain link).

```cpp
// WRONG: next_ext = get_long(data, 1);  // parent pointer!
// CORRECT: next_ext = get_long(data, blk_longs - 2);  // extension link
```

**Symptom:** "cyclic extension chain detected" errors when deleting
files with extension blocks.

**Fix:** Walk the chain via the correct offset, matching Python's
`_entry_blocks()` implementation exactly.

All three bugs existed in the C++ code since initial port; they were
found because the performance test workload was large enough (27 GB,
238K files) to trigger edge cases that smaller tests never hit.

## When to use which strategy

The selection logic, implemented via `AMIDISK_IO` environment variable:

```
AMIDISK_IO=mmap    # Fastest. Pre-allocated images only. High memory use.
AMIDISK_IO=posix   # Fast, low memory. Unix/macOS/WSL. Default.
AMIDISK_IO=fstream # Portable fallback. Windows native, or any platform.
```

Decision tree:

1. **Working with sparse or growing images?** → `posix` or `fstream`
   (mmap requires knowing the final size upfront)

2. **Memory constrained (< 8 GB RAM)?** → `posix` (mmap maps the whole
   image)

3. **Windows without WSL?** → `fstream` (POSIX unavailable)

4. **Maximum speed, batch operations, workstation?** → `mmap`

5. **Default / unsure?** → `posix` (best balance)

## The final numbers

On the benchmark workload (6 GB tar → 27 GB FFS, 238K files):

### macOS (Apple Silicon, NVMe)

| Implementation | Time | vs Python baseline |
|----------------|------|-------------------|
| Python amidisk | 34.77s | — |
| C++ fstream | 14.84s | 2.3x faster |
| C++ POSIX pwrite | 7.70s | 4.5x faster |
| C++ mmap | 6.83s | **5.1x faster** |

### Linux (Docker on Apple Silicon)

| Strategy | Time | Files/second |
|----------|------|--------------|
| fstream | 24.0s | 9,900 |
| mmap | 24.2s | 9,850 |
| posix | 28.3s | 8,430 |

Note: Linux in Docker shows different characteristics due to
virtualized I/O. The fstream and mmap strategies perform similarly
because Docker's filesystem layer dominates the I/O pattern. On bare
metal Linux, expect results similar to macOS.

The C++ engine extracts files at **35,000 files/second** on native
macOS — compared to Python's 6,900 files/second on the same workload.

For context: native macOS `tar -x` of the same archive to APFS takes
50.7 seconds. The C++ amidisk engine, writing to a *vintage Amiga
filesystem format inside an image file*, is now **7.4x faster** than
the host OS extracting to its native filesystem.

## Conclusion

The performance journey from 35 seconds to 7 seconds breaks into three
distinct phases, each teaching a different lesson:

**Phase 1: The compiler wins (2.3x).** Recompiling Python to C++ with
identical algorithms eliminated interpreter overhead. This is the
"obvious" win everyone expects from a rewrite — and it's real, but
it's only 2.3x of a 5x total improvement.

**Phase 2: The syscall interface matters (1.9x).** Choosing the right
I/O abstraction — raw POSIX over iostream, positional writes over
seek+write — nearly doubled speed again. The algorithm didn't change;
only how we asked the kernel to do our I/O.

**Phase 3: Let the kernel drive (1.25x).** Memory mapping eliminated
syscalls entirely, letting the kernel's I/O scheduler optimize disk
access patterns. The final 25% came from getting out of the way.

The meta-lesson: **performance comes from matching your I/O pattern to
kernel expectations, not from micro-optimizing application code.** The
BulkWriteCache optimization — careful buffer management, run coalescing,
span-based structures — contributed about 5%. The I/O strategy selection
contributed 220%. Knowing where to look is everything.
