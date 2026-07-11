"""RDisk -- high-level RDB access, port of amitools' amitools/fs/rdb/RDisk.

Scans the RDSK anchor (anywhere in the first 16 device blocks, per spec),
walks the PART / FSHD / LSEG chains and exposes Partition and FileSystem
objects.
"""

from ..blkdev import PartBlockDevice
from .blocks import (
    RDBlock,
    PartitionBlock,
    DosEnvec,
    FSHeaderBlock,
    LoadSegBlock,
)

END = 0xFFFFFFFF  # chain terminator used on disk

RDB_LOCATION_LIMIT = 16
MAX_CHAIN = 1024  # guard against corrupted/cyclic block chains


class RDiskError(Exception):
    pass


class Partition:
    def __init__(self, blkdev, part_blk, num):
        self.blkdev = blkdev
        self.part_blk = part_blk
        self.num = num
        self.drv_name = part_blk.drv_name
        self.dos_env = part_blk.dos_env

    def get_num_cyls(self):
        return self.dos_env.num_cyls()

    def get_num_secs(self):
        return self.dos_env.num_secs()

    def get_num_blocks(self):
        """Number of filesystem blocks."""
        spb = max(self.dos_env.sec_per_blk, 1)
        return self.dos_env.num_secs() // spb

    def get_block_size(self):
        """Filesystem block size in bytes."""
        spb = max(self.dos_env.sec_per_blk, 1)
        return self.dos_env.size_block * 4 * spb

    def get_byte_offset(self):
        return self.dos_env.start_sec() * self.blkdev.block_bytes

    def get_byte_size(self):
        return self.dos_env.num_secs() * self.blkdev.block_bytes

    def get_dos_type_str(self):
        return self.dos_env.dos_type_str()

    def is_bootable(self):
        return self.part_blk.bootable()

    def create_blkdev(self):
        """Partition-relative block device (sector granularity)."""
        return PartBlockDevice(
            self.blkdev, self.dos_env.start_sec(), self.dos_env.num_secs()
        )

    def get_info(self):
        e = self.dos_env
        info = {
            "num": self.num,
            "drv_name": self.drv_name,
            "dos_type": self.get_dos_type_str(),
            "low_cyl": e.low_cyl,
            "high_cyl": e.high_cyl,
            "surfaces": e.surfaces,
            "blk_per_trk": e.blk_per_trk,
            "sec_per_blk": max(e.sec_per_blk, 1),
            "block_size": self.get_block_size(),
            "reserved": e.reserved,
            "size_bytes": self.get_byte_size(),
            "bootable": self.is_bootable(),
            "boot_pri": e.boot_pri,
            "no_mount": self.part_blk.no_mount(),
            "max_transfer": e.max_transfer,
            "mask": e.mask,
            "num_buffer": e.num_buffer,
        }
        
        # Verify bootblock checksum
        try:
            import struct
            start_sec = e.start_sec()
            # A standard bootblock is 1024 bytes (typically 2 x 512-byte sectors)
            blocks_to_read = max(1, 1024 // self.blkdev.block_bytes)
            raw = self.blkdev.read(start_sec, blocks_to_read)
            if len(raw) >= 1024:
                bb = raw[:1024]
                acc = 0
                for i in range(256):
                    if i == 1:
                        continue
                    acc += struct.unpack_from(">I", bb, i * 4)[0]
                    if acc > 0xFFFFFFFF:
                        acc = (acc & 0xFFFFFFFF) + 1
                expected_chksum = (~acc) & 0xFFFFFFFF
                got_chksum = struct.unpack_from(">I", bb, 4)[0]
                info["bb_chksum_match"] = (got_chksum == expected_chksum)
                info["bb_got_chksum"] = got_chksum
                info["bb_expected_chksum"] = expected_chksum
                info["bb_id"] = struct.unpack_from(">I", bb, 0)[0]
        except Exception:
            pass
            
        return info


class FileSystem:
    def __init__(self, blkdev, fshd_blk, num):
        self.blkdev = blkdev
        self.fshd_blk = fshd_blk
        self.num = num

    def get_dos_type_str(self):
        return self.fshd_blk.dos_type_str()

    def get_version_string(self):
        return "%d.%d" % self.fshd_blk.version_tuple()

    def get_data(self):
        """Concatenate the LSEG chain into the driver's hunk file data."""
        data = bytearray()
        blk_num = self.fshd_blk.dn_seg_list_blk
        seen = set()
        while blk_num != 0 and blk_num != 0xFFFFFFFF:
            if blk_num in seen or len(seen) > MAX_CHAIN:
                raise RDiskError("cyclic LSEG chain")
            seen.add(blk_num)
            lseg = LoadSegBlock(self.blkdev, blk_num)
            if not lseg.read():
                raise RDiskError("invalid LSEG block at %d" % blk_num)
            data += lseg.load_data
            blk_num = lseg.next
        return bytes(data)

    def get_info(self):
        return {
            "num": self.num,
            "dos_type": self.get_dos_type_str(),
            "version": self.get_version_string(),
            "patch_flags": self.fshd_blk.patch_flags,
            "seg_list_blk": self.fshd_blk.dn_seg_list_blk,
        }


class RDisk:
    def __init__(self, blkdev):
        self.blkdev = blkdev
        self.rdb = None
        self.parts = []
        self.fs = []

    @staticmethod
    def peek(blkdev):
        """Find the RDSK block in the first 16 blocks; return LBA or None."""
        limit = min(RDB_LOCATION_LIMIT, blkdev.num_blocks)
        for lba in range(limit):
            try:
                data = blkdev.read(lba)
            except Exception:
                return None
            if data[0:4] == b"RDSK":
                blk = RDBlock(blkdev, lba)
                if blk.read():
                    return lba
        return None

    def open(self):
        lba = self.peek(self.blkdev)
        if lba is None:
            return False
        self.rdb = RDBlock(self.blkdev, lba)
        self.rdb.read()
        self._read_partitions()
        self._read_filesystems()
        return True

    def _walk_chain(self, first, block_cls):
        result = []
        blk_num = first
        seen = set()
        while blk_num != 0 and blk_num != 0xFFFFFFFF:
            if blk_num in seen or len(seen) > MAX_CHAIN:
                raise RDiskError("cyclic %s chain" % block_cls.magic.decode())
            seen.add(blk_num)
            blk = block_cls(self.blkdev, blk_num)
            if not blk.read():
                raise RDiskError(
                    "invalid %s block at %d" % (block_cls.magic.decode(), blk_num)
                )
            result.append(blk)
            blk_num = blk.next
        return result

    def _read_partitions(self):
        blks = self._walk_chain(self.rdb.partition_list, PartitionBlock)
        self.parts = [Partition(self.blkdev, b, i) for i, b in enumerate(blks)]

    def _read_filesystems(self):
        blks = self._walk_chain(self.rdb.fs_header_list, FSHeaderBlock)
        self.fs = [FileSystem(self.blkdev, b, i) for i, b in enumerate(blks)]

    def get_partitions(self):
        return self.parts

    def get_filesystems(self):
        return self.fs

    def find_partition_by_drive_name(self, name):
        lo = name.lower()
        for p in self.parts:
            if p.drv_name.lower() == lo:
                return p
        return None

    # ------------------------------------------------------------------
    # RDB creation / partition CRUD (rdbtool semantics)
    # ------------------------------------------------------------------
    @classmethod
    def create(cls, blkdev, sectors=63, heads=None, rdb_cyls=None):
        """Initialize a fresh RDB on the device (destroys existing table)."""
        if blkdev.read_only:
            raise RDiskError("device is read-only")
        if heads is None:
            for h in (1, 2, 4, 8, 16, 32, 64, 128, 255):
                heads = h
                if blkdev.num_blocks // (h * sectors) <= 65535:
                    break
        cyl_blocks = heads * sectors
        cyls = blkdev.num_blocks // cyl_blocks
        if rdb_cyls is None:
            # reserve room for embedded filesystem drivers (FSHD/LSEG):
            # ~512 sectors (256 KB) unless the image is tiny, min 2 cyls
            want_secs = min(512, max(64, blkdev.num_blocks // 20))
            rdb_cyls = max(2, -(-want_secs // cyl_blocks))
        if cyls <= rdb_cyls:
            raise RDiskError("image too small for an RDB")
        rdb = RDBlock.new(blkdev, 0)
        rdb.host_id = 7
        rdb.block_bytes = blkdev.block_bytes
        rdb.flags = (
            RDBlock.FLAG_LAST | RDBlock.FLAG_LAST_LUN | RDBlock.FLAG_LAST_TID
            | RDBlock.FLAG_DISK_ID
        )
        rdb.badblock_list = END
        rdb.partition_list = END
        rdb.fs_header_list = END
        rdb.drive_init = END
        rdb.cylinders = cyls
        rdb.sectors = sectors
        rdb.heads = heads
        rdb.interleave = 1
        rdb.park = cyls
        rdb.write_precomp = cyls
        rdb.reduced_write = cyls
        rdb.step_rate = 3
        rdb.rdb_blocks_lo = 0
        rdb.rdb_blocks_hi = rdb_cyls * cyl_blocks - 1
        rdb.lo_cylinder = rdb_cyls
        rdb.hi_cylinder = cyls - 1
        rdb.cyl_blocks = cyl_blocks
        rdb.auto_park_seconds = 0
        rdb.high_rdsk_block = 0
        rdb.disk_vendor = "AMIDISK"
        rdb.disk_product = "IMAGE"
        rdb.disk_revision = "0.1"
        rdb.ctrl_vendor = ""
        rdb.ctrl_product = ""
        rdb.ctrl_revision = ""
        rdb.write()
        rd = cls(blkdev)
        rd.open()
        return rd

    def _used_rdb_blocks(self):
        used = {self.rdb.blk_num}
        for p in self.parts:
            used.add(p.part_blk.blk_num)
        for f in self.fs:
            used.add(f.fshd_blk.blk_num)
            blk_num = f.fshd_blk.dn_seg_list_blk
            guard = 0
            while blk_num not in (0, END) and guard < MAX_CHAIN:
                used.add(blk_num)
                lseg = LoadSegBlock(self.blkdev, blk_num)
                if not lseg.read():
                    break
                blk_num = lseg.next
                guard += 1
        return used

    def _alloc_rdb_block(self):
        used = self._used_rdb_blocks()
        hi = min(self.rdb.rdb_blocks_hi, self.blkdev.num_blocks - 1)
        for blk in range(self.rdb.rdb_blocks_lo, hi + 1):
            if blk not in used:
                return blk
        raise RDiskError("no free block in the RDB area")

    def _write_rdb(self):
        self.rdb.write()

    def free_cyl_range(self, num_cyls):
        """First gap of num_cyls cylinders inside the partitionable area."""
        taken = sorted(
            (p.dos_env.low_cyl, p.dos_env.high_cyl) for p in self.parts
        )
        cur = self.rdb.lo_cylinder
        for lo, hi in taken:
            if lo - cur >= num_cyls:
                return cur, cur + num_cyls - 1
            cur = max(cur, hi + 1)
        if self.rdb.hi_cylinder - cur + 1 >= num_cyls:
            return cur, cur + num_cyls - 1
        raise RDiskError(
            "no free space for %d cylinders (%.1f MB)"
            % (num_cyls, num_cyls * self.rdb.cyl_blocks * self.blkdev.block_bytes / 1e6)
        )

    def add_partition(
        self,
        drv_name,
        size_bytes=None,
        num_cyls=None,
        dos_type=0x444F5303,
        sec_per_blk=1,
        bootable=False,
        boot_pri=0,
        max_transfer=0x1FE00,
        mask=0x7FFFFFFE,
        num_buffer=30,
        reserved=2,
        low_cyl=None,
        high_cyl=None,
    ):
        if self.rdb is None:
            raise RDiskError("no RDB open")
        if self.find_partition_by_drive_name(drv_name):
            raise RDiskError("partition %r already exists" % drv_name)
        cyl_bytes = self.rdb.cyl_blocks * self.blkdev.block_bytes
        if low_cyl is not None and high_cyl is not None:
            if not (self.rdb.lo_cylinder <= low_cyl <= high_cyl <= self.rdb.hi_cylinder):
                raise RDiskError(
                    "cylinder range %d-%d outside partitionable area %d-%d"
                    % (low_cyl, high_cyl, self.rdb.lo_cylinder, self.rdb.hi_cylinder)
                )
            for p in self.parts:
                if not (high_cyl < p.dos_env.low_cyl or low_cyl > p.dos_env.high_cyl):
                    raise RDiskError("overlaps partition %s" % p.drv_name)
            lo, hi = low_cyl, high_cyl
        elif num_cyls is None:
            if size_bytes is None:
                # everything that is left
                num_cyls = None
                lo, hi = None, None
                best = 0
                taken = sorted(
                    (p.dos_env.low_cyl, p.dos_env.high_cyl) for p in self.parts
                )
                cur = self.rdb.lo_cylinder
                for tlo, thi in taken + [(self.rdb.hi_cylinder + 1, 0)]:
                    if tlo - cur > best:
                        best, lo, hi = tlo - cur, cur, tlo - 1
                    cur = max(cur, thi + 1)
                if not best:
                    raise RDiskError("no free space left")
            else:
                num_cyls = -(-size_bytes // cyl_bytes)
                lo, hi = self.free_cyl_range(num_cyls)
        else:
            lo, hi = self.free_cyl_range(num_cyls)

        blk = self._alloc_rdb_block()
        pb = PartitionBlock.new(self.blkdev, blk)
        pb.host_id = self.rdb.host_id
        pb.next = END
        pb.flags = PartitionBlock.FLAG_BOOTABLE if bootable else 0
        pb.dev_flags = 0
        pb.drv_name = drv_name
        env = DosEnvec()
        env.table_size = 16
        env.size_block = self.blkdev.block_bytes // 4
        env.surfaces = self.rdb.heads
        env.sec_per_blk = sec_per_blk
        env.blk_per_trk = self.rdb.sectors
        env.reserved = reserved
        env.low_cyl = lo
        env.high_cyl = hi
        env.num_buffer = num_buffer
        env.max_transfer = max_transfer
        env.mask = mask
        env.boot_pri = boot_pri
        env.dos_type = dos_type
        pb.dos_env = env
        pb.write()

        # append to the PART chain
        if self.rdb.partition_list in (0, END):
            self.rdb.partition_list = blk
        else:
            last = self.parts[-1].part_blk
            last.next = blk
            last.write()
        if blk > self.rdb.high_rdsk_block:
            self.rdb.high_rdsk_block = blk
        self._write_rdb()
        self._read_partitions()
        return self.find_partition_by_drive_name(drv_name)

    def add_filesystem(self, data, dos_type, version=(0, 0), patch_flags=0x180):
        """Embed a filesystem driver (Amiga hunk executable) as an
        FSHD + LSEG chain -- the mechanism the ROM boot strap uses to load
        handlers for dostypes it does not know (PFS3, SFS, ...)."""
        if self.rdb is None:
            raise RDiskError("no RDB open")
        for f in self.fs:
            if f.fshd_blk.dos_type == dos_type:
                raise RDiskError(
                    "filesystem for %s already embedded (v%s)"
                    % (f.get_dos_type_str(), f.get_version_string())
                )
        bb = self.blkdev.block_bytes
        per = bb - 20  # LSEG payload bytes per block
        nseg = (len(data) + per - 1) // per
        used = self._used_rdb_blocks()
        hi = min(self.rdb.rdb_blocks_hi, self.blkdev.num_blocks - 1)
        free = [b for b in range(self.rdb.rdb_blocks_lo, hi + 1) if b not in used]
        if len(free) < nseg + 1:
            raise RDiskError(
                "RDB area too small: need %d blocks for the driver, %d free "
                "(re-init with more reserved space: rdb-init --rdb-cyls N)"
                % (nseg + 1, len(free))
            )
        fshd_blk_num = free[0]
        seg_blks = free[1 : 1 + nseg]
        for i, blk in enumerate(seg_blks):
            chunk = data[i * per : (i + 1) * per]
            if len(chunk) % 4:
                chunk = chunk + b"\x00" * (4 - len(chunk) % 4)
            lseg = LoadSegBlock.new(self.blkdev, blk,
                                    size_longs=5 + len(chunk) // 4)
            lseg.host_id = self.rdb.host_id
            lseg.next = seg_blks[i + 1] if i + 1 < nseg else END
            lseg.load_data = chunk
            lseg.write()
        fshd = FSHeaderBlock.new(self.blkdev, fshd_blk_num)
        fshd.host_id = self.rdb.host_id
        fshd.next = END
        fshd.flags = 0
        fshd.dos_type = dos_type
        fshd.version = (version[0] << 16) | version[1]
        fshd.patch_flags = patch_flags
        fshd.dn_type = 0
        fshd.dn_task = 0
        fshd.dn_lock = 0
        fshd.dn_handler = 0
        fshd.dn_stack_size = 0
        fshd.dn_priority = 0
        fshd.dn_startup = 0
        fshd.dn_seg_list_blk = seg_blks[0] if seg_blks else END
        fshd.dn_global_vec = 0xFFFFFFFF  # -1, per patch_flags bit 8
        fshd.write()
        # append to the FSHD chain
        if self.rdb.fs_header_list in (0, END):
            self.rdb.fs_header_list = fshd_blk_num
        else:
            last = self.fs[-1].fshd_blk
            last.next = fshd_blk_num
            last.write()
        top = max([fshd_blk_num] + seg_blks)
        if top > self.rdb.high_rdsk_block:
            self.rdb.high_rdsk_block = top
        self._write_rdb()
        self._read_filesystems()
        return self.fs[-1]

    def delete_partition(self, drv_name):
        part = self.find_partition_by_drive_name(drv_name)
        if part is None:
            raise RDiskError("no partition %r" % drv_name)
        target = part.part_blk
        if self.rdb.partition_list == target.blk_num:
            self.rdb.partition_list = target.next if target.next != 0 else END
            self._write_rdb()
        else:
            prev = None
            for p in self.parts:
                if p.part_blk.blk_num == target.blk_num:
                    break
                prev = p
            prev.part_blk.next = target.next if target.next != 0 else END
            prev.part_blk.write()
        # scrub the orphaned block so scanners don't resurrect it
        self.blkdev.write(target.blk_num, b"\x00" * self.blkdev.block_bytes)
        self._read_partitions()

    def get_info(self):
        r = self.rdb
        return {
            "rdb_block": r.blk_num,
            "block_bytes": r.block_bytes,
            "flags": r.flags,
            "cylinders": r.cylinders,
            "heads": r.heads,
            "sectors": r.sectors,
            "cyl_blocks": r.cyl_blocks,
            "rdb_blocks_lo": r.rdb_blocks_lo,
            "rdb_blocks_hi": r.rdb_blocks_hi,
            "lo_cylinder": r.lo_cylinder,
            "hi_cylinder": r.hi_cylinder,
            "disk_vendor": r.disk_vendor,
            "disk_product": r.disk_product,
            "disk_revision": r.disk_revision,
        }
