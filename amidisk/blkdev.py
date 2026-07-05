"""Layer 1 -- block devices.

Modeled on amitools' blkdev package (RawBlockDevice / ImageFile /
PartBlockDevice): everything above sees sector-addressed read/write plus
size and geometry, so image containers are swappable.
"""

import os
import struct


class BlockDeviceError(Exception):
    pass


class BlockDevice:
    """Abstract sector-addressed device."""

    block_bytes = 512
    num_blocks = 0
    read_only = True

    def read(self, lba, count=1):
        raise NotImplementedError

    def write(self, lba, data):
        raise NotImplementedError

    def flush(self):
        pass

    def close(self):
        pass

    def size_bytes(self):
        return self.num_blocks * self.block_bytes

    def _check_range(self, lba, count):
        if lba < 0 or lba + count > self.num_blocks:
            raise BlockDeviceError(
                "block range out of device: lba=%d count=%d num_blocks=%d"
                % (lba, count, self.num_blocks)
            )


class ImageFileBlkDev(BlockDevice):
    """Raw image file (HDF, ADF, .img): a flat array of sectors."""

    def __init__(self, path, read_only=True, block_bytes=512, nocache=False):
        self.path = path
        self.read_only = read_only
        self.block_bytes = block_bytes
        mode = "rb" if read_only else "r+b"
        self.fh = open(path, mode)
        self._nocache = nocache
        self._mm = None
        mm_env = os.environ.get("AMIDISK_MMAP")
        want_mm = mm_env == "1"
        if mm_env is None and not read_only and not nocache:
            # auto-policy: mmap fully-allocated images (page-fault writes
            # beat per-call syscalls ~20%; no allocation can fail, so no
            # SIGBUS-on-disk-full risk). Sparse images keep the syscall
            # path, whose disk-full failure mode is a clean exception.
            try:
                st = os.stat(path)
                want_mm = st.st_blocks * 512 >= st.st_size * 99 // 100
            except OSError:
                want_mm = False
        if want_mm and not read_only and not nocache:
            # opt-in memory-mapped I/O: page-fault writes + kernel
            # writeback batching beat per-call seek+write syscalls for
            # small scattered writes (measured 4x on cold allocated
            # files); reads become plain memory copies when cached
            import mmap as _mmap
            self.fh.seek(0, os.SEEK_END)
            fsize = self.fh.tell()
            if fsize > 0:
                try:
                    self._mm = _mmap.mmap(self.fh.fileno(), fsize)
                except (ValueError, OSError):
                    self._mm = None
        if nocache:
            # bypass the OS page cache (macOS F_NOCACHE): used by the
            # benchmark suite to measure true device-regime performance
            import fcntl
            if hasattr(fcntl, "F_NOCACHE"):
                fcntl.fcntl(self.fh.fileno(), fcntl.F_NOCACHE, 1)
        self.fh.seek(0, os.SEEK_END)
        self._size = self.fh.tell()
        self.num_blocks = self._size // block_bytes

    def read(self, lba, count=1):
        self._check_range(lba, count)
        if self._mm is not None:
            a = lba * self.block_bytes
            return self._mm[a : a + count * self.block_bytes]
        self.fh.seek(lba * self.block_bytes)
        data = self.fh.read(count * self.block_bytes)
        if len(data) != count * self.block_bytes:
            raise BlockDeviceError("short read at lba %d" % lba)
        return data

    def read_into(self, lba, out):
        """Read len(out) bytes starting at lba directly into a writable
        buffer (no intermediate bytes object). Returns bytes read."""
        self.fh.seek(lba * self.block_bytes)
        return self.fh.readinto(out)

    def pread(self, lba, nbytes):
        """Positional read returning fresh bytes: thread-safe (own fd,
        no shared seek), any byte length. Callers must flush writes
        first if coherence matters."""
        import os as _os
        self._ensure_raw_rfd()
        return _os.pread(self._raw_rfd, nbytes, lba * self.block_bytes)

    def _ensure_raw_rfd(self):
        import os as _os
        if getattr(self, "_raw_rfd", None) is None:
            self._raw_rfd = _os.open(self.path, _os.O_RDONLY)
            try:
                import fcntl as _fcntl
                if getattr(self, "_nocache", False) and hasattr(_fcntl, "F_NOCACHE"):
                    _fcntl.fcntl(self._raw_rfd, _fcntl.F_NOCACHE, 1)
            except ImportError:
                pass

    def pread_into(self, lba, out):
        """Positional read into a buffer: thread-safe (no shared seek),
        used by parallel read assembly. Coherent with buffered writes
        because callers flush first."""
        import os as _os
        self._ensure_raw_rfd()
        return _os.preadv(self._raw_rfd, [out], lba * self.block_bytes)

    def write(self, lba, data):
        if self.read_only:
            raise BlockDeviceError("device is read-only")
        if len(data) % self.block_bytes != 0:
            raise BlockDeviceError("write size not sector aligned")
        self._check_range(lba, len(data) // self.block_bytes)
        if self._mm is not None:
            a = lba * self.block_bytes
            self._mm[a : a + len(data)] = data
            return
        self.fh.seek(lba * self.block_bytes)
        self.fh.write(data)

    def flush(self):
        if self._mm is not None:
            self._mm.flush()
        self.fh.flush()
        os.fsync(self.fh.fileno())

    def close(self):
        if getattr(self, "_mm", None) is not None:
            self._mm.flush()
            self._mm.close()
            self._mm = None
        if getattr(self, "_raw_rfd", None) is not None:
            import os as _os
            _os.close(self._raw_rfd)
            self._raw_rfd = None
        self.fh.close()


VHD_COOKIE = b"conectix"
VHD_DYN_COOKIE = b"cxsparse"
VHD_TYPE_FIXED = 2
VHD_TYPE_DYNAMIC = 3
VHD_TYPE_DIFF = 4


class VHDBlkDev(BlockDevice):
    """Virtual Hard Disk container.

    Fixed VHDs (raw data + 512-byte footer) support read and write.
    Dynamic VHDs are supported read-only (BAT + per-block sector bitmaps).
    """

    def __init__(self, path, read_only=True):
        self.path = path
        self.block_bytes = 512
        mode = "rb" if read_only else "r+b"
        self.fh = open(path, mode)
        self.fh.seek(0, os.SEEK_END)
        file_size = self.fh.tell()
        if file_size < 512:
            raise BlockDeviceError("file too small for VHD")
        self.fh.seek(file_size - 512)
        footer = self.fh.read(512)
        if footer[0:8] != VHD_COOKIE:
            raise BlockDeviceError("no VHD footer cookie")
        self.disk_type = struct.unpack_from(">I", footer, 0x3C)[0]
        self.current_size = struct.unpack_from(">Q", footer, 0x30)[0]
        if self.disk_type == VHD_TYPE_FIXED:
            self.read_only = read_only
            self.num_blocks = self.current_size // 512
            # sanity: data region must fit before the footer
            if self.num_blocks * 512 > file_size - 512:
                self.num_blocks = (file_size - 512) // 512
        elif self.disk_type == VHD_TYPE_DYNAMIC:
            self.read_only = True
            if not read_only:
                raise BlockDeviceError(
                    "dynamic VHD write not supported; convert to fixed VHD or HDF first"
                )
            self._parse_dynamic(footer)
        else:
            raise BlockDeviceError("unsupported VHD type %d" % self.disk_type)

    def _parse_dynamic(self, footer):
        data_offset = struct.unpack_from(">Q", footer, 0x10)[0]
        self.fh.seek(data_offset)
        dyn = self.fh.read(1024)
        if dyn[0:8] != VHD_DYN_COOKIE:
            raise BlockDeviceError("no VHD dynamic header")
        table_offset = struct.unpack_from(">Q", dyn, 16)[0]
        max_entries = struct.unpack_from(">I", dyn, 28)[0]
        self.dyn_block_size = struct.unpack_from(">I", dyn, 32)[0]
        self.fh.seek(table_offset)
        raw = self.fh.read(max_entries * 4)
        self.bat = struct.unpack(">%dI" % max_entries, raw)
        # sector bitmap preceding each data block, padded to sectors
        bitmap_bytes = (self.dyn_block_size // 512 + 7) // 8
        self.bitmap_secs = (bitmap_bytes + 511) // 512
        self.num_blocks = self.current_size // 512

    def read(self, lba, count=1):
        self._check_range(lba, count)
        if self.disk_type == VHD_TYPE_FIXED:
            self.fh.seek(lba * 512)
            data = self.fh.read(count * 512)
            if len(data) != count * 512:
                raise BlockDeviceError("short read at lba %d" % lba)
            return data
        # dynamic: assemble sector by sector runs within VHD blocks
        out = bytearray()
        secs_per_block = self.dyn_block_size // 512
        remaining = count
        cur = lba
        while remaining > 0:
            bat_idx = cur // secs_per_block
            in_blk = cur % secs_per_block
            run = min(remaining, secs_per_block - in_blk)
            entry = self.bat[bat_idx] if bat_idx < len(self.bat) else 0xFFFFFFFF
            if entry == 0xFFFFFFFF:
                out += b"\x00" * (run * 512)
            else:
                off = (entry + self.bitmap_secs + in_blk) * 512
                self.fh.seek(off)
                chunk = self.fh.read(run * 512)
                if len(chunk) != run * 512:
                    raise BlockDeviceError("short read in dynamic VHD block")
                out += chunk
            cur += run
            remaining -= run
        return bytes(out)

    def write(self, lba, data):
        if self.read_only:
            raise BlockDeviceError("device is read-only")
        if len(data) % 512 != 0:
            raise BlockDeviceError("write size not sector aligned")
        self._check_range(lba, len(data) // 512)
        self.fh.seek(lba * 512)
        self.fh.write(data)

    def flush(self):
        self.fh.flush()

    def close(self):
        self.fh.close()


class PartBlockDevice(BlockDevice):
    """A partition presented as its own block device.

    Port of amitools PartBlockDevice: maps partition-relative sectors
    to the absolute sector range described by the PART block's DosEnvec
    (low_cyl..high_cyl, surfaces * blk_per_trk sectors per cylinder).
    """

    def __init__(self, raw_dev, start_sec, num_sec):
        self.raw = raw_dev
        self.start = start_sec
        self.num_blocks = num_sec
        self.block_bytes = raw_dev.block_bytes
        self.read_only = raw_dev.read_only

    def read(self, lba, count=1):
        self._check_range(lba, count)
        return self.raw.read(self.start + lba, count)

    def write(self, lba, data):
        self._check_range(lba, len(data) // self.block_bytes)
        self.raw.write(self.start + lba, data)

    def flush(self):
        self.raw.flush()


class OverlayBlockDevice(BlockDevice):
    """Copy-on-write overlay: writes land in RAM (spilling to a temp file
    past `max_ram_bytes`), the base device stays untouched. Used to
    *simulate* dangerous operations, verify the outcome, and only then
    commit the dirty blocks to the base device."""

    def __init__(self, base, max_ram_bytes=256 << 20):
        self.base = base
        self.block_bytes = base.block_bytes
        self.num_blocks = base.num_blocks
        self.read_only = False
        self._ram = {}            # lba -> block bytes
        self._max_ram_blocks = max_ram_bytes // self.block_bytes
        self._spill_fh = None
        self._spill_idx = {}      # lba -> offset in spill file

    def _get_dirty(self, lba):
        if lba in self._ram:
            return self._ram[lba]
        off = self._spill_idx.get(lba)
        if off is not None:
            self._spill_fh.seek(off)
            return self._spill_fh.read(self.block_bytes)
        return None

    def _put_dirty(self, lba, block):
        if lba in self._ram or len(self._ram) < self._max_ram_blocks:
            self._ram[lba] = block
            return
        if self._spill_fh is None:
            import tempfile

            self._spill_fh = tempfile.TemporaryFile(prefix="amidisk-overlay-")
        off = self._spill_idx.get(lba)
        if off is None:
            self._spill_fh.seek(0, os.SEEK_END)
            off = self._spill_fh.tell()
            self._spill_idx[lba] = off
        self._spill_fh.seek(off)
        self._spill_fh.write(block)

    def read(self, lba, count=1):
        self._check_range(lba, count)
        bb = self.block_bytes
        if not self._ram and not self._spill_idx:
            return self.base.read(lba, count)
        # fast path: no dirty block in range
        if all(
            (lba + i) not in self._ram and (lba + i) not in self._spill_idx
            for i in range(count)
        ):
            return self.base.read(lba, count)
        out = bytearray(self.base.read(lba, count))
        for i in range(count):
            d = self._get_dirty(lba + i)
            if d is not None:
                out[i * bb : (i + 1) * bb] = d
        return bytes(out)

    def write(self, lba, data):
        bb = self.block_bytes
        if len(data) % bb:
            raise BlockDeviceError("write size not sector aligned")
        self._check_range(lba, len(data) // bb)
        for i in range(len(data) // bb):
            self._put_dirty(lba + i, bytes(data[i * bb : (i + 1) * bb]))

    def dirty_blocks(self):
        return sorted(set(self._ram) | set(self._spill_idx))

    def dirty_bytes(self):
        return (len(set(self._ram) | set(self._spill_idx))) * self.block_bytes

    def commit(self, target):
        """Flush dirty blocks to `target` (a writable BlockDevice)."""
        for lba in self.dirty_blocks():
            target.write(lba, self._get_dirty(lba))
        target.flush()

    def verify_committed(self, target):
        """Read-back compare after commit; returns list of bad LBAs."""
        bad = []
        for lba in self.dirty_blocks():
            if target.read(lba) != self._get_dirty(lba):
                bad.append(lba)
        return bad

    def close(self):
        if self._spill_fh:
            self._spill_fh.close()


def open_blkdev(path, read_only=True):
    """Auto-detect the container format and open the image."""
    with open(path, "rb") as fh:
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
        if size >= 512:
            fh.seek(size - 512)
            footer = fh.read(512)
        else:
            footer = b""
    if footer[0:8] == VHD_COOKIE:
        return VHDBlkDev(path, read_only=read_only)
    return ImageFileBlkDev(path, read_only=read_only)


def fill_parallel(dev, mv, segs, seg_bytes=16 << 20, threads=4):
    """Fill memoryview `mv` from device runs in parallel.

    segs: iterable of (lba, buf_offset, nbytes). Runs are split into
    seg_bytes slices and read with positional pread on worker threads --
    the GIL releases during I/O and NVMe queues serve them concurrently.
    Falls back to sequential read_into when the device lacks pread_into.
    """
    if not hasattr(dev, "pread_into"):
        for lba, off, nbytes in segs:
            if dev.read_into(lba, mv[off : off + nbytes]) != nbytes:
                raise BlockDeviceError("short read at lba %d" % lba)
        return
    dev.fh.flush()  # coherence: pread bypasses the buffered writer
    work = []
    bb = dev.block_bytes
    for lba, off, nbytes in segs:
        while nbytes > 0:
            n = min(nbytes, seg_bytes)
            work.append((lba, off, n))
            lba += n // bb
            off += n
            nbytes -= n
    if len(work) == 1:
        lba, off, n = work[0]
        if dev.pread_into(lba, mv[off : off + n]) != n:
            raise BlockDeviceError("short read at lba %d" % lba)
        return
    from concurrent.futures import ThreadPoolExecutor

    def job(seg):
        lba, off, n = seg
        if dev.pread_into(lba, mv[off : off + n]) != n:
            raise BlockDeviceError("short read at lba %d" % lba)

    with ThreadPoolExecutor(max_workers=threads) as ex:
        for _ in ex.map(job, work):
            pass


def alloc_read_buffer(size):
    """Buffer for assembling large file reads: anonymous mmap for big
    sizes (kernel lazy-zero pages skip the upfront memset a bytearray
    pays), bytearray for small ones. Both support the buffer protocol;
    callers may hand either back to the API user."""
    if size >= (8 << 20):
        import mmap
        return mmap.mmap(-1, size)
    return bytearray(size)


class BulkWriteCache:
    """Write-back cache for bulk imports: absorbs the small metadata and
    file writes an import generates (header + data + directory rewrite
    per file) and flushes them as large, sorted, coalesced sequential
    writes. Activated by the engines' bulk() mode; the crash window is
    the same contract bulk() already documents.

    Small writes (<=8 blocks) live in a per-block dict so rewrites of
    the same block (directory updates, hash-chain links) merge for
    free. Large writes (data runs, ext chains) are kept whole in a
    sorted segment list -- the allocators hand out distinct blocks, and
    reads verify overlap instead of assuming it cannot happen.
    """

    def __init__(self, base, flush_threshold=512 << 20):
        self.base = base
        self.block_bytes = base.block_bytes
        self.num_blocks = base.num_blocks
        self.read_only = base.read_only
        self._small = {}            # blocknr -> bytes(block)
        self._big = []              # sorted [(start_blk, nblocks, bytes)]
        self._big_starts = []
        self._pending = 0
        self._threshold = flush_threshold

    def __getattr__(self, name):
        return getattr(self.base, name)

    def _big_lookup(self, blk):
        import bisect
        i = bisect.bisect_right(self._big_starts, blk) - 1
        if i >= 0:
            s, n, data = self._big[i]
            if s <= blk < s + n:
                return data[(blk - s) * self.block_bytes :
                            (blk - s + 1) * self.block_bytes]
        return None

    def write(self, lba, data):
        bb = self.block_bytes
        n = len(data) // bb
        if n <= 8:
            for i in range(n):
                self._small[lba + i] = bytes(data[i * bb : (i + 1) * bb])
        else:
            import bisect
            i = bisect.bisect_right(self._big_starts, lba)
            self._big.insert(i, (lba, n, bytes(data)))
            self._big_starts.insert(i, lba)
            for b in range(lba, lba + n):
                self._small.pop(b, None)
        self._pending += len(data)
        if self._pending >= self._threshold:
            self.flush_cache()

    def read(self, lba, count=1):
        bb = self.block_bytes
        if not self._small and not self._big:
            return self.base.read(lba, count)
        if count <= 256:
            parts = []
            base_run = None  # (start, n) of underlying blocks to fetch
            out = []
            for b in range(lba, lba + count):
                blk = self._small.get(b)
                if blk is None:
                    blk = self._big_lookup(b)
                out.append(blk)
            if all(p is not None for p in out):
                return b"".join(out)
            if all(p is None for p in out):
                return self.base.read(lba, count)
            under = self.base.read(lba, count)
            return b"".join(
                out[i] if out[i] is not None
                else under[i * bb : (i + 1) * bb]
                for i in range(count))
        # large read: cheapest correct path is flush-then-read-through
        self.flush_cache()
        return self.base.read(lba, count)

    def read_into(self, lba, out):
        if self._small or self._big:
            self.flush_cache()
        return self.base.read_into(lba, out)

    def pread_into(self, lba, out):
        if self._small or self._big:
            self.flush_cache()
        return self.base.pread_into(lba, out)

    def flush_cache(self):
        """Dump everything as sorted, coalesced sequential writes."""
        bb = self.block_bytes
        items = [(b, 1, d) for b, d in self._small.items()] + self._big
        items.sort(key=lambda t: t[0])
        i = 0
        MAXRUN = 32 << 20
        while i < len(items):
            start = items[i][0]
            parts = [items[i][2]]
            size = len(items[i][2])
            end = start + items[i][1]
            j = i + 1
            while (j < len(items) and items[j][0] == end
                   and size + len(items[j][2]) <= MAXRUN):
                parts.append(items[j][2])
                size += len(items[j][2])
                end += items[j][1]
                j += 1
            self.base.write(start, b"".join(parts) if len(parts) > 1
                            else parts[0])
            i = j
        self._small.clear()
        self._big = []
        self._big_starts = []
        self._pending = 0

    def flush(self):
        self.flush_cache()
        self.base.flush()

    def close(self):
        self.flush_cache()
        self.base.close()
