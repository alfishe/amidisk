"""RDB on-disk block structures: RDSK, PART, FSHD, LSEG, BADB.

Port of amitools' amitools/fs/block/rdb/ block classes (RDBlock,
PartitionBlock, FSHeaderBlock, LoadSegBlock) with the same field names,
which in turn follow the AmigaOS NDK <devices/hardblocks.h> layout.
All values are big-endian longwords; every block carries a checksum:
the sum of the first `size` longwords must be zero.
"""

import struct


def checksum_ok(data, num_longs):
    s = 0
    for i in range(num_longs):
        s = (s + struct.unpack_from(">I", data, i * 4)[0]) & 0xFFFFFFFF
    return s == 0


def fix_checksum(data, num_longs, chk_loc=2):
    """Recompute the checksum longword in a mutable buffer."""
    struct.pack_into(">I", data, chk_loc * 4, 0)
    s = 0
    for i in range(num_longs):
        s = (s + struct.unpack_from(">I", data, i * 4)[0]) & 0xFFFFFFFF
    struct.pack_into(">I", data, chk_loc * 4, (-s) & 0xFFFFFFFF)


class RDBBaseBlock:
    """Common RDB block behavior: magic, size, checksum, longword access."""

    magic = b"????"

    def __init__(self, blkdev, blk_num):
        self.blkdev = blkdev
        self.blk_num = blk_num
        self.data = None
        self.valid = False

    # -- raw access -------------------------------------------------
    def _long(self, idx):
        return struct.unpack_from(">I", self.data, idx * 4)[0]

    def _slong(self, idx):
        return struct.unpack_from(">i", self.data, idx * 4)[0]

    def _put_long(self, idx, val):
        struct.pack_into(">I", self.data, idx * 4, val & 0xFFFFFFFF)

    def _bytes(self, off, size):
        return bytes(self.data[off : off + size])

    def _bstr(self, off, max_chars):
        n = self.data[off]
        if n > max_chars:
            n = max_chars
        return bytes(self.data[off + 1 : off + 1 + n])

    def _put_bstr(self, off, max_chars, val):
        val = val[:max_chars]
        self.data[off] = len(val)
        self.data[off + 1 : off + 1 + len(val)] = val
        # zero pad the rest of the field
        for i in range(off + 1 + len(val), off + 1 + max_chars):
            self.data[i] = 0

    @classmethod
    def new(cls, blkdev, blk_num, size_longs=64):
        """Create a fresh, zeroed block of this type in memory."""
        blk = cls(blkdev, blk_num)
        blk.data = bytearray(blkdev.block_bytes)
        blk.data[0:4] = cls.magic
        blk.size_longs = size_longs
        struct.pack_into(">I", blk.data, 4, size_longs)
        blk.valid = True
        return blk

    # -- I/O ---------------------------------------------------------
    def read(self):
        raw = self.blkdev.read(self.blk_num)
        self.data = bytearray(raw)
        self.valid = False
        if self.data[0:4] != self.magic:
            return False
        self.size_longs = self._long(1)
        if self.size_longs < 3 or self.size_longs * 4 > len(self.data):
            return False
        if not checksum_ok(self.data, self.size_longs):
            return False
        self.valid = True
        self._unpack()
        return True

    def write(self):
        self._pack()
        fix_checksum(self.data, self.size_longs)
        self.blkdev.write(self.blk_num, bytes(self.data))

    def _unpack(self):
        pass

    def _pack(self):
        pass


class RDBlock(RDBBaseBlock):
    """RDSK -- Rigid Disk Block, anchor of the partition table."""

    magic = b"RDSK"

    # rb_Flags
    FLAG_LAST = 0x01
    FLAG_LAST_LUN = 0x02
    FLAG_LAST_TID = 0x04
    FLAG_NO_RESELECT = 0x08
    FLAG_DISK_ID = 0x10
    FLAG_CTRL_ID = 0x20
    FLAG_SYNCH = 0x40

    def _unpack(self):
        self.host_id = self._long(3)
        self.block_bytes = self._long(4)
        self.flags = self._long(5)
        self.badblock_list = self._long(6)
        self.partition_list = self._long(7)
        self.fs_header_list = self._long(8)
        self.drive_init = self._long(9)
        # physical drive characteristics
        self.cylinders = self._long(16)
        self.sectors = self._long(17)
        self.heads = self._long(18)
        self.interleave = self._long(19)
        self.park = self._long(20)
        self.write_precomp = self._long(24)
        self.reduced_write = self._long(25)
        self.step_rate = self._long(26)
        # logical drive characteristics
        self.rdb_blocks_lo = self._long(32)
        self.rdb_blocks_hi = self._long(33)
        self.lo_cylinder = self._long(34)
        self.hi_cylinder = self._long(35)
        self.cyl_blocks = self._long(36)
        self.auto_park_seconds = self._long(37)
        self.high_rdsk_block = self._long(38)
        # drive identification
        self.disk_vendor = self._bytes(160, 8).rstrip(b"\x00 ").decode("latin-1")
        self.disk_product = self._bytes(168, 16).rstrip(b"\x00 ").decode("latin-1")
        self.disk_revision = self._bytes(184, 4).rstrip(b"\x00 ").decode("latin-1")
        self.ctrl_vendor = self._bytes(188, 8).rstrip(b"\x00 ").decode("latin-1")
        self.ctrl_product = self._bytes(196, 16).rstrip(b"\x00 ").decode("latin-1")
        self.ctrl_revision = self._bytes(212, 4).rstrip(b"\x00 ").decode("latin-1")

    def _put_padded(self, off, size, text):
        raw = text.encode("latin-1")[:size].ljust(size, b" ")
        self.data[off : off + size] = raw

    def _pack(self):
        self._put_long(3, self.host_id)
        self._put_long(4, self.block_bytes)
        self._put_long(5, self.flags)
        self._put_long(6, self.badblock_list)
        self._put_long(7, self.partition_list)
        self._put_long(8, self.fs_header_list)
        self._put_long(9, self.drive_init)
        for i in range(10, 16):
            self._put_long(i, 0xFFFFFFFF)
        self._put_long(16, self.cylinders)
        self._put_long(17, self.sectors)
        self._put_long(18, self.heads)
        self._put_long(19, self.interleave)
        self._put_long(20, self.park)
        self._put_long(24, self.write_precomp)
        self._put_long(25, self.reduced_write)
        self._put_long(26, self.step_rate)
        self._put_long(32, self.rdb_blocks_lo)
        self._put_long(33, self.rdb_blocks_hi)
        self._put_long(34, self.lo_cylinder)
        self._put_long(35, self.hi_cylinder)
        self._put_long(36, self.cyl_blocks)
        self._put_long(37, self.auto_park_seconds)
        self._put_long(38, self.high_rdsk_block)
        self._put_padded(160, 8, self.disk_vendor)
        self._put_padded(168, 16, self.disk_product)
        self._put_padded(184, 4, self.disk_revision)
        self._put_padded(188, 8, self.ctrl_vendor)
        self._put_padded(196, 16, self.ctrl_product)
        self._put_padded(212, 4, self.ctrl_revision)


class DosEnvec:
    """de_* environment vector embedded in PART blocks (hardblocks.h)."""

    FIELDS = (
        "table_size",   # highest valid index below
        "size_block",   # block size in longwords
        "sec_org",
        "surfaces",
        "sec_per_blk",  # sectors per filesystem block
        "blk_per_trk",
        "reserved",     # blocks reserved at partition start (bootblocks)
        "pre_alloc",
        "interleave",
        "low_cyl",
        "high_cyl",
        "num_buffer",
        "buf_mem_type",
        "max_transfer",
        "mask",
        "boot_pri",
        "dos_type",
        "baud",
        "control",
        "boot_blocks",
    )

    def __init__(self):
        for f in self.FIELDS:
            setattr(self, f, 0)

    @classmethod
    def parse(cls, blk, base_long):
        env = cls()
        table_size = blk._long(base_long)
        env.table_size = table_size
        for i, f in enumerate(cls.FIELDS[1:], start=1):
            if i <= table_size:
                setattr(env, f, blk._long(base_long + i))
        env.boot_pri = blk._slong(base_long + 15) if table_size >= 15 else 0
        return env

    def pack(self, blk, base_long):
        blk._put_long(base_long, self.table_size)
        for i, f in enumerate(self.FIELDS[1:], start=1):
            if i <= self.table_size:
                blk._put_long(base_long + i, getattr(self, f))

    # -- derived geometry --------------------------------------------
    def cyl_secs(self):
        return self.surfaces * self.blk_per_trk

    def num_cyls(self):
        return self.high_cyl - self.low_cyl + 1

    def start_sec(self):
        return self.low_cyl * self.cyl_secs()

    def num_secs(self):
        return self.num_cyls() * self.cyl_secs()

    def dos_type_str(self):
        return dos_type_to_str(self.dos_type)


def dos_type_to_str(dt):
    tag = struct.pack(">I", dt)
    txt = ""
    for b in tag[:3]:
        txt += chr(b) if 32 <= b < 127 else "?"
    return "%s\\%X" % (txt, tag[3])


class PartitionBlock(RDBBaseBlock):
    """PART -- one partition definition."""

    magic = b"PART"

    FLAG_BOOTABLE = 0x01
    FLAG_NO_MOUNT = 0x02

    def _unpack(self):
        self.host_id = self._long(3)
        self.next = self._long(4)
        self.flags = self._long(5)
        self.dev_flags = self._long(8)
        self.drv_name = self._bstr(36, 31).decode("latin-1")
        self.dos_env = DosEnvec.parse(self, 32)

    def _pack(self):
        self._put_long(3, self.host_id)
        self._put_long(4, self.next)
        self._put_long(5, self.flags)
        self._put_long(8, self.dev_flags)
        self._put_bstr(36, 31, self.drv_name.encode("latin-1"))
        self.dos_env.pack(self, 32)

    def bootable(self):
        return bool(self.flags & self.FLAG_BOOTABLE)

    def no_mount(self):
        return bool(self.flags & self.FLAG_NO_MOUNT)


class FSHeaderBlock(RDBBaseBlock):
    """FSHD -- embedded filesystem driver header."""

    magic = b"FSHD"

    def _unpack(self):
        self.host_id = self._long(3)
        self.next = self._long(4)
        self.flags = self._long(5)
        self.dos_type = self._long(8)
        self.version = self._long(9)
        self.patch_flags = self._long(10)
        # DeviceNode fields
        self.dn_type = self._long(11)
        self.dn_task = self._long(12)
        self.dn_lock = self._long(13)
        self.dn_handler = self._long(14)
        self.dn_stack_size = self._long(15)
        self.dn_priority = self._long(16)
        self.dn_startup = self._long(17)
        self.dn_seg_list_blk = self._long(18)
        self.dn_global_vec = self._long(19)

    def version_tuple(self):
        return (self.version >> 16, self.version & 0xFFFF)

    def dos_type_str(self):
        return dos_type_to_str(self.dos_type)


class LoadSegBlock(RDBBaseBlock):
    """LSEG -- chained chunks of a filesystem driver binary (hunk data)."""

    magic = b"LSEG"

    def _unpack(self):
        self.host_id = self._long(3)
        self.next = self._long(4)
        self.load_data = self._bytes(20, (self.size_longs - 5) * 4)


class BadBlocksBlock(RDBBaseBlock):
    """BADB -- bad block remapping (rare; preserved but not interpreted)."""

    magic = b"BADB"

    def _unpack(self):
        self.host_id = self._long(3)
        self.next = self._long(4)
