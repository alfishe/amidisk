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
        # reserved block may span several device blocks: re-read whole thing
        self.root_raw = self._read_blocks(ROOTBLOCK, self.rescluster)
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
                self.rext = ext
                self.superindex_tab = struct.unpack_from(">16I", ext, 64)
                rd = struct.unpack_from(">3H", ext, 16)
                self.root_modified = amiga_to_datetime(*rd)
                vd = struct.unpack_from(">3H", ext, 22)
                self.volume_modified = amiga_to_datetime(*vd)
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
            "read_only": True,
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
