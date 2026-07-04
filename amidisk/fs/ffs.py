"""Native OFS/FFS filesystem engine (DOS\\0 .. DOS\\7), read and write.

Block layouts follow Laurent Clevy's ADF format documentation and were
cross-checked against amitools and ADFlib. Supports any filesystem
block size (de_SizeBlock * 4 * de_SecsPerBlk), as used by FFS 45+
volumes with 1-8 sectors per block.

All eight classic dostypes are fully supported, including write:
  DOS\\0/\\1  OFS / FFS
  DOS\\2/\\3  + international mode
  DOS\\4/\\5  + directory cache (cache chains maintained on every mutation)
  DOS\\6/\\7  + long filenames (LNFS, FFS 45+/OS 3.1.4): names up to 107
             chars in the combined name+comment field, overflow comments
             in separate T_COMMENT blocks, dates at the moved location
"""

import struct
from datetime import datetime

from .util import (
    amiga_to_datetime,
    datetime_to_amiga,
    hash_name,
    names_equal,
    protect_to_str,
)

# primary block types
T_HEADER = 2
T_DATA = 8
T_LIST = 16
T_DIRCACHE = 33
T_COMMENT = 64  # LNFS overflow comment block (DOS\6/\7)

# secondary types
ST_ROOT = 1
ST_USERDIR = 2
ST_SOFTLINK = 3
ST_LINKDIR = 4
ST_FILE = -3
ST_LINKFILE = -4

MAX_NAME = 30
MAX_COMMENT = 79
MAX_CHAIN = 1_000_000  # cycle guard


class FSError(Exception):
    pass


class BlockBuf:
    """Mutable filesystem block with longword accessors.

    Negative long indices address from the end of the block, matching
    the way the on-disk format defines its tail fields.
    """

    def __init__(self, data, bs):
        self.data = bytearray(data) if data is not None else bytearray(bs)
        self.bs = bs
        self.nl = bs // 4

    def _off(self, idx):
        if idx < 0:
            idx += self.nl
        return idx * 4

    def long(self, idx):
        return struct.unpack_from(">I", self.data, self._off(idx))[0]

    def slong(self, idx):
        return struct.unpack_from(">i", self.data, self._off(idx))[0]

    def put_long(self, idx, val):
        struct.pack_into(">I", self.data, self._off(idx), val & 0xFFFFFFFF)

    def put_slong(self, idx, val):
        struct.pack_into(">i", self.data, self._off(idx), val)

    def bstr(self, byte_off, max_chars):
        n = self.data[byte_off]
        if n > max_chars:
            n = max_chars
        return bytes(self.data[byte_off + 1 : byte_off + 1 + n])

    def put_bstr(self, byte_off, max_chars, val):
        val = val[:max_chars]
        self.data[byte_off] = len(val)
        self.data[byte_off + 1 : byte_off + 1 + max_chars] = val.ljust(max_chars, b"\x00")

    def sum_longs(self):
        s = 0
        for (v,) in struct.iter_unpack(">I", bytes(self.data)):
            s = (s + v) & 0xFFFFFFFF
        return s

    def fix_checksum(self, chk_long=5):
        self.put_long(chk_long, 0)
        self.put_long(chk_long, (-self.sum_longs()) & 0xFFFFFFFF)

    def checksum_ok(self):
        return self.sum_longs() == 0


class Entry:
    """Parsed directory entry (file / dir / link header block)."""

    __slots__ = (
        "blk", "type", "sec_type", "name", "size", "protect", "comment",
        "days", "mins", "ticks", "hash_chain", "parent", "extension",
        "high_seq", "first_data", "real_entry", "uid", "gid",
        "comment_block",
    )

    @classmethod
    def parse(cls, buf, blk, is_longname=False):
        e = cls()
        e.blk = blk
        e.type = buf.long(0)
        e.sec_type = buf.slong(-1)
        
        if is_longname:
            nac = buf.data[buf.bs - 184 : buf.bs - 72]
            name_len = min(nac[0], 107)
            e.name = bytes(nac[1 : name_len + 1])
            comment_len = nac[name_len + 1] if name_len + 1 < 112 else 0
            e.comment_block = 0
            if comment_len > 0:
                e.comment = bytes(nac[name_len + 2 : name_len + 2 + comment_len])
            else:
                e.comment = b""
                # an oversized comment lives in a separate T_COMMENT block
                e.comment_block = buf.long(-18)
            e.days = buf.long(-15)
            e.mins = buf.long(-14)
            e.ticks = buf.long(-13)
        else:
            e.comment_block = 0
            e.name = buf.bstr(buf.bs - 80, MAX_NAME)
            e.comment = buf.bstr(buf.bs - 184, MAX_COMMENT)
            e.days = buf.long(-23)
            e.mins = buf.long(-22)
            e.ticks = buf.long(-21)
            
        e.size = buf.long(-47)          # byte_size
        e.protect = buf.long(-48)
        e.hash_chain = buf.long(-4)
        e.parent = buf.long(-3)
        e.extension = buf.long(-2)
        e.high_seq = buf.long(2)
        e.first_data = buf.long(4)
        e.real_entry = buf.long(-11)    # for hard links
        uidgid = buf.long(-49)
        e.uid = uidgid >> 16
        e.gid = uidgid & 0xFFFF
        return e

    def is_dir(self):
        return self.sec_type in (ST_USERDIR, ST_ROOT)

    def is_file(self):
        return self.sec_type == ST_FILE

    def is_link(self):
        return self.sec_type in (ST_SOFTLINK, ST_LINKDIR, ST_LINKFILE)

    def type_str(self):
        return {
            ST_ROOT: "root",
            ST_USERDIR: "dir",
            ST_FILE: "file",
            ST_SOFTLINK: "softlink",
            ST_LINKDIR: "hardlink-dir",
            ST_LINKFILE: "hardlink-file",
        }.get(self.sec_type, "unknown(%d)" % self.sec_type)

    def mtime(self):
        return amiga_to_datetime(self.days, self.mins, self.ticks)

    def name_str(self):
        return self.name.decode("latin-1")

    def protect_str(self):
        return protect_to_str(self.protect)

    def get_info(self):
        return {
            "name": self.name_str(),
            "type": self.type_str(),
            "size": self.size if self.is_file() else None,
            "protect": self.protect_str(),
            "comment": self.comment.decode("latin-1"),
            "mtime": self.mtime().isoformat(sep=" ", timespec="seconds"),
            "block": self.blk,
        }


class Bitmap:
    """Block allocation bitmap: one bit per block, set = free."""

    def __init__(self, vol):
        self.vol = vol
        self.bits_per_page = (vol.bs - 4) * 8
        self.page_blks = []
        self.pages = []
        self.dirty = set()
        self._cursor = vol.root_blk + 1
        self._free_count = None

    def load(self):
        vol = self.vol
        needed = (vol.total - vol.reserved + self.bits_per_page - 1) // self.bits_per_page
        ptrs = []
        # 25 pointers in the root block
        root = vol.read_buf(vol.root_blk)
        for i in range(25):
            p = root.long(-49 + i)
            if p:
                ptrs.append(p)
        # extension chain
        ext = root.long(-24)
        guard = 0
        while ext and len(ptrs) < needed + 1024:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic bitmap extension chain")
            ebuf = vol.read_buf(ext)
            for i in range(vol.nl - 1):
                p = ebuf.long(i)
                if p:
                    ptrs.append(p)
            ext = ebuf.long(vol.nl - 1)
        if len(ptrs) < needed:
            raise FSError(
                "bitmap too small: %d pages, need %d" % (len(ptrs), needed)
            )
        self.page_blks = ptrs[:needed]
        self.pages = [self.vol.read_buf(p) for p in self.page_blks]
        for buf in self.pages:
            if not buf.checksum_ok():
                raise FSError("bitmap block checksum error")

    def _locate(self, blk):
        idx = blk - self.vol.reserved
        return idx // self.bits_per_page, idx % self.bits_per_page

    def is_free(self, blk):
        page, off = self._locate(blk)
        word = self.pages[page].long(1 + off // 32)
        return bool((word >> (off % 32)) & 1)

    def _set(self, blk, free):
        page, off = self._locate(blk)
        buf = self.pages[page]
        li = 1 + off // 32
        word = buf.long(li)
        bit = 1 << (off % 32)
        old_val = bool(word & bit)
        if free != old_val:
            buf.put_long(li, (word | bit) if free else (word & ~bit))
            self.dirty.add(page)
            if self._free_count is not None:
                self._free_count += (1 if free else -1)

    def alloc(self, count):
        """Allocate `count` blocks; returns list or raises FSError."""
        vol = self.vol
        # fail fast from the popcount instead of scanning the whole bitmap
        if count > self.count_free():
            raise FSError(
                "disk full: needed %d blocks, %d free" % (count, self.count_free())
            )
        found = []
        start = self._cursor if vol.reserved <= self._cursor < vol.total else vol.reserved
        blk = start
        total = vol.total
        wrapped = False
        while len(found) < count:
            idx = blk - vol.reserved
            if idx % 32 == 0 and blk + 32 <= total:
                page, off = self._locate(blk)
                buf = self.pages[page]
                li = 1 + off // 32
                word = buf.long(li)
                if word == 0:
                    # fully allocated: skip the word
                    blk += 32
                    if wrapped and start <= blk:
                        break
                    continue
                if word == 0xFFFFFFFF and len(found) + 32 <= count:
                    # fully free and we need all of it: grab the word
                    buf.put_long(li, 0)
                    self.dirty.add(page)
                    if self._free_count is not None:
                        self._free_count -= 32
                    found.extend(range(blk, blk + 32))
                    blk += 32
                    continue
            if blk >= total:
                blk = vol.reserved
                wrapped = True
            if wrapped and blk >= start:
                for b in found:
                    self._set(b, True)
                raise FSError("disk full: needed %d blocks" % count)
            if self.is_free(blk):
                self._set(blk, False)
                found.append(blk)
            blk += 1
        self._cursor = blk
        return found

    def free(self, blks):
        for b in blks:
            self._set(b, True)

    def count_free(self):
        if self._free_count is not None:
            return self._free_count
        vol = self.vol
        valid_bits = vol.total - vol.reserved
        free = 0
        for pi, buf in enumerate(self.pages):
            base = pi * self.bits_per_page
            data = bytes(buf.data[4 : 4 + (self.bits_per_page // 8)])
            n = int.from_bytes(data, "little")  # bit order irrelevant for popcount
            cnt = bin(n).count("1")
            over = base + self.bits_per_page - valid_bits
            if over > 0:
                # mask out pad bits beyond the volume in the last page
                cnt = 0
                for off in range(min(self.bits_per_page, valid_bits - base)):
                    word = buf.long(1 + off // 32)
                    cnt += (word >> (off % 32)) & 1
            free += cnt
        self._free_count = free
        return free

    def flush(self):
        for page in sorted(self.dirty):
            buf = self.pages[page]
            buf.fix_checksum(0)
            self.vol.write_buf(self.page_blks[page], buf)
        self.dirty.clear()


class FFSVolume:
    """One mounted OFS/FFS volume."""

    def __init__(self, blkdev, sec_per_blk=1, reserved=2, dos_type=None):
        self.dev = blkdev
        self.spb = max(sec_per_blk, 1)
        self.bs = blkdev.block_bytes * self.spb
        self.nl = self.bs // 4
        self.tsz = self.nl - 56
        self.total = blkdev.num_blocks // self.spb
        self.reserved = reserved if reserved > 0 else 2
        self.root_blk = (self.reserved + self.total - 1) // 2
        self.read_only = blkdev.read_only
        self.dos_type = dos_type
        self.bitmap = None
        self.label = None

    # -- raw block I/O -------------------------------------------------
    def read_buf(self, blk):
        if blk < 1 or blk >= self.total:
            raise FSError("block %d out of volume range" % blk)
        return BlockBuf(self.dev.read(blk * self.spb, self.spb), self.bs)

    def write_buf(self, blk, buf):
        if self.read_only:
            raise FSError("volume is read-only")
        if blk < self.reserved or blk >= self.total:
            raise FSError("write to block %d out of volume range" % blk)
        self.dev.write(blk * self.spb, bytes(buf.data))

    # -- mount ----------------------------------------------------------
    def open(self):
        # dostype from the bootblock when present, else from the caller
        try:
            boot = self.dev.read(0)
            if boot[0:3] == b"DOS":
                self.dos_type = struct.unpack(">I", boot[0:4])[0]
        except Exception:
            pass
        if self.dos_type is None or (self.dos_type >> 8) != 0x444F53:
            raise FSError("not an AmigaDOS (DOS\\x) volume")
        flavor = self.dos_type & 0xFF
        if flavor > 7:
            raise FSError("unsupported DOS type flavor %d" % flavor)
        self.ffs = bool(flavor & 1)
        self.intl = flavor >= 2
        self.dircache = flavor in (4, 5)
        self.is_longname = flavor in (6, 7)
        self.max_name_len = 107 if self.is_longname else MAX_NAME

        root = self.read_buf(self.root_blk)
        if not (
            root.long(0) == T_HEADER
            and root.slong(-1) == ST_ROOT
            and root.checksum_ok()
        ):
            raise FSError(
                "no valid root block at %d (block size %d)" % (self.root_blk, self.bs)
            )
        self.label = root.bstr(self.bs - 80, MAX_NAME).decode("latin-1")
        self.bitmap = Bitmap(self)
        self.bitmap.load()
        return self

    def dos_type_str(self):
        from ..rdb.blocks import dos_type_to_str

        return dos_type_to_str(self.dos_type)

    def root_entry(self):
        buf = self.read_buf(self.root_blk)
        e = Entry.parse(buf, self.root_blk, self.is_longname)
        e.name = self.label.encode("latin-1")
        return e

    def get_info(self):
        root = self.read_buf(self.root_blk)
        free = self.bitmap.count_free()
        used = self.total - self.reserved - free
        return {
            "label": self.label,
            "dos_type": self.dos_type_str(),
            "block_size": self.bs,
            "total_blocks": self.total,
            "used_blocks": used,
            "free_blocks": free,
            "free_bytes": free * self.bs,
            "root_block": self.root_blk,
            "created": amiga_to_datetime(
                root.long(-7), root.long(-6), root.long(-5)
            ).isoformat(sep=" ", timespec="seconds"),
            "modified": amiga_to_datetime(
                root.long(-10), root.long(-9), root.long(-8)
            ).isoformat(sep=" ", timespec="seconds"),
            "read_only": self.read_only,
        }

    # -- path resolution --------------------------------------------------
    @staticmethod
    def _split(path):
        if isinstance(path, str):
            path = path.encode("latin-1", errors="replace")
        parts = [p for p in path.replace(b"\\", b"/").split(b"/") if p]
        return parts

    def _fill_comment(self, e):
        """Load an LNFS overflow comment from its T_COMMENT block."""
        if getattr(e, "comment_block", 0) and not e.comment:
            try:
                cbuf = self.read_buf(e.comment_block)
                if cbuf.long(0) == T_COMMENT:
                    e.comment = cbuf.bstr(24, MAX_COMMENT)
            except FSError:
                pass
        return e

    def resolve(self, path):
        """Return Entry for path ('' or '/' = root) or raise FSError."""
        parts = self._split(path)
        cur = self.root_entry()
        cur_buf = self.read_buf(self.root_blk)
        for seg in parts:
            if not cur.is_dir():
                raise FSError("'%s' is not a directory" % cur.name_str())
            nxt = self._find_in_dir(cur_buf, seg)
            if nxt is None:
                raise FSError("path not found: %s" % path)
            cur = nxt
            cur_buf = self.read_buf(cur.blk)
        self._fill_comment(cur)
        return cur

    def _find_in_dir(self, dir_buf, name):
        h = hash_name(name, self.tsz, self.intl)
        blk = dir_buf.long(6 + h)
        guard = 0
        while blk:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic hash chain")
            buf = self.read_buf(blk)
            e = Entry.parse(buf, blk, self.is_longname)
            if names_equal(e.name, name, self.intl):
                return e
            blk = e.hash_chain
        return None

    def list_dir(self, path=""):
        e = self.resolve(path)
        if not e.is_dir():
            raise FSError("not a directory: %s" % path)
        entries = self._list_entries(e.blk)
        if self.is_longname:
            for ent in entries:
                self._fill_comment(ent)
        return entries

    def _list_entries(self, dir_blk, sort=True):
        buf = self.read_buf(dir_blk)
        entries = []
        for i in range(self.tsz):
            blk = buf.long(6 + i)
            guard = 0
            while blk:
                guard += 1
                if guard > MAX_CHAIN:
                    raise FSError("cyclic hash chain in dir block %d" % dir_blk)
                ebuf = self.read_buf(blk)
                e = Entry.parse(ebuf, blk, self.is_longname)
                entries.append(e)
                blk = e.hash_chain
        if sort:
            entries.sort(key=lambda e: e.name.lower())
        return entries

    def walk(self, path=""):
        """Yield (dir_path_str, Entry) recursively, depth-first."""
        start = self.resolve(path)
        if not start.is_dir():
            raise FSError("not a directory: %s" % path)
        base = "/".join(p.decode("latin-1") for p in self._split(path))
        stack = [(base, start.blk)]
        while stack:
            prefix, blk = stack.pop()
            for e in self._list_entries(blk):
                yield prefix, e
                if e.sec_type == ST_USERDIR:
                    sub = (prefix + "/" if prefix else "") + e.name_str()
                    stack.append((sub, e.blk))

    # -- file reading ------------------------------------------------------
    def read_file(self, path):
        """Return an iterator over the file's data chunks."""
        e = self.resolve(path) if not isinstance(path, Entry) else path
        if e.sec_type == ST_LINKFILE and e.real_entry:
            e = Entry.parse(self.read_buf(e.real_entry), e.real_entry, self.is_longname)
        if not e.is_file():
            raise FSError("not a file: %s" % e.name_str())
        return self._read_data(e)

    def read_file_bytes(self, path):
        return b"".join(self.read_file(path))

    def _data_block_table(self, entry):
        """All data-block pointers of a file, in order."""
        ptrs = []
        blk = entry.blk
        guard = 0
        data_bytes = self.bs if self.ffs else self.bs - 24
        need = (entry.size + data_bytes - 1) // data_bytes
        while blk and len(ptrs) < need:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic extension chain in file %s" % entry.name_str())
            buf = self.read_buf(blk)
            count = buf.long(2)
            if count == 0:
                raise FSError("truncated file %s" % entry.name_str())
            for k in range(min(count, self.tsz)):
                ptr = buf.long(6 + self.tsz - 1 - k)
                if ptr == 0:
                    raise FSError("hole in data block table of %s" % entry.name_str())
                ptrs.append(ptr)
            blk = buf.long(-2)
        if len(ptrs) < need:
            raise FSError("missing extension block in %s" % entry.name_str())
        return ptrs[:need]

    def _read_data(self, entry):
        """Yield the file's data, coalescing consecutive blocks into
        large reads (one syscall per run instead of one per block)."""
        remaining = entry.size
        if remaining <= 0:
            return
        ptrs = self._data_block_table(entry)
        data_bytes = self.bs if self.ffs else self.bs - 24
        MAX_RUN = max(1, (4 << 20) // self.bs)
        i = 0
        n = len(ptrs)
        while i < n and remaining > 0:
            j = i + 1
            while j < n and j - i < MAX_RUN and ptrs[j] == ptrs[j - 1] + 1:
                j += 1
            raw = self.dev.read(ptrs[i] * self.spb, (j - i) * self.spb)
            if self.ffs:
                take = min(len(raw), remaining)
                yield raw[:take] if take != len(raw) else raw
                remaining -= take
            else:
                for k in range(j - i):
                    o = k * self.bs
                    btype, _hk, _seq, dsize = struct.unpack_from(">4I", raw, o)
                    if btype != T_DATA:
                        raise FSError("bad OFS data block %d" % ptrs[i + k])
                    take = min(dsize, remaining, data_bytes)
                    yield raw[o + 24 : o + 24 + take]
                    remaining -= take
                    if remaining <= 0:
                        break
            i = j

    def _now_stamp(self, buf, long_idx, when=None):
        d, m, t = datetime_to_amiga(when or datetime.now())
        buf.put_long(long_idx, d)
        buf.put_long(long_idx + 1, m)
        buf.put_long(long_idx + 2, t)

    def _touch_volume(self):
        root = self.read_buf(self.root_blk)
        self._now_stamp(root, -10)  # volume modified date
        root.fix_checksum()
        self.write_buf(self.root_blk, root)

    def _date_loc(self, blk):
        """Longword index of an entry's modification date. On LNFS volumes
        the date moved to -15 because -23 sits inside the combined
        name+comment field; the root block keeps the classic layout."""
        if self.is_longname and blk != self.root_blk:
            return -15
        return -23

    def _touch_dir(self, dir_blk):
        buf = self.read_buf(dir_blk)
        self._now_stamp(buf, self._date_loc(dir_blk))
        buf.fix_checksum()
        self.write_buf(dir_blk, buf)

    @staticmethod
    def _check_name(name, limit=MAX_NAME):
        if isinstance(name, str):
            name = name.encode("latin-1", errors="replace")
        if not 1 <= len(name) <= limit:
            raise FSError("invalid name length: %r" % name)
        if b":" in name or b"/" in name or any(c < 32 for c in name):
            raise FSError("invalid characters in name: %r" % name)
        return name

    def _require_writable(self):
        if self.read_only:
            raise FSError(
                "volume is read-only"
                + (" (dircache volumes are not writable)" if getattr(self, "dircache", False) else "")
            )

    def _resolve_parent(self, path):
        """Split path into (parent Entry, name bytes)."""
        parts = self._split(path)
        if not parts:
            raise FSError("empty path")
        name = self._check_name(parts[-1], self.max_name_len)
        parent_path = b"/".join(parts[:-1])
        parent = self.resolve(parent_path)
        if not parent.is_dir():
            raise FSError("parent is not a directory")
        return parent, name

    def _link_entry(self, dir_blk, new_blk, name):
        """Insert block into the dir's hash chain (ascending block order)."""
        h = hash_name(name, self.tsz, self.intl)
        dbuf = self.read_buf(dir_blk)
        chain = []
        blk = dbuf.long(6 + h)
        guard = 0
        while blk:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic hash chain")
            chain.append(blk)
            blk = Entry.parse(self.read_buf(blk), blk, self.is_longname).hash_chain
        # insertion point: keep chain sorted ascending by block number
        pos = 0
        while pos < len(chain) and chain[pos] < new_blk:
            pos += 1
        succ = chain[pos] if pos < len(chain) else 0
        nbuf = self.read_buf(new_blk)
        nbuf.put_long(-4, succ)
        nbuf.fix_checksum()
        self.write_buf(new_blk, nbuf)
        if pos == 0:
            dbuf.put_long(6 + h, new_blk)
            self._now_stamp(dbuf, self._date_loc(dir_blk))
            dbuf.fix_checksum()
            self.write_buf(dir_blk, dbuf)
        else:
            prev = chain[pos - 1]
            pbuf = self.read_buf(prev)
            pbuf.put_long(-4, new_blk)
            pbuf.fix_checksum()
            self.write_buf(prev, pbuf)
            self._touch_dir(dir_blk)

    def _unlink_entry(self, entry):
        """Remove entry from its parent's hash chain."""
        dir_blk = entry.parent
        h = hash_name(entry.name, self.tsz, self.intl)
        dbuf = self.read_buf(dir_blk)
        blk = dbuf.long(6 + h)
        prev = None
        guard = 0
        while blk and blk != entry.blk:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic hash chain")
            prev = blk
            blk = Entry.parse(self.read_buf(blk), blk, self.is_longname).hash_chain
        if blk != entry.blk:
            raise FSError("entry %s not found in parent chain" % entry.name_str())
        if prev is None:
            dbuf.put_long(6 + h, entry.hash_chain)
            self._now_stamp(dbuf, self._date_loc(dir_blk))
            dbuf.fix_checksum()
            self.write_buf(dir_blk, dbuf)
        else:
            pbuf = self.read_buf(prev)
            pbuf.put_long(-4, entry.hash_chain)
            pbuf.fix_checksum()
            self.write_buf(prev, pbuf)
            self._touch_dir(dir_blk)

    def _new_header(self, blk, sec_type, parent_blk, name, protect=0, comment=b""):
        buf = BlockBuf(None, self.bs)
        buf.put_long(0, T_HEADER)
        buf.put_long(1, blk)
        buf.put_slong(-1, sec_type)
        buf.put_long(-3, parent_blk)
        buf.put_long(-48, protect)
        
        if self.is_longname:
            comment = comment[:MAX_COMMENT]
            nac = bytearray(112)
            nac[0] = len(name)
            nac[1 : 1 + len(name)] = name
            c_offset = 1 + len(name)
            if comment and 2 + len(name) + len(comment) > 112:
                # comment does not fit inline: store it in a T_COMMENT block
                nac[c_offset] = 0
                (cblk,) = self.bitmap.alloc(1)
                cbuf = BlockBuf(None, self.bs)
                cbuf.put_long(0, T_COMMENT)
                cbuf.put_long(1, cblk)
                cbuf.put_long(2, blk)  # header_key of the owning entry
                cbuf.put_bstr(24, MAX_COMMENT, comment)
                cbuf.fix_checksum()
                self.write_buf(cblk, cbuf)
                buf.put_long(-18, cblk)
            else:
                nac[c_offset] = len(comment)
                if comment:
                    nac[c_offset + 1 : c_offset + 1 + len(comment)] = comment
            buf.data[buf.bs - 184 : buf.bs - 72] = nac
            self._now_stamp(buf, -15)
        else:
            buf.put_bstr(self.bs - 80, MAX_NAME, name)
            buf.put_bstr(self.bs - 184, MAX_COMMENT, comment)
            self._now_stamp(buf, -23)
        return buf

    # -- dircache (DOS\4/\5) maintenance -------------------------------------
    def _dc_chain(self, dir_buf):
        """Existing dircache chain blocks of a directory."""
        chain = []
        blk = dir_buf.long(-2)
        guard = 0
        while blk:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic dircache chain")
            cbuf = self.read_buf(blk)
            if cbuf.long(0) != T_DIRCACHE:
                break
            chain.append(blk)
            blk = cbuf.long(4)
        return chain

    @staticmethod
    def _dc_record(e):
        rec = bytearray(24)
        struct.pack_into(">3I", rec, 0, e.blk, e.size if e.is_file() else 0, e.protect)
        struct.pack_into(">2H", rec, 12, e.uid, e.gid)
        struct.pack_into(
            ">3H", rec, 16, e.days & 0xFFFF, e.mins & 0xFFFF, e.ticks & 0xFFFF
        )
        rec[22] = e.sec_type & 0xFF
        rec[23] = len(e.name)
        rec += e.name + bytes([len(e.comment)]) + e.comment
        if len(rec) & 1:
            rec += b"\x00"
        return bytes(rec)

    def _write_dc_blocks(self, dir_blk, records):
        """Write a fresh dircache chain, return its first block."""
        groups = [[]]
        used = 0
        cap = self.bs - 24
        for r in records:
            if used + len(r) > cap:
                groups.append([])
                used = 0
            groups[-1].append(r)
            used += len(r)
        blks = self.bitmap.alloc(len(groups))
        for i, (g, blk) in enumerate(zip(groups, blks)):
            buf = BlockBuf(None, self.bs)
            buf.put_long(0, T_DIRCACHE)
            buf.put_long(1, blk)
            buf.put_long(2, dir_blk)
            buf.put_long(3, len(g))
            buf.put_long(4, blks[i + 1] if i + 1 < len(blks) else 0)
            payload = b"".join(g)
            buf.data[24 : 24 + len(payload)] = payload
            buf.fix_checksum()
            self.write_buf(blk, buf)
        return blks[0]

    def _update_dircache(self, dir_blk):
        """Rebuild the dircache chain of one directory from its hash table."""
        if not getattr(self, "dircache", False):
            return
        dbuf = self.read_buf(dir_blk)
        self.bitmap.free(self._dc_chain(dbuf))
        records = [
            self._dc_record(e) for e in self._list_entries(dir_blk, sort=False)
        ]
        first = self._write_dc_blocks(dir_blk, records)
        dbuf = self.read_buf(dir_blk)
        dbuf.put_long(-2, first)
        dbuf.fix_checksum()
        self.write_buf(dir_blk, dbuf)

    # -- mutations ---------------------------------------------------------
    def mkdir(self, path):
        self._require_writable()
        parent, name = self._resolve_parent(path)
        pbuf = self.read_buf(parent.blk)
        if self._find_in_dir(pbuf, name):
            raise FSError("already exists: %s" % path)
        (blk,) = self.bitmap.alloc(1)
        buf = self._new_header(blk, ST_USERDIR, parent.blk, name)
        buf.fix_checksum()
        self.write_buf(blk, buf)
        self._link_entry(parent.blk, blk, name)
        if getattr(self, "dircache", False):
            # fresh directory carries an empty cache block of its own
            first = self._write_dc_blocks(blk, [])
            nbuf = self.read_buf(blk)
            nbuf.put_long(-2, first)
            nbuf.fix_checksum()
            self.write_buf(blk, nbuf)
            self._update_dircache(parent.blk)
        self.bitmap.flush()
        self._touch_volume()
        return blk

    def makedirs(self, path):
        parts = self._split(path)
        cur = b""
        for seg in parts:
            cur = cur + b"/" + seg if cur else seg
            try:
                e = self.resolve(cur)
                if not e.is_dir():
                    raise FSError("'%s' exists and is not a directory" % cur.decode("latin-1"))
            except FSError as ex:
                if "not found" not in str(ex):
                    raise
                self.mkdir(cur)

    def write_file(self, path, data, size=None, protect=0, comment=b"", mtime=None):
        """Create or replace a file with `data` (bytes or iterator of bytes)."""
        self._require_writable()
        parent, name = self._resolve_parent(path)
        pbuf = self.read_buf(parent.blk)
        existing = self._find_in_dir(pbuf, name)
        if existing is not None:
            if not existing.is_file():
                raise FSError("exists and is not a file: %s" % path)
            self.delete(existing)
            parent = Entry.parse(self.read_buf(parent.blk), parent.blk, self.is_longname)

        if size is None:
            size = len(data)
        if size >= 1 << 32:
            raise FSError("files >= 4 GB not supported (byte_size is 32-bit)")

        data_bytes = self.bs if self.ffs else self.bs - 24
        ndata = (size + data_bytes - 1) // data_bytes
        next_ext = max(0, ndata - self.tsz)
        next_blocks = (next_ext + self.tsz - 1) // self.tsz
        blocks = self.bitmap.alloc(1 + ndata + next_blocks)
        hdr_blk = blocks[0]
        data_blks = blocks[1 : 1 + ndata]
        ext_blks = blocks[1 + ndata :]

        try:
            # data blocks: coalesce consecutive allocations into large
            # writes (one syscall per run instead of one per 512 bytes)
            if isinstance(data, (bytes, bytearray, memoryview)):
                src = memoryview(data)
                pos = [0]

                def pull(n):
                    chunk = bytes(src[pos[0] : pos[0] + n])
                    pos[0] += n
                    if len(chunk) < n:
                        raise FSError("stream ended prematurely")
                    return chunk
            else:
                it = iter(data)
                sbuf = bytearray()

                def pull(n):
                    while len(sbuf) < n:
                        try:
                            sbuf.extend(next(it))
                        except StopIteration:
                            raise FSError("stream ended prematurely")
                    chunk = bytes(sbuf[:n])
                    del sbuf[:n]
                    return chunk

            MAX_RUN = max(1, (4 << 20) // self.bs)  # cap run buffers at 4 MB
            i = 0
            while i < ndata:
                # extend the run while block numbers stay consecutive
                j = i + 1
                while (j < ndata and j - i < MAX_RUN
                       and data_blks[j] == data_blks[j - 1] + 1):
                    j += 1
                want = min((j - i) * data_bytes, size - i * data_bytes)
                payload = pull(want)
                if self.ffs:
                    pad = (j - i) * self.bs - len(payload)
                    if pad:
                        payload += b"\x00" * pad
                    self.dev.write(data_blks[i] * self.spb, payload)
                else:
                    run = bytearray((j - i) * self.bs)
                    for k in range(i, j):
                        chunk = payload[(k - i) * data_bytes :
                                        (k - i + 1) * data_bytes]
                        o = (k - i) * self.bs
                        struct.pack_into(
                            ">5I", run, o, T_DATA, hdr_blk, k + 1, len(chunk),
                            data_blks[k + 1] if k + 1 < ndata else 0,
                        )
                        run[o + 24 : o + 24 + len(chunk)] = chunk
                        s = 0
                        for (v,) in struct.iter_unpack(
                                ">I", bytes(run[o : o + self.bs])):
                            s = (s + v) & 0xFFFFFFFF
                        struct.pack_into(">I", run, o + 20, (-s) & 0xFFFFFFFF)
                    self.dev.write(data_blks[i] * self.spb, bytes(run))
                i = j

            # extension blocks (written last-to-first to know the next pointer)
            next_ext_ptr = 0
            for x in range(next_blocks - 1, -1, -1):
                blk = ext_blks[x]
                part = data_blks[self.tsz * (x + 1) : self.tsz * (x + 2)]
                buf = BlockBuf(None, self.bs)
                buf.put_long(0, T_LIST)
                buf.put_long(1, blk)
                buf.put_long(2, len(part))
                for k, p in enumerate(part):
                    buf.put_long(6 + self.tsz - 1 - k, p)
                buf.put_long(-3, hdr_blk)
                buf.put_long(-2, next_ext_ptr)
                buf.put_slong(-1, ST_FILE)
                buf.fix_checksum()
                self.write_buf(blk, buf)
                next_ext_ptr = blk

            # file header
            buf = self._new_header(hdr_blk, ST_FILE, parent.blk, name, protect, comment)
            if mtime is not None:
                self._now_stamp(buf, self._date_loc(hdr_blk), mtime)
            head = data_blks[: self.tsz]
            buf.put_long(2, len(head))
            buf.put_long(4, data_blks[0] if data_blks else 0)
            for k, p in enumerate(head):
                buf.put_long(6 + self.tsz - 1 - k, p)
            buf.put_long(-47, size)
            buf.put_long(-2, next_ext_ptr)
            buf.fix_checksum()
            self.write_buf(hdr_blk, buf)
            
        except Exception:
            self.bitmap.free(blocks)
            self.bitmap.flush()
            raise

        self._link_entry(parent.blk, hdr_blk, name)
        self._update_dircache(parent.blk)
        self.bitmap.flush()
        self._touch_volume()
        return hdr_blk

    def _entry_blocks(self, entry):
        """All blocks belonging to a file/link header (header, exts, data)."""
        blocks = [entry.blk]
        if getattr(entry, "comment_block", 0):
            blocks.append(entry.comment_block)
        if entry.sec_type in (ST_SOFTLINK, ST_LINKFILE, ST_LINKDIR):
            return blocks
        if entry.sec_type == ST_USERDIR:
            if getattr(self, "dircache", False):
                blocks += self._dc_chain(self.read_buf(entry.blk))
            return blocks
        blk = entry.blk
        guard = 0
        while blk:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic extension chain")
            buf = self.read_buf(blk)
            count = buf.long(2)
            for k in range(min(count, self.tsz)):
                ptr = buf.long(6 + self.tsz - 1 - k)
                if ptr:
                    blocks.append(ptr)
            blk = buf.long(-2)
            if blk:
                blocks.append(blk)
        return blocks

    def delete(self, path, recursive=False):
        self._require_writable()
        entry = path if isinstance(path, Entry) else self.resolve(path)
        if entry.sec_type == ST_ROOT:
            raise FSError("cannot delete the root directory")
        if entry.sec_type == ST_USERDIR:
            children = self._list_entries(entry.blk)
            if children and not recursive:
                raise FSError("directory not empty: %s" % entry.name_str())
            for child in children:
                self.delete(child, recursive=True)
            # re-read: child deletions touched this dir block
            entry = Entry.parse(self.read_buf(entry.blk), entry.blk, self.is_longname)
        self._unlink_entry(entry)
        self.bitmap.free(self._entry_blocks(entry))
        self._update_dircache(entry.parent)
        self.bitmap.flush()
        self._touch_volume()

    def rename(self, src, dst):
        self._require_writable()
        entry = self.resolve(src)
        if entry.sec_type == ST_ROOT:
            raise FSError("cannot rename the root directory")
        new_parent, new_name = self._resolve_parent(dst)
        dbuf = self.read_buf(new_parent.blk)
        clash = self._find_in_dir(dbuf, new_name)
        if clash is not None and clash.blk != entry.blk:
            raise FSError("destination exists: %s" % dst)
        # move a dir into itself?
        if entry.sec_type == ST_USERDIR:
            p = new_parent.blk
            while p and p != self.root_blk:
                if p == entry.blk:
                    raise FSError("cannot move a directory into itself")
                p = Entry.parse(self.read_buf(p), p, self.is_longname).parent
        self._unlink_entry(entry)
        buf = self.read_buf(entry.blk)
        if self.is_longname:
            nac = buf.data[buf.bs - 184 : buf.bs - 72]
            name_len = nac[0]
            comment_len = nac[name_len + 1]
            comment = nac[name_len + 2 : name_len + 2 + comment_len] if comment_len > 0 else b""

            new_nac = bytearray(112)
            new_nac[0] = len(new_name)
            new_nac[1 : 1 + len(new_name)] = new_name
            c_offset = 1 + len(new_name)
            if comment and 2 + len(new_name) + len(comment) > 112:
                # the longer name pushed the inline comment out: move it
                # to a T_COMMENT block (an existing block ptr is kept as-is)
                new_nac[c_offset] = 0
                (cblk,) = self.bitmap.alloc(1)
                cbuf = BlockBuf(None, self.bs)
                cbuf.put_long(0, T_COMMENT)
                cbuf.put_long(1, cblk)
                cbuf.put_long(2, entry.blk)
                cbuf.put_bstr(24, MAX_COMMENT, bytes(comment))
                cbuf.fix_checksum()
                self.write_buf(cblk, cbuf)
                buf.put_long(-18, cblk)
                self.bitmap.flush()
            else:
                new_nac[c_offset] = len(comment)
                if len(comment) > 0:
                    new_nac[c_offset + 1 : c_offset + 1 + len(comment)] = comment
            buf.data[buf.bs - 184 : buf.bs - 72] = new_nac
        else:
            buf.put_bstr(self.bs - 80, MAX_NAME, new_name)
        buf.put_long(-3, new_parent.blk)
        buf.fix_checksum()
        self.write_buf(entry.blk, buf)
        self._link_entry(new_parent.blk, entry.blk, new_name)
        self._update_dircache(entry.parent)
        if new_parent.blk != entry.parent:
            self._update_dircache(new_parent.blk)
        self.bitmap.flush()
        self._touch_volume()

    # -- formatting ----------------------------------------------------------
    def format(self, label, dos_type=None):
        """Write a fresh empty filesystem (bootblock, root, bitmap)."""
        if self.dev.read_only:
            raise FSError("device is read-only")
        if dos_type is not None:
            self.dos_type = dos_type
        if self.dos_type is None or (self.dos_type >> 8) != 0x444F53:
            raise FSError("dos_type required (e.g. 0x444F5303 for DOS\\3)")
        flavor = self.dos_type & 0xFF
        if flavor > 7:
            raise FSError("cannot format DOS\\%d volumes" % flavor)
        self.ffs = bool(flavor & 1)
        self.intl = flavor >= 2
        self.dircache = flavor in (4, 5)
        self.is_longname = flavor in (6, 7)
        self.max_name_len = 107 if self.is_longname else MAX_NAME
        self.read_only = False
        label = self._check_name(label)  # volume labels stay <= 30 chars
        if self.total - self.reserved < 8:
            raise FSError("volume too small")

        # bitmap geometry: pages directly after the root block
        bits_per_page = (self.bs - 4) * 8
        npages = (self.total - self.reserved + bits_per_page - 1) // bits_per_page
        next_blocks = 0
        if npages > 25:
            per_ext = self.nl - 1
            next_blocks = (npages - 25 + per_ext - 1) // per_ext
        page_blks = [self.root_blk + 1 + i for i in range(npages)]
        ext_blks = [self.root_blk + 1 + npages + i for i in range(next_blocks)]
        if page_blks + ext_blks and (self.root_blk + 1 + npages + next_blocks) > self.total:
            raise FSError("volume too small for its bitmap")

        # bootblock: dostype magic, rest zeros (non-bootable)
        boot = bytearray(self.bs * min(self.reserved, 2))
        struct.pack_into(">I", boot, 0, self.dos_type)
        self.dev.write(0, bytes(boot))

        # bitmap pages: everything free, then allocate root + bitmap itself
        pages = [BlockBuf(None, self.bs) for _ in range(npages)]
        valid_bits = self.total - self.reserved
        for pi, buf in enumerate(pages):
            base = pi * bits_per_page
            for li in range(1, self.nl):
                word_base = base + (li - 1) * 32
                if word_base >= valid_bits:
                    break
                word = 0
                for b in range(min(32, valid_bits - word_base)):
                    word |= 1 << b
                buf.put_long(li, word)
        self.bitmap = Bitmap(self)
        self.bitmap.page_blks = page_blks
        self.bitmap.pages = pages
        for blk in [self.root_blk] + page_blks + ext_blks:
            self.bitmap._set(blk, False)
        self.bitmap.dirty = set(range(npages))
        self.bitmap.flush()

        # bitmap extension chain
        idx = 25
        for x, eblk in enumerate(ext_blks):
            ebuf = BlockBuf(None, self.bs)
            for i in range(self.nl - 1):
                if idx < npages:
                    ebuf.put_long(i, page_blks[idx])
                    idx += 1
            ebuf.put_long(self.nl - 1, ext_blks[x + 1] if x + 1 < next_blocks else 0)
            self.dev.write(eblk * self.spb, bytes(ebuf.data))

        # root block
        root = BlockBuf(None, self.bs)
        root.put_long(0, T_HEADER)
        root.put_long(3, self.tsz)
        root.put_slong(-50, -1)  # bm_flag = valid
        for i in range(min(25, npages)):
            root.put_long(-49 + i, page_blks[i])
        root.put_long(-24, ext_blks[0] if ext_blks else 0)
        root.put_bstr(self.bs - 80, MAX_NAME, label)
        self._now_stamp(root, -23)
        self._now_stamp(root, -10)
        self._now_stamp(root, -7)
        root.put_slong(-1, ST_ROOT)
        if self.dircache:
            root.put_long(-2, self._write_dc_blocks(self.root_blk, []))
            self.bitmap.flush()
        root.fix_checksum()
        self.write_buf(self.root_blk, root)

        self.label = label.decode("latin-1")
        return self

    # -- repair ---------------------------------------------------------------
    def collect_used_blocks(self):
        """Every block reachable from the directory tree + fs metadata.

        Returns (used_set, problems) where problems lists unreadable
        subtrees that made the scan incomplete.
        """
        used = {self.root_blk}
        problems = []
        root = self.read_buf(self.root_blk)
        used.update(self.bitmap.page_blks)
        ext = root.long(-24)
        guard = 0
        while ext and guard < MAX_CHAIN:
            guard += 1
            used.add(ext)
            ext = self.read_buf(ext).long(self.nl - 1)
        for prefix, e in self.walk(""):
            path = (prefix + "/" if prefix else "") + e.name_str()
            try:
                used.update(self._entry_blocks(e))
            except FSError as ex:
                problems.append("%s: %s" % (path, ex))
        if getattr(self, "dircache", False):
            used.update(self._dc_chain(root))
        return used, problems

    def repair(self, apply=False):
        """Rebuild the allocation bitmap from the directory tree.

        Fixes lost blocks (allocated but unreachable) and, critically,
        blocks in use but marked free. Returns a report dict; only
        writes when apply=True.
        """
        if apply and self.dev.read_only:
            raise FSError("device opened read-only")
        used, problems = self.collect_used_blocks()
        wrong_free = []   # used by tree but marked free (dangerous)
        wrong_used = []   # marked used but unreachable (lost blocks)
        for pi, actual, expect, base_idx, limit in self._bitmap_page_diffs(used):
            for off in range(limit):
                w, b = divmod(off, 32)
                byte = w * 4 + (3 - b // 8)
                a = (actual[byte] >> (b % 8)) & 1
                e = (expect[byte] >> (b % 8)) & 1
                if a == e:
                    continue
                blk = self.reserved + base_idx + off
                (wrong_free if e == 0 else wrong_used).append(blk)
        report = {
            "used_blocks": len(used),
            "alloc_missing": len(wrong_free),
            "lost_blocks": len(wrong_used),
            "problems": problems,
            "applied": False,
        }
        if apply and (wrong_free or wrong_used):
            for blk in wrong_free:
                self.bitmap._set(blk, False)
            for blk in wrong_used:
                self.bitmap._set(blk, True)
            self.bitmap.flush()
            # bm_flag: mark bitmap valid again
            root = self.read_buf(self.root_blk)
            root.put_slong(-50, -1)
            root.fix_checksum()
            self.write_buf(self.root_blk, root)
            report["applied"] = True
        return report

    def _bitmap_page_diffs(self, used):
        """Yield (page_idx, actual, expected, base_bit_idx, valid_bits) for
        bitmap pages that differ from the state implied by `used`.

        The expected free-bitmap is built once (bit set = free, cleared for
        every tree-reachable block) so page comparison runs at bytes-compare
        speed; only damaged pages are examined bit by bit by the caller.
        """
        page_bytes = self.bs - 4
        bpp = self.bitmap.bits_per_page
        npages = len(self.bitmap.pages)
        expected = bytearray(b"\xff" * (npages * page_bytes))
        valid = self.total - self.reserved
        for blk in used:
            idx = blk - self.reserved
            if 0 <= idx < valid:
                w, b = divmod(idx, 32)
                expected[w * 4 + (3 - b // 8)] &= ~(1 << (b % 8)) & 0xFF
        for pi in range(npages):
            actual = bytes(self.bitmap.pages[pi].data[4 : 4 + page_bytes])
            expect = bytes(expected[pi * page_bytes : (pi + 1) * page_bytes])
            base_idx = pi * bpp
            limit = min(bpp, valid - base_idx)
            if limit <= 0:
                break
            if limit == bpp and actual == expect:
                continue  # clean full page: nothing to look at
            yield pi, actual, expect, base_idx, limit

    # -- verification --------------------------------------------------------
    def check(self, deep=False):
        """Walk all structures; verify checksums, types and the bitmap."""
        errors = []
        warnings = []
        used = {}

        def mark(blk, owner):
            if blk < self.reserved or blk >= self.total:
                errors.append("%s: block %d out of range" % (owner, blk))
                return False
            if blk in used:
                errors.append(
                    "%s: block %d already used by %s" % (owner, blk, used[blk])
                )
                return False
            used[blk] = owner
            return True

        root = self.read_buf(self.root_blk)
        if not root.checksum_ok():
            errors.append("root block checksum invalid")
        mark(self.root_blk, "root")
        for pi, pblk in enumerate(self.bitmap.page_blks):
            mark(pblk, "bitmap page %d" % pi)
        # bitmap extension blocks
        ext = root.long(-24)
        guard = 0
        while ext:
            guard += 1
            if guard > MAX_CHAIN:
                errors.append("cyclic bitmap extension chain")
                break
            mark(ext, "bitmap ext")
            ext = self.read_buf(ext).long(self.nl - 1)

        n_files = n_dirs = 0
        for prefix, e in self.walk(""):
            path = (prefix + "/" if prefix else "") + e.name_str()
            buf = self.read_buf(e.blk)
            if not buf.checksum_ok():
                errors.append("%s: header checksum invalid" % path)
                continue
            if e.type != T_HEADER:
                errors.append("%s: bad primary type %d" % (path, e.type))
            if self.is_longname and getattr(e, "comment_block", 0):
                cbuf = self.read_buf(e.comment_block)
                if cbuf.long(0) != T_COMMENT or not cbuf.checksum_ok():
                    errors.append("%s: bad comment block %d" % (path, e.comment_block))
                mark(e.comment_block, path + " (comment)")
            if e.sec_type == ST_USERDIR:
                n_dirs += 1
                mark(e.blk, path)
            elif e.sec_type == ST_FILE:
                n_files += 1
                seen_data = 0
                blk = e.blk
                guard = 0
                ok = True
                while blk and ok:
                    guard += 1
                    if guard > MAX_CHAIN:
                        errors.append("%s: cyclic extension chain" % path)
                        break
                    hbuf = self.read_buf(blk)
                    if blk != e.blk and not hbuf.checksum_ok():
                        errors.append("%s: extension block %d checksum invalid" % (path, blk))
                        ok = False
                        break
                    mark(blk, path)
                    count = hbuf.long(2)
                    for k in range(min(count, self.tsz)):
                        ptr = hbuf.long(6 + self.tsz - 1 - k)
                        if ptr == 0:
                            errors.append("%s: zero data pointer" % path)
                            continue
                        if mark(ptr, path):
                            seen_data += 1
                            if deep and not self.ffs:
                                dbuf = self.read_buf(ptr)
                                if dbuf.long(0) != T_DATA or not dbuf.checksum_ok():
                                    errors.append(
                                        "%s: bad OFS data block %d" % (path, ptr)
                                    )
                    blk = hbuf.long(-2)
                data_bytes = self.bs if self.ffs else self.bs - 24
                expect = (e.size + data_bytes - 1) // data_bytes
                if ok and seen_data != expect:
                    errors.append(
                        "%s: %d data blocks, expected %d for %d bytes"
                        % (path, seen_data, expect, e.size)
                    )
            elif e.is_link():
                mark(e.blk, path)
            else:
                errors.append("%s: unknown sec_type %d" % (path, e.sec_type))

        # dircache blocks (DOS\4/5): verify chains and records vs hash tables
        if getattr(self, "dircache", False):
            dirs = [("(root)", self.root_blk)]
            for prefix, e in self.walk(""):
                if e.sec_type == ST_USERDIR:
                    dirs.append(
                        ((prefix + "/" if prefix else "") + e.name_str(), e.blk)
                    )
            for dpath, dblk in dirs:
                dbuf = self.read_buf(dblk)
                records = {}
                blk = dbuf.long(-2)
                guard = 0
                while blk:
                    guard += 1
                    if guard > MAX_CHAIN:
                        errors.append("%s: cyclic dircache chain" % dpath)
                        break
                    cbuf = self.read_buf(blk)
                    if cbuf.long(0) != T_DIRCACHE:
                        errors.append(
                            "%s: dircache block %d has type %d"
                            % (dpath, blk, cbuf.long(0))
                        )
                        break
                    if not cbuf.checksum_ok():
                        errors.append("%s: dircache block %d checksum" % (dpath, blk))
                    if cbuf.long(2) != dblk:
                        errors.append(
                            "%s: dircache block %d parent mismatch" % (dpath, blk)
                        )
                    mark(blk, "dircache of %s" % dpath)
                    off = 24
                    for _ in range(cbuf.long(3)):
                        if off + 24 > self.bs:
                            errors.append("%s: dircache record overflow" % dpath)
                            break
                        hdr = struct.unpack_from(">I", cbuf.data, off)[0]
                        nlen = cbuf.data[off + 23]
                        nm = bytes(cbuf.data[off + 24 : off + 24 + nlen])
                        clen = cbuf.data[off + 24 + nlen]
                        records[hdr] = nm
                        off += 24 + nlen + 1 + clen
                        if off & 1:
                            off += 1
                    blk = cbuf.long(4)
                actual = {
                    e.blk: e.name for e in self._list_entries(dblk, sort=False)
                }
                if records != actual:
                    errors.append(
                        "%s: dircache disagrees with hash table (%d cached, %d real)"
                        % (dpath, len(records), len(actual))
                    )

        # bitmap consistency: compare whole pages at memcmp speed and only
        # drill into individual bits where a page actually differs -- a
        # per-block Python loop takes ~10s on a 10 GB volume
        lost = shared = 0
        for pi, actual, expect, base_idx, limit in self._bitmap_page_diffs(used):
            for off in range(limit):
                w, b = divmod(off, 32)
                byte = w * 4 + (3 - b // 8)
                a = (actual[byte] >> (b % 8)) & 1
                e = (expect[byte] >> (b % 8)) & 1
                if a == e:
                    continue
                blk = self.reserved + base_idx + off
                if e == 0:  # in use by the tree but marked free
                    if shared <= 20:
                        errors.append(
                            "block %d in use by %s but marked free"
                            % (blk, used.get(blk, "?"))
                        )
                    shared += 1
                else:
                    lost += 1
        if shared > 20:
            errors.append("... more bitmap errors suppressed")
        if lost:
            warnings.append("%d allocated blocks not reachable from the tree" % lost)

        return {
            "files": n_files,
            "dirs": n_dirs,
            "used_blocks": len(used),
            "errors": errors,
            "warnings": warnings,
            "ok": not errors,
        }
