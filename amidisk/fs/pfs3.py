"""Native PFS3 reader (PFS\\3 / PDS\\3 / AFS\\1 family), read-only.

Structures ported from pfs3aio blocks.h / anodes.c / directory.c
(Toni Wilen's canonical PFS3 source). Writing PFS3 is deliberately out
of scope for the native engine -- per the architecture doc, mutations
belong to the emulated original handler until a native writer has
survived differential testing.

Addressing model (from the source):
  - all block numbers are partition-relative, in `bytesperblock` units
  - metadata ("reserved") blocks are reserved_blksize bytes long and are
    read as `rescluster` consecutive device blocks
  - the rootblock lives at block 2
  - anodes (allocation nodes) form chains of (blocknr, clustersize) runs;
    anode number 5 is the root directory
"""

import struct

from .ffs import FSError  # shared exception type
from .util import amiga_to_datetime, protect_to_str

# rootblock.disktype values
ID_PFS_DISK = 0x50465301   # 'PFS\1'
ID_AFS_DISK = 0x41465301   # 'AFS\1'
ID_MUAF_DISK = 0x6D754146  # 'muAF'
ID_MUPFS_DISK = 0x6D755046  # 'muPF'
ID_PFS2_DISK = 0x50465302  # 'PFS\2'
VALID_DISK_IDS = {ID_PFS_DISK, ID_AFS_DISK, ID_MUAF_DISK, ID_MUPFS_DISK, ID_PFS2_DISK}

# options bits
MODE_HARDDISK = 1
MODE_SPLITTED_ANODES = 2
MODE_DIR_EXTENSION = 4
MODE_DELDIR = 8
MODE_EXTENSION = 32
MODE_DATESTAMP = 64
MODE_SUPERINDEX = 128
MODE_LARGEFILE = 2048

# block ids
DBLKID = 0x4442   # 'DB'
ABLKID = 0x4142   # 'AB'
IBLKID = 0x4942   # 'IB'
SBLKID = 0x5342   # 'SB'
BMBLKID = 0x424D  # 'BM'
BMIBLKID = 0x4D49  # 'MI'
EXTENSIONID = 0x4558  # 'EX'

ANODE_ROOTDIR = 5

# direntry.type values (dos secondary types)
ST_USERDIR = 2
ST_FILE = -3
ST_SOFTLINK = 3
ST_LINKDIR = 4
ST_LINKFILE = -4
ST_ROLLOVERFILE = -16

ROOTBLOCK = 2
MAX_CHAIN = 1_000_000


class PFS3Entry:
    __slots__ = (
        "name", "type", "anode", "size", "protect", "comment",
        "days", "mins", "ticks", "link", "uid", "gid",
    )

    def is_dir(self):
        return self.type == ST_USERDIR

    def is_file(self):
        return self.type in (ST_FILE, ST_ROLLOVERFILE)

    def is_link(self):
        return self.type in (ST_SOFTLINK, ST_LINKDIR, ST_LINKFILE)

    def type_str(self):
        return {
            ST_USERDIR: "dir",
            ST_FILE: "file",
            ST_ROLLOVERFILE: "rollover-file",
            ST_SOFTLINK: "softlink",
            ST_LINKDIR: "hardlink-dir",
            ST_LINKFILE: "hardlink-file",
        }.get(self.type, "unknown(%d)" % self.type)

    def name_str(self):
        return self.name.decode("latin-1")

    def mtime(self):
        return amiga_to_datetime(self.days, self.mins, self.ticks)

    def protect_str(self):
        return protect_to_str(self.protect & 0xFF)

    def get_info(self):
        return {
            "name": self.name_str(),
            "type": self.type_str(),
            "size": self.size if self.is_file() else None,
            "protect": self.protect_str(),
            "comment": self.comment.decode("latin-1"),
            "mtime": self.mtime().isoformat(sep=" ", timespec="seconds"),
            "block": self.anode,
        }


class PFS3Volume:
    """Read-only PFS3 volume."""

    def __init__(self, blkdev, sec_per_blk=1, reserved=2, dos_type=None):
        self.dev = blkdev
        self.spb = max(sec_per_blk, 1)
        self.bytes_per_block = blkdev.block_bytes * self.spb
        self.total = blkdev.num_blocks // self.spb
        self.dos_type = dos_type
        self.read_only = True
        self.label = None
        self._anode_cache = {}
        self._index_cache = {}

    # -- low level -----------------------------------------------------
    def _read_blocks(self, blocknr, count):
        if blocknr + count > self.total or blocknr < 0:
            raise FSError("PFS3 block %d out of volume" % blocknr)
        return self.dev.read(blocknr * self.spb, count * self.spb)

    def _read_reserved(self, blocknr):
        return self._read_blocks(blocknr, self.rescluster)

    # -- mount ---------------------------------------------------------
    def open(self):
        raw = self._read_blocks(ROOTBLOCK, 1)
        disktype = struct.unpack_from(">I", raw, 0)[0]
        if disktype not in VALID_DISK_IDS:
            raise FSError("no PFS3 rootblock (disktype 0x%08x)" % disktype)
        self.disktype = disktype
        (
            self.options, self.datestamp,
            cday, cmin, ctick, prot,
        ) = struct.unpack_from(">IIHHHH", raw, 4)
        nlen = raw[20]
        self.label = raw[21 : 21 + min(nlen, 31)].decode("latin-1")
        (
            self.lastreserved, self.firstreserved, self.reserved_free,
            self.reserved_blksize, self.rblkcluster, self.blocksfree,
            self.alwaysfree, self.roving_ptr, self.deldir, self.disksize,
            self.extension, _nu,
        ) = struct.unpack_from(">IIIHHIIIIIII", raw, 52)
        self.created = amiga_to_datetime(cday, cmin, ctick)
        self.rescluster = self.reserved_blksize // self.bytes_per_block
        if self.rescluster < 1:
            raise FSError("bad reserved_blksize %d" % self.reserved_blksize)
        # the root cluster (rootblock + reserved bitmap) spans rblkcluster
        # sectors; fall back to one reserved cluster if the field is bogus
        cluster = self.rblkcluster if self.rblkcluster >= self.rescluster else self.rescluster
        self.root_raw = bytearray(self._read_blocks(ROOTBLOCK, cluster))
        self.anodes_per_block = (self.reserved_blksize - 16) // 12
        self.index_per_block = (self.reserved_blksize - 12) // 4
        self.super_index = bool(self.options & MODE_SUPERINDEX)
        self.split_anodes = bool(self.options & MODE_SPLITTED_ANODES)
        self.large_file = bool(self.options & MODE_LARGEFILE)
        # small-mode index tables live in the rootblock itself
        self.small_indexblocks = struct.unpack_from(">99I", self.root_raw, 96 + 20)
        self.small_bitmapindex = struct.unpack_from(">5I", self.root_raw, 96)
        self.large_bitmapindex = struct.unpack_from(">104I", self.root_raw, 96)
        # rootblock extension
        self.rext = None
        self.superindex_tab = (0,) * 16
        if self.extension and (self.options & MODE_EXTENSION):
            ext = self._read_reserved(self.extension)
            if struct.unpack_from(">H", ext, 0)[0] == EXTENSIONID:
                self.rext = bytearray(ext)
                self.superindex_tab = struct.unpack_from(">16I", ext, 64)
                rd = struct.unpack_from(">3H", ext, 16)
                self.root_modified = amiga_to_datetime(*rd)
                vd = struct.unpack_from(">3H", ext, 22)
                self.volume_modified = amiga_to_datetime(*vd)
                self.fnsize = struct.unpack_from(">H", ext, 56)[0] or 32
        # write support requires the modern feature set (any volume
        # formatted by pfs3aio/PFS3 in harddisk mode qualifies)
        writable_layout = (
            (self.options & MODE_HARDDISK)
            and self.split_anodes
            and (self.options & MODE_DIR_EXTENSION)
            and self.rext is not None
        )
        self.read_only = self.dev.read_only or not writable_layout
        return self

    def dos_type_str(self):
        from ..rdb.blocks import dos_type_to_str

        return dos_type_to_str(self.dos_type or ID_PFS_DISK)

    def get_info(self):
        info = {
            "label": self.label,
            "filesystem": "PFS3",
            "dos_type": self.dos_type_str(),
            "block_size": self.bytes_per_block,
            "total_blocks": self.disksize or self.total,
            "free_blocks": self.blocksfree,
            "free_bytes": self.blocksfree * self.bytes_per_block,
            "used_blocks": (self.disksize or self.total) - self.blocksfree,
            "root_block": ROOTBLOCK,
            "created": self.created.isoformat(sep=" ", timespec="seconds"),
            "read_only": self.read_only,
        }
        if self.rext is not None:
            info["modified"] = self.volume_modified.isoformat(
                sep=" ", timespec="seconds"
            )
        return info

    # -- anode resolution ------------------------------------------------
    def _get_index_block(self, nr):
        """Anode index block #nr -> raw reserved block."""
        if nr in self._index_cache:
            return self._index_cache[nr]
        if self.super_index:
            snr, soff = divmod(nr, self.index_per_block)
            if snr >= len(self.superindex_tab) or not self.superindex_tab[snr]:
                raise FSError("PFS3 superindex %d missing" % snr)
            sraw = self._read_reserved(self.superindex_tab[snr])
            if struct.unpack_from(">H", sraw, 0)[0] not in (SBLKID, IBLKID):
                raise FSError("bad PFS3 superblock id")
            blocknr = struct.unpack_from(">i", sraw, 12 + soff * 4)[0]
        else:
            if nr >= len(self.small_indexblocks):
                raise FSError("PFS3 index block %d out of range" % nr)
            blocknr = self.small_indexblocks[nr]
        if not blocknr:
            raise FSError("PFS3 index block %d is zero" % nr)
        raw = self._read_reserved(blocknr)
        if struct.unpack_from(">H", raw, 0)[0] != IBLKID:
            raise FSError("bad PFS3 index block id at %d" % blocknr)
        self._index_cache[nr] = raw
        return raw

    def _get_anode(self, anodenr):
        """anodenr -> (clustersize, blocknr, next)."""
        if anodenr in self._anode_cache:
            return self._anode_cache[anodenr]
        if self.split_anodes:
            seqnr, offset = anodenr >> 16, anodenr & 0xFFFF
        else:
            seqnr, offset = divmod(anodenr, self.anodes_per_block)
        inr, ioff = divmod(seqnr, self.index_per_block)
        iraw = self._get_index_block(inr)
        ablk = struct.unpack_from(">i", iraw, 12 + ioff * 4)[0]
        if ablk <= 0:
            raise FSError("PFS3 anode block %d missing" % seqnr)
        araw = self._read_reserved(ablk)
        if struct.unpack_from(">H", araw, 0)[0] != ABLKID:
            raise FSError("bad PFS3 anode block id at %d" % ablk)
        if offset >= self.anodes_per_block:
            raise FSError("PFS3 anode offset out of range")
        cs, bn, nxt = struct.unpack_from(">3I", araw, 16 + offset * 12)
        self._anode_cache[anodenr] = (cs, bn, nxt)
        return cs, bn, nxt

    def _anode_chain(self, anodenr):
        chain = []
        guard = 0
        while anodenr:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic PFS3 anode chain")
            cs, bn, nxt = self._get_anode(anodenr)
            if cs and bn != 0xFFFFFFFF:  # empty anodes mark data-less files
                chain.append((cs, bn))
            anodenr = nxt
        return chain

    # -- directories -------------------------------------------------------
    def _parse_direntry(self, raw, off):
        e = PFS3Entry()
        nxt = raw[off]
        e.type = struct.unpack_from(">b", raw, off + 1)[0]
        e.anode, e.size = struct.unpack_from(">2I", raw, off + 2)
        e.days, e.mins, e.ticks = struct.unpack_from(">3H", raw, off + 10)
        prot_low = raw[off + 16]
        nlen = raw[off + 17]
        e.name = bytes(raw[off + 18 : off + 18 + nlen])
        coff = off + 18 + nlen
        clen = raw[coff] if coff < off + nxt else 0
        e.comment = bytes(raw[coff + 1 : coff + 1 + clen])
        e.link = e.uid = e.gid = 0
        e.protect = prot_low
        if self.options & MODE_DIR_EXTENSION:
            # extrafields: reverse-packed UWORD array at entry end
            fields_end = off + nxt
            flags = struct.unpack_from(">H", raw, fields_end - 2)[0]
            pos = fields_end - 2
            vals = []
            # extrafields struct = 7 ULONG/UWORD slots = 11 UWORDs
            nwords = 11
            f = flags
            for _ in range(nwords):
                if f & 1:
                    pos -= 2
                    vals.append(struct.unpack_from(">H", raw, pos)[0])
                else:
                    vals.append(0)
                f >>= 1
            # slots: link(2w) uid gid prot(2w) virtualsize(2w) rollpointer(2w) fsizex
            e.link = (vals[0] << 16) | vals[1]
            e.uid, e.gid = vals[2], vals[3]
            e.protect = ((vals[4] << 16 | vals[5]) & 0xFFFFFF00) | prot_low
            if self.large_file:
                fsizex = vals[10]
                e.size |= fsizex << 32
        return e

    def _dir_entries(self, anodenr):
        entries = []
        cap = self.reserved_blksize
        for cs, bn in self._anode_chain(anodenr):
            for k in range(cs):
                raw = self._read_reserved(bn + k)
                if struct.unpack_from(">H", raw, 0)[0] != DBLKID:
                    raise FSError("bad PFS3 dir block at %d" % (bn + k))
                off = 20
                while off < cap and raw[off]:
                    e = self._parse_direntry(raw, off)
                    entries.append(e)
                    off += raw[off]
        return entries

    # -- public API (mirrors FFSVolume where sensible) ------------------------
    def root_entry(self):
        e = PFS3Entry()
        e.name = (self.label or "").encode("latin-1")
        e.type = ST_USERDIR
        e.anode = ANODE_ROOTDIR
        e.size = 0
        e.protect = 0
        e.comment = b""
        e.days = e.mins = e.ticks = 0
        e.link = e.uid = e.gid = 0
        return e

    @staticmethod
    def _split(path):
        if isinstance(path, str):
            path = path.encode("latin-1")
        return [p for p in path.replace(b"\\", b"/").split(b"/") if p]

    @staticmethod
    def _upper(c):
        if 0x61 <= c <= 0x7A or (0xE0 <= c <= 0xFE and c != 0xF7):
            return c - 0x20
        return c

    def _names_equal(self, a, b):
        return len(a) == len(b) and all(
            self._upper(x) == self._upper(y) for x, y in zip(a, b)
        )

    def resolve(self, path):
        parts = self._split(path)
        cur = self.root_entry()
        for seg in parts:
            if not cur.is_dir():
                raise FSError("'%s' is not a directory" % cur.name_str())
            found = None
            for e in self._dir_entries(cur.anode):
                if self._names_equal(e.name, seg):
                    found = e
                    break
            if found is None:
                raise FSError("path not found: %s" % path)
            cur = found
        return cur

    def list_dir(self, path=""):
        e = self.resolve(path)
        if not e.is_dir():
            raise FSError("not a directory")
        entries = self._dir_entries(e.anode)
        entries.sort(key=lambda x: x.name.lower())
        return entries

    def walk(self, path=""):
        start = self.resolve(path)
        if not start.is_dir():
            raise FSError("not a directory")
        base = "/".join(p.decode("latin-1") for p in self._split(path))
        stack = [(base, start.anode)]
        while stack:
            prefix, anode = stack.pop()
            for e in self._dir_entries(anode):
                yield prefix, e
                if e.is_dir():
                    sub = (prefix + "/" if prefix else "") + e.name_str()
                    stack.append((sub, e.anode))

    def read_file(self, path):
        e = path if isinstance(path, PFS3Entry) else self.resolve(path)
        if e.type == ST_LINKFILE and e.link:
            raise FSError("hard link: open the link target instead")
        if not e.is_file():
            raise FSError("not a file: %s" % e.name_str())
        return self._read_data(e)

    def read_file_bytes(self, path):
        return b"".join(self.read_file(path))

    def _read_data(self, entry):
        remaining = entry.size
        guard = 0
        for cs, bn in self._anode_chain(entry.anode):
            if remaining <= 0:
                break
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic PFS3 data chain")
            # read run in reasonably sized slices
            blocks_left = cs
            blk = bn
            while blocks_left > 0 and remaining > 0:
                n = min(blocks_left, 512)
                data = self._read_blocks(blk, n)
                if remaining < len(data):
                    data = data[:remaining]
                yield data
                remaining -= len(data)
                blk += n
                blocks_left -= n
        if remaining > 0:
            raise FSError(
                "truncated PFS3 file %s (%d bytes missing)"
                % (entry.name_str(), remaining)
            )

    # ======================================================================
    # write support (ported from pfs3aio allocation.c / anodes.c /
    # directory.c / format.c; offline mode -- no postponed operations)
    # ======================================================================

    def _require_writable(self):
        if self.read_only:
            raise FSError("PFS3 volume is read-only (device or unsupported layout)")

    def _write_raw(self, blocknr, raw):
        self.dev.write(blocknr * self.spb, bytes(raw))

    def _write_reserved(self, blocknr, raw):
        raw = bytearray(raw)
        struct.pack_into(">I", raw, 4, self.datestamp)
        self._write_raw(blocknr, raw)

    # -- reserved area allocator (MSB-first bitmap behind the rootblock) --
    def _res_bitmap_view(self):
        # reserved bitmap starts one logical block into the root cluster
        return self.root_raw, self.bytes_per_block + 12  # skip BM header

    def _numreserved(self):
        return (self.lastreserved - self.firstreserved + 1) // self.rescluster

    def _alloc_reserved(self):
        raw, base = self._res_bitmap_view()
        nres = self._numreserved()
        for i in range((nres + 31) // 32):
            field = struct.unpack_from(">I", raw, base + i * 4)[0]
            if not field:
                continue
            for j in range(31, -1, -1):
                if field & (1 << j):
                    idx = i * 32 + (31 - j)
                    blocknr = self.firstreserved + idx * self.rescluster
                    if blocknr <= self.lastreserved:
                        struct.pack_into(
                            ">I", raw, base + i * 4, field & ~(1 << j)
                        )
                        self.reserved_free -= 1
                        return blocknr
        raise FSError("PFS3 reserved area is full")

    def _free_reserved(self, blocknr):
        raw, base = self._res_bitmap_view()
        idx = (blocknr - self.firstreserved) // self.rescluster
        i, j = idx // 32, 31 - (idx % 32)
        field = struct.unpack_from(">I", raw, base + i * 4)[0]
        struct.pack_into(">I", raw, base + i * 4, field | (1 << j))
        self.reserved_free += 1

    # -- main area allocator ------------------------------------------------
    def _lpb(self):
        return self.reserved_blksize // 4 - 3  # longs per bitmap block

    def _bitmapstart(self):
        return self.lastreserved + 1

    def _bitmapindex_capacity(self):
        return 104 if self.super_index else 5

    def _get_bitmapindex_blocknr(self, nr):
        if nr >= self._bitmapindex_capacity():
            raise FSError("bitmap index %d out of range" % nr)
        return struct.unpack_from(">I", self.root_raw, 96 + nr * 4)[0]

    def _get_bmb(self, seqnr):
        if seqnr in self._bmb_cache:
            return self._bmb_cache[seqnr]
        inr, ioff = divmod(seqnr, self.index_per_block)
        miblk = self._get_bitmapindex_blocknr(inr)
        if not miblk:
            raise FSError("missing bitmap index block %d" % inr)
        miraw = self._read_reserved(miblk)
        if struct.unpack_from(">H", miraw, 0)[0] != BMIBLKID:
            raise FSError("bad bitmap index id at %d" % miblk)
        bmblk = struct.unpack_from(">I", miraw, 12 + ioff * 4)[0]
        if not bmblk:
            raise FSError("missing bitmap block %d" % seqnr)
        raw = bytearray(self._read_reserved(bmblk))
        if struct.unpack_from(">H", raw, 0)[0] != BMBLKID:
            raise FSError("bad bitmap block id at %d" % bmblk)
        self._bmb_cache[seqnr] = (bmblk, raw)
        return self._bmb_cache[seqnr]

    def _main_bit(self, blocknr):
        bit = blocknr - self._bitmapstart()
        nr, i = bit // 32, bit % 32
        lpb = self._lpb()
        return nr // lpb, nr % lpb, 1 << (31 - i)

    def _block_is_free(self, blocknr):
        seq, off, mask = self._main_bit(blocknr)
        _, raw = self._get_bmb(seq)
        return bool(struct.unpack_from(">I", raw, 12 + off * 4)[0] & mask)

    def _set_main_bit(self, blocknr, free):
        seq, off, mask = self._main_bit(blocknr)
        bmblk, raw = self._get_bmb(seq)
        field = struct.unpack_from(">I", raw, 12 + off * 4)[0]
        struct.pack_into(
            ">I", raw, 12 + off * 4, (field | mask) if free else (field & ~mask)
        )
        self._bmb_dirty.add(seq)

    def _alloc_main(self, count):
        """Allocate `count` blocks from the main area; returns runs
        [(start, len)] (contiguous where possible)."""
        total = self.disksize or self.total
        start = self._bitmapstart()
        runs = []
        got = 0
        blk = self._roving if start <= self._roving < total else start
        wrapped = False
        run_s = run_n = 0
        while got < count:
            if blk >= total:
                if wrapped:
                    break
                wrapped = True
                if run_n:
                    runs.append((run_s, run_n))
                    run_n = 0
                blk = start
                continue
            if wrapped and blk >= (self._roving if start <= self._roving < total else start):
                break
            if self._block_is_free(blk):
                if run_n and run_s + run_n == blk:
                    run_n += 1
                else:
                    if run_n:
                        runs.append((run_s, run_n))
                    run_s, run_n = blk, 1
                self._set_main_bit(blk, False)
                got += 1
            elif run_n:
                runs.append((run_s, run_n))
                run_n = 0
            blk += 1
        if run_n:
            runs.append((run_s, run_n))
        if got < count:
            for s, n in runs:
                for b in range(s, s + n):
                    self._set_main_bit(b, True)
            raise FSError("PFS3 volume full: needed %d blocks" % count)
        self._roving = blk
        self.blocksfree -= count
        return runs

    def _free_main_run(self, start, length):
        for b in range(start, start + length):
            self._set_main_bit(b, True)
        self.blocksfree += length

    # -- anode allocation -----------------------------------------------------
    def _anode_index_blocknr(self, inr, create=False):
        """Block number of anode index block #inr (small or super mode)."""
        if self.super_index:
            snr, soff = divmod(inr, self.index_per_block)
            if snr >= 16:
                raise FSError("superindex exhausted")
            sblk = self.superindex_tab[snr]
            if not sblk:
                if not create:
                    return 0
                sblk = self._alloc_reserved()
                raw = bytearray(self.reserved_blksize)
                struct.pack_into(">HHII", raw, 0, SBLKID, 0, 0, snr)
                self._write_reserved(sblk, raw)
                tab = list(self.superindex_tab)
                tab[snr] = sblk
                self.superindex_tab = tuple(tab)
                struct.pack_into(">I", self.rext, 64 + snr * 4, sblk)
            sraw = bytearray(self._read_reserved(sblk))
            iblk = struct.unpack_from(">I", sraw, 12 + soff * 4)[0]
            if not iblk and create:
                iblk = self._alloc_reserved()
                iraw = bytearray(self.reserved_blksize)
                struct.pack_into(">HHII", iraw, 0, IBLKID, 0, 0, inr)
                self._write_reserved(iblk, iraw)
                struct.pack_into(">I", sraw, 12 + soff * 4, iblk)
                self._write_reserved(sblk, sraw)
            return iblk
        # small mode: index table inside the rootblock at byte 116
        if inr >= 99:
            raise FSError("anode index table exhausted")
        iblk = struct.unpack_from(">I", self.root_raw, 116 + inr * 4)[0]
        if not iblk and create:
            iblk = self._alloc_reserved()
            iraw = bytearray(self.reserved_blksize)
            struct.pack_into(">HHII", iraw, 0, IBLKID, 0, 0, inr)
            self._write_reserved(iblk, iraw)
            struct.pack_into(">I", self.root_raw, 116 + inr * 4, iblk)
        return iblk

    def _anode_block(self, seqnr, create=False):
        """(blocknr, raw) of anode block seqnr, optionally creating it."""
        if seqnr in self._ab_cache:
            return self._ab_cache[seqnr]
        inr, ioff = divmod(seqnr, self.index_per_block)
        iblk = self._anode_index_blocknr(inr, create=create)
        if not iblk:
            return (0, None)
        iraw = bytearray(self._read_reserved(iblk))
        ablk = struct.unpack_from(">i", iraw, 12 + ioff * 4)[0]
        if ablk <= 0:
            if not create:
                return (0, None)
            ablk = self._alloc_reserved()
            araw = bytearray(self.reserved_blksize)
            struct.pack_into(">HHII", araw, 0, ABLKID, 0, 0, seqnr)
            self._write_reserved(ablk, araw)
            struct.pack_into(">I", iraw, 12 + ioff * 4, ablk)
            self._write_reserved(iblk, iraw)
            self._ab_cache[seqnr] = (ablk, araw)
            return self._ab_cache[seqnr]
        araw = bytearray(self._read_reserved(ablk))
        if struct.unpack_from(">H", araw, 0)[0] != ABLKID:
            raise FSError("bad anode block id at %d" % ablk)
        self._ab_cache[seqnr] = (ablk, araw)
        return self._ab_cache[seqnr]

    def _save_anode(self, anodenr, cs, bn, nxt):
        seqnr, offset = anodenr >> 16, anodenr & 0xFFFF
        ablk, araw = self._anode_block(seqnr, create=True)
        struct.pack_into(">3I", araw, 16 + offset * 12, cs, bn, nxt)
        self._ab_dirty.add(seqnr)
        self._anode_cache.pop(anodenr, None)

    def _alloc_anode(self, hint_seqnr=0):
        """Find a free anode slot (blocknr == 0); returns anodenr."""
        seqnrs = [hint_seqnr] if hint_seqnr else []
        seqnr = 0
        while True:
            if seqnr not in seqnrs:
                seqnrs.append(seqnr)
            ablk, araw = self._anode_block(seqnrs[-1], create=False)
            if araw is None:
                # no more blocks: create a fresh one at this seqnr
                ablk, araw = self._anode_block(seqnr, create=True)
            for k in range(self.anodes_per_block):
                if seqnrs[-1] == 0 and k <= ANODE_ROOTDIR:
                    continue  # never hand out the reserved anodes
                cs, bn, nxt = struct.unpack_from(">3I", araw, 16 + k * 12)
                if bn == 0 and cs == 0 and nxt == 0:
                    anodenr = (seqnrs[-1] << 16) | k
                    self._save_anode(anodenr, 0, 0xFFFFFFFF, 0)
                    return anodenr
            seqnr = seqnrs[-1] + 1

    def _free_anode(self, anodenr):
        self._save_anode(anodenr, 0, 0, 0)

    # -- transaction bookkeeping ----------------------------------------------
    def _begin(self):
        self._require_writable()
        self.datestamp += 1
        self._bmb_cache = {}
        self._bmb_dirty = set()
        self._ab_cache = {}
        self._ab_dirty = set()
        if not hasattr(self, "_roving"):
            self._roving = self._bitmapstart()

    def _commit(self, touch_root_date=True):
        for seq in self._bmb_dirty:
            bmblk, raw = self._bmb_cache[seq]
            self._write_reserved(bmblk, raw)
        for seq in self._ab_dirty:
            ablk, araw = self._ab_cache[seq]
            self._write_reserved(ablk, araw)
        # rootblock counters + datestamp
        struct.pack_into(">I", self.root_raw, 8, self.datestamp)
        struct.pack_into(">I", self.root_raw, 60, self.reserved_free)
        struct.pack_into(">I", self.root_raw, 68, self.blocksfree)
        struct.pack_into(">I", self.root_raw, 76, 0)  # roving_ptr (hint only)
        self._write_raw(ROOTBLOCK, self.root_raw)
        if self.rext is not None:
            struct.pack_into(">I", self.rext, 8, self.datestamp)
            if touch_root_date:
                from .util import datetime_to_amiga
                from datetime import datetime as _dt

                d, m, t = datetime_to_amiga(_dt.now())
                struct.pack_into(">3H", self.rext, 16, d & 0xFFFF, m, t)
            self._write_reserved(self.extension, self.rext)
        self._anode_cache.clear()
        self._index_cache.clear()

    # -- directory manipulation --------------------------------------------
    def _dir_chain(self, dir_anodenr):
        """[(anodenr, blocknr)] of all dirblocks of a directory."""
        out = []
        nr = dir_anodenr
        guard = 0
        while nr:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic dir anode chain")
            cs, bn, nxt = self._get_anode(nr)
            if bn not in (0, 0xFFFFFFFF):
                for k in range(max(cs, 1)):
                    out.append((nr, bn + k))
            nr = nxt
        return out

    def _build_direntry(self, etype, anode, fsize, name, comment=b"",
                        protect=0, mtime=None):
        from .util import datetime_to_amiga
        from datetime import datetime as _dt

        name = bytes(name)
        limit = min(getattr(self, "fnsize", 32) - 1, 107)
        if not 1 <= len(name) <= limit:
            raise FSError("invalid PFS3 name length %d" % len(name))
        if b"/" in name or b":" in name or any(c < 32 for c in name):
            raise FSError("invalid characters in name")
        comment = bytes(comment)[:79]
        d, m, t = datetime_to_amiga(mtime or _dt.now())
        fixed = struct.pack(
            ">bIIHHHBB", etype, anode, fsize & 0xFFFFFFFF,
            d & 0xFFFF, m, t, protect & 0xFF, len(name)
        )
        body = fixed + name + bytes([len(comment)]) + comment
        entry = bytes([0]) + body  # placeholder for 'next'
        if len(entry) & 1:
            entry += b"\x00"
        if self.options & MODE_DIR_EXTENSION:
            entry += struct.pack(">H", 0)  # empty extrafields: flags word
        entry = bytes([len(entry)]) + entry[1:]
        return entry

    def _entries_end(self, raw):
        off = 20
        while off < self.reserved_blksize and raw[off]:
            off += raw[off]
        return off

    def _add_entry(self, dir_entry, entry_bytes):
        """Insert a built direntry into directory `dir_entry` (an SFSEntry-
        style PFS3Entry with .anode = dir identity)."""
        chain = self._dir_chain(dir_entry.anode)
        parent_of_dir = self._dirblock_parent(dir_entry.anode)
        for nr, bn in chain:
            raw = bytearray(self._read_reserved(bn))
            end = self._entries_end(raw)
            if end + len(entry_bytes) + 1 <= self.reserved_blksize:
                raw[end : end + len(entry_bytes)] = entry_bytes
                if end + len(entry_bytes) < self.reserved_blksize:
                    raw[end + len(entry_bytes)] = 0
                self._write_reserved(bn, raw)
                return
        # extend the directory with a fresh dirblock at the chain tail
        newblk = self._alloc_reserved()
        newnr = self._alloc_anode(hint_seqnr=chain[-1][0] >> 16 if chain else 0)
        last_nr = chain[-1][0] if chain else dir_entry.anode
        cs, bn, nxt = self._get_anode(last_nr)
        self._save_anode(last_nr, cs, bn, newnr)
        self._save_anode(newnr, 1, newblk, 0)
        raw = bytearray(self.reserved_blksize)
        struct.pack_into(">HHIHHII", raw, 0, DBLKID, 0, 0, 0, 0,
                         dir_entry.anode, parent_of_dir)
        raw[20 : 20 + len(entry_bytes)] = entry_bytes
        self._write_reserved(newblk, raw)

    def _dirblock_parent(self, dir_anodenr):
        if dir_anodenr == ANODE_ROOTDIR:
            return 0
        chain = self._dir_chain(dir_anodenr)
        if not chain:
            return 0
        raw = self._read_reserved(chain[0][1])
        return struct.unpack_from(">I", raw, 16)[0]

    def _locate_entry(self, dir_anodenr, name):
        """(blocknr, offset, entry) of `name` in the directory, or None."""
        for nr, bn in self._dir_chain(dir_anodenr):
            raw = self._read_reserved(bn)
            off = 20
            while off < self.reserved_blksize and raw[off]:
                e = self._parse_direntry(raw, off)
                if self._names_equal(e.name, name):
                    return bn, off, e
                off += raw[off]
        return None

    def _remove_entry(self, dir_anodenr, name):
        loc = self._locate_entry(dir_anodenr, name)
        if loc is None:
            raise FSError("entry not found: %r" % name)
        bn, off, e = loc
        raw = bytearray(self._read_reserved(bn))
        size = raw[off]
        end = self._entries_end(raw)
        raw[off:end - size] = raw[off + size:end]
        raw[end - size:end] = b"\x00" * size
        self._write_reserved(bn, raw)
        # drop empty non-head dirblocks from the chain
        if raw[20] == 0:
            chain = self._dir_chain(dir_anodenr)
            for i, (nr, b) in enumerate(chain):
                if b == bn and i > 0:
                    prev_nr = chain[i - 1][0]
                    pcs, pbn, _ = self._get_anode(prev_nr)
                    _, _, nxt = self._get_anode(nr)
                    self._save_anode(prev_nr, pcs, pbn, nxt)
                    self._free_anode(nr)
                    self._free_reserved(b)
                    break
        return e

    def _resolve_dir(self, path):
        e = self.resolve(path)
        if not e.is_dir():
            raise FSError("not a directory: %s" % path)
        return e

    def _split_parent(self, path):
        parts = self._split(path)
        if not parts:
            raise FSError("empty path")
        parent = self._resolve_dir(b"/".join(parts[:-1]))
        return parent, parts[-1]

    # -- public mutating API -------------------------------------------------
    def mkdir(self, path):
        self._begin()
        parent, name = self._split_parent(path)
        if self._locate_entry(parent.anode, name):
            raise FSError("already exists: %s" % path)
        anodenr = self._alloc_anode()
        dirblk = self._alloc_reserved()
        raw = bytearray(self.reserved_blksize)
        struct.pack_into(">HHIHHII", raw, 0, DBLKID, 0, 0, 0, 0,
                         anodenr, parent.anode)
        self._write_reserved(dirblk, raw)
        self._save_anode(anodenr, 1, dirblk, 0)
        entry = self._build_direntry(ST_USERDIR, anodenr, 0, name)
        self._add_entry(parent, entry)
        self._commit()
        return anodenr

    def makedirs(self, path):
        parts = self._split(path)
        cur = b""
        for seg in parts:
            cur = cur + b"/" + seg if cur else seg
            try:
                e = self.resolve(cur)
                if not e.is_dir():
                    raise FSError("'%s' exists and is not a directory"
                                  % cur.decode("latin-1"))
            except FSError as ex:
                if "not found" not in str(ex):
                    raise
                self.mkdir(cur)

    def write_file(self, path, data, size=None, protect=0, comment=b"", mtime=None):
        self._begin()
        parent, name = self._split_parent(path)
        if size is None:
            size = len(data)
            
        if size >= 1 << 32:
            raise FSError("files >= 4 GB not supported")
        existing = self._locate_entry(parent.anode, name)
        if existing is not None:
            if not existing[2].is_file():
                raise FSError("exists and is not a file: %s" % path)
            self._delete_file_data(existing[2])
            self._remove_entry(parent.anode, name)

        nblocks = (size + self.bytes_per_block - 1) // self.bytes_per_block
        anodenr = self._alloc_anode()
        
        try:
            if nblocks:
                runs = self._alloc_main(nblocks)
                # write data
                if isinstance(data, bytes):
                    pos = 0
                    for s, n in runs:
                        chunk = data[pos : pos + n * self.bytes_per_block]
                        pad = n * self.bytes_per_block - len(chunk)
                        if pad:
                            chunk = bytes(chunk) + b"\x00" * pad
                        self._write_raw(s, chunk)
                        pos += n * self.bytes_per_block
                else:
                    iterator = iter(data)
                    buf_stream = bytearray()
                    remaining = size
                    for s, n in runs:
                        run_bytes = min(remaining, n * self.bytes_per_block)
                        while len(buf_stream) < run_bytes:
                            try:
                                buf_stream += next(iterator)
                            except StopIteration:
                                raise FSError("stream ended prematurely")
                                
                        chunk = bytes(buf_stream[:run_bytes])
                        buf_stream = buf_stream[run_bytes:]
                        
                        pad = n * self.bytes_per_block - len(chunk)
                        if pad:
                            chunk = chunk + b"\x00" * pad
                        self._write_raw(s, chunk)
                        remaining -= run_bytes

                # anode chain: one anode per run
                cur = anodenr
                for i, (s, n) in enumerate(runs):
                    nxt = 0
                    if i + 1 < len(runs):
                        nxt = self._alloc_anode(hint_seqnr=cur >> 16)
                    self._save_anode(cur, n, s, nxt)
                    cur = nxt
            else:
                self._save_anode(anodenr, 0, 0xFFFFFFFF, 0)
        except Exception:
            if nblocks and 'runs' in locals():
                for s, n in runs:
                    self._free_main_run(s, n)
            self._free_anode(anodenr)
            raise
            
        entry = self._build_direntry(
            ST_FILE, anodenr, size, name, comment, protect, mtime
        )
        self._add_entry(parent, entry)
        self._commit()
        return anodenr

    def _delete_file_data(self, entry):
        nr = entry.anode
        guard = 0
        while nr:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic anode chain")
            cs, bn, nxt = self._get_anode(nr)
            if cs and bn not in (0, 0xFFFFFFFF):
                self._free_main_run(bn, cs)
            self._free_anode(nr)
            nr = nxt

    def delete(self, path, recursive=False):
        self._begin()
        entry = self.resolve(path) if not isinstance(path, PFS3Entry) else path
        if entry.anode == ANODE_ROOTDIR:
            raise FSError("cannot delete the root directory")
        parent_nr = self._dirblock_parent(entry.anode) if entry.is_dir() else None
        if entry.is_dir():
            children = self._dir_entries(entry.anode)
            if children and not recursive:
                raise FSError("directory not empty: %s" % entry.name_str())
            # delete children first (each op commits on its own)
            base = self._path_of(entry)
            for ch in children:
                self.delete(base + b"/" + ch.name, recursive=True)
            self._begin()
            # free the dir's own blocks + anodes
            for nr, bn in self._dir_chain(entry.anode):
                self._free_reserved(bn)
            nr = entry.anode
            guard = 0
            while nr:
                guard += 1
                if guard > MAX_CHAIN:
                    break
                _, _, nxt = self._get_anode(nr)
                self._free_anode(nr)
                nr = nxt
            self._remove_entry(parent_nr, entry.name)
        else:
            # find the parent by locating the entry
            parent_nr = self._find_parent_of(entry)
            if entry.is_file():
                self._delete_file_data(entry)
            elif entry.is_link():
                cs, bn, nxt = self._get_anode(entry.anode)
                if cs and bn not in (0, 0xFFFFFFFF):
                    self._free_main_run(bn, cs)
                self._free_anode(entry.anode)
            self._remove_entry(parent_nr, entry.name)
        self._commit()

    def _path_of(self, entry):
        """Reconstruct a path for `entry` (used by recursive delete)."""
        for prefix, e in self.walk(""):
            if e.anode == entry.anode and e.name == entry.name:
                return (prefix.encode("latin-1") + b"/" + e.name) if prefix else e.name
        raise FSError("entry vanished: %s" % entry.name_str())

    def _find_parent_of(self, entry):
        for prefix, e in self.walk(""):
            if e.anode == entry.anode and e.name == entry.name:
                d = self._resolve_dir(prefix)
                return d.anode
        raise FSError("entry not found in tree: %s" % entry.name_str())

    def rename(self, src, dst):
        self._begin()
        entry = self.resolve(src)
        if entry.anode == ANODE_ROOTDIR:
            raise FSError("cannot rename the root directory")
        new_parent, new_name = self._split_parent(dst)
        clash = self._locate_entry(new_parent.anode, new_name)
        if clash is not None and clash[2].anode != entry.anode:
            raise FSError("destination exists: %s" % dst)
        if entry.is_dir():  # no move into own subtree
            p = new_parent.anode
            guard = 0
            while p and guard < MAX_CHAIN:
                if p == entry.anode:
                    raise FSError("cannot move a directory into itself")
                p = self._dirblock_parent(p)
                guard += 1
        old_parent_nr = self._find_parent_of(entry)
        removed = self._remove_entry(old_parent_nr, entry.name)
        entry_bytes = self._build_direntry(
            removed.type, removed.anode, removed.size, new_name,
            removed.comment, removed.protect & 0xFF,
            amiga_to_datetime(removed.days, removed.mins, removed.ticks),
        )
        self._add_entry(new_parent, entry_bytes)
        if entry.is_dir() and new_parent.anode != old_parent_nr:
            # fix the parent pointer in all of the moved dir's dirblocks
            for nr, bn in self._dir_chain(entry.anode):
                raw = bytearray(self._read_reserved(bn))
                struct.pack_into(">I", raw, 16, new_parent.anode)
                self._write_reserved(bn, raw)
        self._commit()

    # -- format ---------------------------------------------------------------
    def format(self, label, dos_type=None, dosenvec=None):
        """Create a fresh PFS3 filesystem (port of pfs3aio FDSFormat)."""
        if self.dev.read_only:
            raise FSError("device is read-only")
        label = bytes(label)
        if not 1 <= len(label) <= 31:
            raise FSError("invalid label length")
        bs = self.bytes_per_block
        total = self.total
        MAXSMALLDISK = 5 * 253 * 253 * 32
        MAXDISKSIZE1K = 104 * 253 * 253 * 32
        if total > MAXDISKSIZE1K:
            raise FSError("PFS3 format beyond 100 GB not supported here")
        supermode = total > MAXSMALLDISK
        resblocksize = max(1024, bs)
        options = (
            MODE_HARDDISK | MODE_SPLITTED_ANODES | MODE_DIR_EXTENSION
            | 16 | MODE_DATESTAMP | 512 | 1024   # SIZEFIELD | EXTROVING | LONGFN
            | MODE_EXTENSION
        )
        if supermode:
            options |= MODE_SUPERINDEX
        rescluster = resblocksize // bs

        # sizing (CalcNumReserved)
        taken = 32
        i = 2048
        while i and i // 2 < total:
            m = 10 if i >= 512 * 2048 else 14
            taken += taken * m // 16
            i <<= 1
        taken //= resblocksize // 1024
        taken = min(4096 + 255 * 1024 * 8, taken - 1)
        numreserved = (taken + 31) & ~31

        # reserved bitmap sizing (in 1K units, then reserved blocks)
        numblocks_1k = 1
        j = 125
        while j < numreserved // 32:
            numblocks_1k += 1
            j += 256
        rb_blocks = (1024 * numblocks_1k + resblocksize - 1) // resblocksize
        rblkcluster = rescluster * rb_blocks

        firstreserved = 2
        lastreserved = rescluster * numreserved + firstreserved - 1
        if lastreserved + 8 >= total:
            raise FSError("volume too small for PFS3")
        reserved_free = numreserved - rb_blocks - 1
        blocksfree = total - rescluster * numreserved - firstreserved

        from .util import datetime_to_amiga
        from datetime import datetime as _dt

        d, m, t = datetime_to_amiga(_dt.now())

        # bootblock: 2 logical blocks, PFS\1 magic
        boot = bytearray(2 * bs)
        struct.pack_into(">I", boot, 0, ID_PFS_DISK)
        self._write_raw(0, boot)

        # root cluster: rootblock + reserved bitmap
        root = bytearray(rblkcluster * bs)
        struct.pack_into(">iIIHHHH", root, 0, ID_PFS_DISK, options, 1,
                         d & 0xFFFF, m, t, 0xF0)
        root[20] = len(label)
        root[21 : 21 + len(label)] = label
        struct.pack_into(
            ">IIIHHIIIIIII", root, 52,
            lastreserved, firstreserved, reserved_free,
            resblocksize, rblkcluster, blocksfree,
            blocksfree // 20, 0, 0, total,
            firstreserved + rblkcluster, 0,
        )
        # reserved bitmap directly behind the (logical) rootblock
        bmoff = bs
        struct.pack_into(">HHII", root, bmoff, BMBLKID, 0, 1, 0)
        base = bmoff + 12
        for k in range(numreserved // 32):
            struct.pack_into(">I", root, base + k * 4, 0xFFFFFFFF)
        last = 0
        for k in range(numreserved % 32):
            last |= 0x80000000 >> k
        if numreserved % 32:
            struct.pack_into(">I", root, base + (numreserved // 32) * 4, last)
        for k in range(rb_blocks + 1):   # root cluster blocks + extension
            o = base + (k // 32) * 4
            v = struct.unpack_from(">I", root, o)[0]
            struct.pack_into(">I", root, o, v ^ (0x80000000 >> (k % 32)))

        # adopt state so the normal allocators work during bootstrap
        self.disktype = ID_PFS_DISK
        self.options = options
        self.datestamp = 1
        self.label = label.decode("latin-1")
        self.lastreserved = lastreserved
        self.firstreserved = firstreserved
        self.reserved_free = reserved_free
        self.reserved_blksize = resblocksize
        self.rblkcluster = rblkcluster
        self.blocksfree = blocksfree
        self.alwaysfree = blocksfree // 20
        self.deldir = 0
        self.disksize = total
        self.extension = firstreserved + rblkcluster
        self.rescluster = rescluster
        self.root_raw = root
        self.anodes_per_block = (resblocksize - 16) // 12
        self.index_per_block = (resblocksize - 12) // 4
        self.super_index = supermode
        self.split_anodes = True
        self.large_file = False
        self.superindex_tab = (0,) * 16
        self.created = _dt.now()
        self.fnsize = 32
        self._anode_cache = {}
        self._index_cache = {}
        self.read_only = False

        # rootblock extension
        rext = bytearray(resblocksize)
        struct.pack_into(">HHIII", rext, 0, EXTENSIONID, 0, 0, 1,
                         (19 << 16) + 2)
        struct.pack_into(">3H", rext, 16, d & 0xFFFF, m, t)
        struct.pack_into(">H", rext, 56, 32)  # fnsize
        if dosenvec:
            n = min(dosenvec[0] if dosenvec else 0, 16)
            for k in range(n + 1):
                struct.pack_into(">I", rext, 200 + k * 4, dosenvec[k])
        self.rext = rext

        self._begin()
        self.datestamp = 1
        # main bitmap: all free
        lpb = self._lpb()
        bits_needed = (total - (lastreserved + 1) + 31) // 32
        no_bmb = (bits_needed + lpb - 1) // lpb
        cap = self._bitmapindex_capacity()
        n_mi = (no_bmb + self.index_per_block - 1) // self.index_per_block
        if n_mi > cap:
            raise FSError("volume too large for bitmap index")
        seq = 0
        for minr in range(n_mi):
            miblk = self._alloc_reserved()
            miraw = bytearray(resblocksize)
            struct.pack_into(">HHII", miraw, 0, BMIBLKID, 0, 1, minr)
            for o in range(self.index_per_block):
                if seq >= no_bmb:
                    break
                bmblk = self._alloc_reserved()
                bmraw = bytearray(resblocksize)
                struct.pack_into(">HHII", bmraw, 0, BMBLKID, 0, 1, seq)
                for k in range(lpb):
                    struct.pack_into(">I", bmraw, 12 + k * 4, 0xFFFFFFFF)
                self._write_reserved(bmblk, bmraw)
                struct.pack_into(">I", miraw, 12 + o * 4, bmblk)
                seq += 1
            self._write_reserved(miblk, miraw)
            struct.pack_into(">I", self.root_raw, 96 + minr * 4, miblk)

        # bootstrap anodes 0..4 (reserved) and the root directory (5)
        ablk, araw = self._anode_block(0, create=True)
        for k in range(ANODE_ROOTDIR):
            struct.pack_into(">3I", araw, 16 + k * 12, 0, 0xFFFFFFFF, 0)
        rootdirblk = self._alloc_reserved()
        struct.pack_into(">3I", araw, 16 + ANODE_ROOTDIR * 12, 1, rootdirblk, 0)
        self._ab_dirty.add(0)
        draw = bytearray(resblocksize)
        struct.pack_into(">HHIHHII", draw, 0, DBLKID, 0, 0, 0, 0,
                         ANODE_ROOTDIR, 0)
        self._write_reserved(rootdirblk, draw)

        self._commit(touch_root_date=False)
        return self

    # -- verification -----------------------------------------------------
    def check(self, deep=False):
        """Structural validation: walk tree, resolve every anode chain."""
        errors = []
        warnings = []
        n_files = n_dirs = 0
        seen_anodes = set()
        try:
            for prefix, e in self.walk(""):
                path = (prefix + "/" if prefix else "") + e.name_str()
                if e.anode in seen_anodes and not e.is_link():
                    errors.append("%s: anode %d reused" % (path, e.anode))
                seen_anodes.add(e.anode)
                if e.is_dir():
                    n_dirs += 1
                elif e.is_file():
                    n_files += 1
                    try:
                        chain = self._anode_chain(e.anode)
                        total = sum(cs for cs, _ in chain) * self.bytes_per_block
                        if total < e.size:
                            errors.append(
                                "%s: anode chain holds %d bytes < size %d"
                                % (path, total, e.size)
                            )
                        for cs, bn in chain:
                            if bn + cs > self.total:
                                errors.append("%s: data run out of volume" % path)
                        if deep:
                            for _ in self._read_data(e):
                                pass
                    except FSError as ex:
                        errors.append("%s: %s" % (path, ex))
        except FSError as ex:
            errors.append("tree walk aborted: %s" % ex)
        return {
            "files": n_files,
            "dirs": n_dirs,
            "used_blocks": None,
            "errors": errors,
            "warnings": warnings,
            "ok": not errors,
        }
