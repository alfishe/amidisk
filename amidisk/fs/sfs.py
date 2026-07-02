"""Native SFS reader (SFS\\0 / SFS\\2), read-only.

Structures ported from the AROS SmartFilesystem source
(rom/filesys/SFS/FS/blockstructure.h, objects.h, btreenodes.h) and
cross-checked against the asfs Linux kernel driver.

Layout:
  - root block at block 0 (id 'SFS\\0', structure version 3); a backup
    root block sits at totalblocks-1
  - directories are doubly-linked chains of ObjectContainer blocks
    ('OBJC') holding variable-length fsObject records
  - file data runs are described by fsExtentBNodes in a B-tree of
    'BNDC' blocks rooted at rootblock.extentbnoderoot; each extent is
    (start block, run length, next extent key)
  - dates are seconds since 1978-01-01 (not ticks)
  - RWED protection bits are stored active-high (opposite of AmigaDOS)
"""

import struct
from datetime import timedelta

from .ffs import FSError
from .util import EPOCH, protect_to_str

ROOT_ID = 0x53465300         # 'SFS\0'
OBJC_ID = 0x4F424A43         # 'OBJC'
BNDC_ID = 0x424E4443         # 'BNDC'
HTAB_ID = 0x48544142         # 'HTAB'
SLNK_ID = 0x534C4E4B         # 'SLNK'
ADMC_ID = 0x41444D43         # 'ADMC'
TROK_ID = 0x54524F4B         # 'TROK'
NODC_ID = 0x4E4F4443         # 'NODC'
BTMP_ID = 0x42544D50         # 'BTMP'

STRUCTURE_VERSION = 3

OTYPE_HIDDEN = 1
OTYPE_HARDLINK = 32
OTYPE_LINK = 64
OTYPE_DIR = 128

MAX_CHAIN = 1_000_000

ROOTNODE = 1
RECYCLEDNODE = 2

def _sfs_hash(name, casesensitive=False):
    h = 0
    for c in name:
        if c == ord(b'/'): break
        if not casesensitive:
            if (0x61 <= c <= 0x7A) or (0xE0 <= c <= 0xFE and c != 0xF7):
                c -= 32
        h = (h * 13 + c) & 0xFFFF
    return h


def _sfs_time(secs):
    try:
        return EPOCH + timedelta(seconds=secs)
    except OverflowError:
        return EPOCH


class SFSEntry:
    __slots__ = (
        "name", "bits", "objectnode", "size", "protect", "comment",
        "secs", "data", "hashtable", "firstdirblock", "uid", "gid",
    )

    def is_dir(self):
        return bool(self.bits & OTYPE_DIR)

    def is_file(self):
        return not (self.bits & (OTYPE_DIR | OTYPE_LINK))

    def is_link(self):
        return bool(self.bits & OTYPE_LINK)

    def type_str(self):
        if self.bits & OTYPE_DIR:
            return "dir"
        if self.bits & OTYPE_LINK:
            return "hardlink" if self.bits & OTYPE_HARDLINK else "softlink"
        return "file"

    def name_str(self):
        return self.name.decode("latin-1")

    def mtime(self):
        return _sfs_time(self.secs)

    def protect_str(self):
        # SFS stores RWED active-high; flip to AmigaDOS convention
        return protect_to_str(self.protect ^ 0x0F)

    def get_info(self):
        return {
            "name": self.name_str(),
            "type": self.type_str(),
            "size": self.size if self.is_file() else None,
            "protect": self.protect_str(),
            "comment": self.comment.decode("latin-1"),
            "mtime": self.mtime().isoformat(sep=" ", timespec="seconds"),
            "block": self.objectnode,
        }


class SFSVolume:
    """Read-only SFS volume."""

    def __init__(self, blkdev, sec_per_blk=1, reserved=2, dos_type=None):
        self.dev = blkdev
        self.dos_type = dos_type
        self.read_only = True
        self.label = None
        # block size comes from the root block; start with device blocks
        self.spb = 1
        self.bs = blkdev.block_bytes

    # -- low level -------------------------------------------------------
    def _write_block(self, blocknr, raw):
        if len(raw) != self.bs:
            raise FSError("Block length mismatch")
        self.dev.write(blocknr * self.spb, raw)

    def _fix_checksum(self, raw, id_val, blocknr):
        struct.pack_into(">3I", raw, 0, id_val, 0, blocknr)
        s = 0
        for (v,) in struct.iter_unpack(">I", raw):
            s = (s + v) & 0xFFFFFFFF
        chk = (0xFFFFFFFF - s) & 0xFFFFFFFF
        struct.pack_into(">I", raw, 4, chk)

    def _read_block(self, blocknr, count=1):
        if blocknr < 0 or blocknr + count > self.total:
            raise FSError("SFS block %d out of volume" % blocknr)
        return self.dev.read(blocknr * self.spb, count * self.spb)

    @staticmethod
    def _checksum_ok(raw):
        s = 0
        for (v,) in struct.iter_unpack(">I", raw):
            s = (s + v) & 0xFFFFFFFF
        return s == 0xFFFFFFFF

    def _read_checked(self, blocknr, want_id):
        raw = self._read_block(blocknr)
        bid, _chk, own = struct.unpack_from(">3I", raw, 0)
        if bid != want_id:
            raise FSError(
                "SFS block %d has id 0x%08x, expected 0x%08x" % (blocknr, bid, want_id)
            )
        if not self._checksum_ok(raw):
            raise FSError("SFS block %d checksum invalid" % blocknr)
        if own != blocknr:
            raise FSError("SFS block %d ownblock mismatch (%d)" % (blocknr, own))
        return raw

    # -- mount -------------------------------------------------------------
    def open(self):
        raw = self.dev.read(0)
        bid = struct.unpack_from(">I", raw, 0)[0]
        if bid != ROOT_ID:
            raise FSError("no SFS root block (id 0x%08x)" % bid)
        version = struct.unpack_from(">H", raw, 12)[0]
        if version != STRUCTURE_VERSION:
            raise FSError("unsupported SFS structure version %d" % version)
        (self.datecreated,) = struct.unpack_from(">I", raw, 16)
        self.bits = raw[20]
        (self.totalblocks, self.blocksize) = struct.unpack_from(">2I", raw, 48)
        (
            self.bitmapbase, self.adminspacecontainer,
            self.rootobjectcontainer, self.extentbnoderoot,
            self.objectnoderoot,
        ) = struct.unpack_from(">5I", raw, 96)
        if self.blocksize % self.dev.block_bytes:
            raise FSError("SFS blocksize %d not a multiple of device sector" % self.blocksize)
        self.spb = self.blocksize // self.dev.block_bytes
        self.bs = self.blocksize
        self.total = self.totalblocks
        if self.total * self.spb > self.dev.num_blocks:
            self.total = self.dev.num_blocks // self.spb
        # re-read the root block at full block size and verify
        raw = self._read_checked(0, ROOT_ID)
        self.case_sensitive = bool(self.bits & 128)
        # the volume object is the first object in the root container
        cont = self._read_checked(self.rootobjectcontainer, OBJC_ID)
        self.root_obj = self._parse_object(cont, 24)
        self.label = self.root_obj.name_str()
        # free block count lives in fsRootInfo at the container's tail
        (self.freeblocks,) = struct.unpack_from(">I", cont, self.bs - 36 + 8)
        return self

    def dos_type_str(self):
        from ..rdb.blocks import dos_type_to_str

        return dos_type_to_str(self.dos_type or 0x53465300)

    def get_info(self):
        return {
            "label": self.label,
            "filesystem": "SFS",
            "dos_type": self.dos_type_str(),
            "block_size": self.bs,
            "total_blocks": self.totalblocks,
            "free_blocks": self.freeblocks,
            "free_bytes": self.freeblocks * self.bs,
            "used_blocks": self.totalblocks - self.freeblocks,
            "root_block": 0,
            "created": _sfs_time(self.datecreated).isoformat(
                sep=" ", timespec="seconds"
            ),
            "modified": self.root_obj.mtime().isoformat(sep=" ", timespec="seconds"),
            "read_only": self.read_only,
        }

    def format(self, label, dos_type=None):
        if not isinstance(label, bytes):
            label = label.encode("latin-1")
        if len(label) > 30:
            label = label[:30]
            
        self.dos_type = dos_type or 0x53465300
        self.label = label.decode("latin-1")
        
        import time
        currentdate = int(time.time() - 252460800) # seconds since Jan 1 1978
        
        blocks_admin = 32
        blocks_reserved_start = 2
        blocks_reserved_end = 1
        blocks_total = self.dev.num_blocks // self.spb
        
        bits_per_block = (self.bs - 12) * 8
        blocks_bitmap = (blocks_total + bits_per_block - 1) // bits_per_block
        
        block_adminspace = blocks_reserved_start
        block_root = blocks_reserved_start + 1
        block_extentbnoderoot = block_root + 3
        block_bitmapbase = block_adminspace + blocks_admin
        block_objectnoderoot = block_root + 4
        block_recycled = block_root + 5
        
        # 1. AdminSpaceContainer
        raw_admc = bytearray(self.bs)
        raw_admc[24] = blocks_admin
        struct.pack_into(">2I", raw_admc, 28, block_adminspace, 0xFE000000)
        self._fix_checksum(raw_admc, ADMC_ID, block_adminspace)
        self._write_block(block_adminspace, raw_admc)
        
        # 2. Root ObjectContainer
        raw_objc = bytearray(self.bs)
        struct.pack_into(">HHII", raw_objc, 24, 0, 0, ROOTNODE, 15)
        struct.pack_into(">2I", raw_objc, 36, block_root + 1, block_recycled)
        struct.pack_into(">I", raw_objc, 44, currentdate)
        raw_objc[48] = OTYPE_DIR
        raw_objc[49:49+len(label)] = label
        
        freeblocks = blocks_total - blocks_admin - blocks_reserved_start - blocks_reserved_end - blocks_bitmap
        struct.pack_into(">9I", raw_objc, self.bs - 36, 0, 0, freeblocks, currentdate, 0, 0, 0, 0, 0)
        self._fix_checksum(raw_objc, OBJC_ID, block_root)
        self._write_block(block_root, raw_objc)
        
        # 3. Root HashTable
        raw_htab = bytearray(self.bs)
        struct.pack_into(">I", raw_htab, 12, ROOTNODE)
        h = _sfs_hash(b".recycled", False)
        h_idx = h % ((self.bs - 16) // 4)
        struct.pack_into(">I", raw_htab, 16 + h_idx * 4, RECYCLEDNODE)
        self._fix_checksum(raw_htab, HTAB_ID, block_root + 1)
        self._write_block(block_root + 1, raw_htab)
        
        # 4. Transaction Block
        raw_trok = bytearray(self.bs)
        self._fix_checksum(raw_trok, TROK_ID, block_root + 2)
        self._write_block(block_root + 2, raw_trok)
        
        # 5. ExtentNode B-Tree root
        raw_bndc = bytearray(self.bs)
        struct.pack_into(">HBB", raw_bndc, 12, 0, 1, 14)
        self._fix_checksum(raw_bndc, BNDC_ID, block_extentbnoderoot)
        self._write_block(block_extentbnoderoot, raw_bndc)
        
        # 6. ObjectNode root
        raw_nodc = bytearray(self.bs)
        struct.pack_into(">2I", raw_nodc, 12, 1, 1)
        struct.pack_into(">I", raw_nodc, 20, block_root)
        struct.pack_into(">I", raw_nodc, 28, block_recycled)
        struct.pack_into(">H", raw_nodc, 32, h)
        struct.pack_into(">I", raw_nodc, 36, 0xFFFFFFFF)
        struct.pack_into(">I", raw_nodc, 44, 0xFFFFFFFF)
        struct.pack_into(">I", raw_nodc, 52, 0xFFFFFFFF)
        struct.pack_into(">I", raw_nodc, 60, 0xFFFFFFFF)
        self._fix_checksum(raw_nodc, NODC_ID, block_objectnoderoot)
        self._write_block(block_objectnoderoot, raw_nodc)
        
        # 7. Recycled ObjectContainer
        raw_recy = bytearray(self.bs)
        struct.pack_into(">I", raw_recy, 12, ROOTNODE)
        struct.pack_into(">HHII", raw_recy, 24, 0, 0, RECYCLEDNODE, 3)
        struct.pack_into(">I", raw_recy, 44, currentdate)
        raw_recy[48] = OTYPE_DIR | 2 | 4 | OTYPE_HIDDEN
        recy_name = b".recycled"
        raw_recy[49:49+len(recy_name)] = recy_name
        self._fix_checksum(raw_recy, OBJC_ID, block_recycled)
        self._write_block(block_recycled, raw_recy)
        
        # 8. Bitmap
        startfree = blocks_admin + blocks_bitmap + blocks_reserved_start
        sizefree = blocks_total - startfree - blocks_reserved_end
        
        block = block_bitmapbase
        for _ in range(blocks_bitmap):
            raw_btmp = bytearray(self.bs)
            for cnt2 in range((self.bs - 12) // 4):
                if startfree > 0:
                    startfree -= 32
                    if startfree < 0:
                        val = (1 << (-startfree)) - 1
                        struct.pack_into(">I", raw_btmp, 12 + cnt2 * 4, val)
                        sizefree += startfree
                elif sizefree > 0:
                    sizefree -= 32
                    if sizefree < 0:
                        val = ~((1 << (-sizefree)) - 1) & 0xFFFFFFFF
                        struct.pack_into(">I", raw_btmp, 12 + cnt2 * 4, val)
                    else:
                        struct.pack_into(">I", raw_btmp, 12 + cnt2 * 4, 0xFFFFFFFF)
                else:
                    break
            self._fix_checksum(raw_btmp, BTMP_ID, block)
            self._write_block(block, raw_btmp)
            block += 1
            
        # 9. Root blocks
        raw_root = bytearray(self.bs)
        struct.pack_into(">H", raw_root, 12, STRUCTURE_VERSION)
        struct.pack_into(">I", raw_root, 16, currentdate)
        raw_root[20] = 64 # ROOTBITS_RECYCLED
        lastbyte = blocks_total * self.bs
        struct.pack_into(">4I", raw_root, 32, 0, 0, lastbyte >> 32, lastbyte & 0xFFFFFFFF)
        struct.pack_into(">2I", raw_root, 48, blocks_total, self.bs)
        struct.pack_into(">5I", raw_root, 96, block_bitmapbase, block_adminspace, block_root, block_extentbnoderoot, block_objectnoderoot)
        
        self._fix_checksum(raw_root, ROOT_ID, 0)
        self._write_block(0, raw_root)
        
        self._fix_checksum(raw_root, ROOT_ID, blocks_total - 1)
        self._write_block(blocks_total - 1, raw_root)
        
        self.dev.flush()
        
        # Finally, mount the newly formatted partition
        return self.open()

    # -- objects -------------------------------------------------------------
    def _parse_object(self, raw, off):
        e = SFSEntry()
        (e.uid, e.gid, e.objectnode, e.protect) = struct.unpack_from(">HHII", raw, off)
        a, b = struct.unpack_from(">2I", raw, off + 12)
        (e.secs,) = struct.unpack_from(">I", raw, off + 20)
        e.bits = raw[off + 24]
        if e.bits & OTYPE_DIR:
            e.hashtable, e.firstdirblock = a, b
            e.data, e.size = 0, 0
        else:
            e.data, e.size = a, b
            e.hashtable = e.firstdirblock = 0
        # name and comment: two NUL-terminated strings from off+25
        p = off + 25
        end = raw.index(b"\x00", p)
        e.name = bytes(raw[p:end])
        p = end + 1
        end = raw.index(b"\x00", p)
        e.comment = bytes(raw[p:end])
        return e

    def _iter_container_objects(self, raw):
        """Yield (offset, SFSEntry) for every object in an OBJC block."""
        off = 24
        guard = 0
        while off + 27 < self.bs:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("runaway object container scan")
            objectnode = struct.unpack_from(">I", raw, off + 4)[0]
            if objectnode == 0:
                break
            e = self._parse_object(raw, off)
            yield e
            # advance: fixed part + name + NUL + comment + NUL, pad to even
            off = off + 25 + len(e.name) + 1 + len(e.comment) + 1
            if off & 1:
                off += 1

    def _dir_entries(self, entry):
        """All objects in a directory (entry = SFSEntry with dir bits)."""
        entries = []
        block = entry.firstdirblock
        guard = 0
        while block:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic SFS dir container chain")
            raw = self._read_checked(block, OBJC_ID)
            entries.extend(e for e in self._iter_container_objects(raw))
            block = struct.unpack_from(">I", raw, 16)[0]  # be_next
        return entries

    # -- public API ------------------------------------------------------------
    def root_entry(self):
        return self.root_obj

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
        if self.case_sensitive:
            return a == b
        return len(a) == len(b) and all(
            self._upper(x) == self._upper(y) for x, y in zip(a, b)
        )

    def resolve(self, path):
        parts = self._split(path)
        cur = self.root_obj
        for seg in parts:
            if not cur.is_dir():
                raise FSError("'%s' is not a directory" % cur.name_str())
            found = None
            for e in self._dir_entries(cur):
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
        entries = self._dir_entries(e)
        entries.sort(key=lambda x: x.name.lower())
        return entries

    def walk(self, path=""):
        start = self.resolve(path)
        if not start.is_dir():
            raise FSError("not a directory")
        base = "/".join(p.decode("latin-1") for p in self._split(path))
        stack = [(base, start)]
        while stack:
            prefix, d = stack.pop()
            for e in self._dir_entries(d):
                yield prefix, e
                if e.is_dir():
                    sub = (prefix + "/" if prefix else "") + e.name_str()
                    stack.append((sub, e))

    # -- extents / file data ----------------------------------------------------
    def _find_extent(self, key):
        """Look up the extent whose key == block number `key`."""
        blocknr = self.extentbnoderoot
        guard = 0
        while True:
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("SFS extent btree too deep")
            raw = self._read_checked(blocknr, BNDC_ID)
            nodecount, isleaf, nodesize = struct.unpack_from(">HBB", raw, 12)
            if nodecount == 0:
                raise FSError("empty SFS extent btree node")
            # greatest node with node.key <= key (scan from the end)
            base = 16
            chosen = None
            for n in range(nodecount - 1, -1, -1):
                noff = base + n * nodesize
                nkey = struct.unpack_from(">I", raw, noff)[0]
                if n == 0 or key >= nkey:
                    chosen = noff
                    break
            if isleaf:
                nkey, nxt, prev, blocks = struct.unpack_from(">IIIH", raw, chosen)
                if nkey != key:
                    raise FSError("SFS extent for block %d not found" % key)
                return nxt, blocks
            blocknr = struct.unpack_from(">I", raw, chosen + 4)[0]

    def read_file(self, path):
        e = path if isinstance(path, SFSEntry) else self.resolve(path)
        if e.is_link():
            raise FSError("link: open the link target instead")
        if not e.is_file():
            raise FSError("not a file: %s" % e.name_str())
        return self._read_data(e)

    def read_file_bytes(self, path):
        return b"".join(self.read_file(path))

    def _read_data(self, entry):
        remaining = entry.size
        block = entry.data
        guard = 0
        while remaining > 0:
            if not block:
                raise FSError(
                    "truncated SFS file %s (%d bytes missing)"
                    % (entry.name_str(), remaining)
                )
            guard += 1
            if guard > MAX_CHAIN:
                raise FSError("cyclic SFS extent chain")
            nxt, blocks = self._find_extent(block)
            todo = blocks
            cur = block
            while todo > 0 and remaining > 0:
                n = min(todo, 512)
                data = self._read_block(cur, n)
                if remaining < len(data):
                    data = data[:remaining]
                yield data
                remaining -= len(data)
                cur += n
                todo -= n
            block = nxt

    def read_softlink(self, entry):
        raw = self._read_checked(entry.data, SLNK_ID)
        end = raw.index(b"\x00", 24)
        return bytes(raw[24:end]).decode("latin-1")

    # -- verification ---------------------------------------------------------
    def check(self, deep=False):
        errors = []
        warnings = []
        n_files = n_dirs = 0
        seen_nodes = set()
        try:
            for prefix, e in self.walk(""):
                path = (prefix + "/" if prefix else "") + e.name_str()
                if e.objectnode in seen_nodes:
                    errors.append("%s: objectnode %d reused" % (path, e.objectnode))
                seen_nodes.add(e.objectnode)
                if e.is_dir():
                    n_dirs += 1
                elif e.is_file():
                    n_files += 1
                    try:
                        total = 0
                        block = e.data
                        guard = 0
                        while block and total < e.size:
                            guard += 1
                            if guard > MAX_CHAIN:
                                errors.append("%s: cyclic extents" % path)
                                break
                            nxt, blocks = self._find_extent(block)
                            if block + blocks > self.total:
                                errors.append("%s: extent out of volume" % path)
                                break
                            total += blocks * self.bs
                            block = nxt
                        if total < e.size:
                            errors.append(
                                "%s: extents hold %d bytes < size %d"
                                % (path, total, e.size)
                            )
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
