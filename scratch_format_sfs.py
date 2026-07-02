import struct
import time
from datetime import timedelta
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

from amidisk.blkdev import ImageFileBlkDev
from amidisk.fs.sfs import SFSVolume

ADMINSPACECONTAINER_ID = 0x41444D43 # 'ADMC'
OBJECTCONTAINER_ID = 0x4F424A43     # 'OBJC'
HASHTABLE_ID = 0x48544142           # 'HTAB'
TRANSACTIONOK_ID = 0x54524F4B       # 'TROK'
BNODECONTAINER_ID = 0x424E4443      # 'BNDC'
NODECONTAINER_ID = 0x4E4F4443       # 'NODC'
BITMAP_ID = 0x42544D50              # 'BTMP'
ROOT_ID = 0x53465300                # 'SFS\0'

ROOTNODE = 1
RECYCLEDNODE = 2

def sfs_hash(name, casesensitive=False):
    h = 0
    for c in name:
        if c == ord('/'): break
        if not casesensitive:
            if (0x61 <= c <= 0x7A) or (0xE0 <= c <= 0xFE and c != 0xF7):
                c -= 32
        h = (h * 13 + c) & 0xFFFF
    return h

def _write_block(vol, blocknr, raw):
    if len(raw) != vol.bs:
        raise Exception("Block length mismatch")
    vol.dev.write(blocknr * vol.spb, raw)

def _fix_checksum(raw, id_val, blocknr):
    struct.pack_into(">3I", raw, 0, id_val, 0, blocknr)
    s = 0
    for (v,) in struct.iter_unpack(">I", raw):
        s = (s + v) & 0xFFFFFFFF
    chk = (0xFFFFFFFF - s) & 0xFFFFFFFF
    struct.pack_into(">I", raw, 4, chk)

def format_sfs(vol, label, dos_type=None):
    if not isinstance(label, bytes):
        label = label.encode("latin-1")
    if len(label) > 30:
        label = label[:30]
        
    vol.dos_type = dos_type or 0x53465300
    vol.label = label.decode("latin-1")
    
    currentdate = int(time.time() - 252460800) # seconds since Jan 1 1978
    
    blocks_admin = 32
    blocks_reserved_start = 2
    blocks_reserved_end = 1
    blocks_total = vol.dev.num_blocks // vol.spb
    
    # blocks_bitmap is number of blocks for bitmap
    bits_per_block = (vol.bs - 12) * 8
    blocks_bitmap = (blocks_total + bits_per_block - 1) // bits_per_block
    
    block_adminspace = blocks_reserved_start
    block_root = blocks_reserved_start + 1
    block_extentbnoderoot = block_root + 3
    block_bitmapbase = block_adminspace + blocks_admin
    block_objectnoderoot = block_root + 4
    block_recycled = block_root + 5
    
    # 1. AdminSpaceContainer
    raw_admc = bytearray(vol.bs)
    struct.pack_into(">I", raw_admc, 16, 0) # next
    struct.pack_into(">I", raw_admc, 20, 0) # previous
    raw_admc[24] = blocks_admin
    struct.pack_into(">2I", raw_admc, 28, block_adminspace, 0xFE000000)
    _fix_checksum(raw_admc, ADMINSPACECONTAINER_ID, block_adminspace)
    _write_block(vol, block_adminspace, raw_admc)
    
    # 2. Root ObjectContainer
    raw_objc = bytearray(vol.bs)
    struct.pack_into(">HHII", raw_objc, 24, 0, 0, ROOTNODE, 15) # protect
    struct.pack_into(">2I", raw_objc, 36, block_root + 1, block_recycled)
    struct.pack_into(">I", raw_objc, 44, currentdate)
    raw_objc[48] = 128 # OTYPE_DIR
    raw_objc[49:49+len(label)] = label
    
    freeblocks = blocks_total - blocks_admin - blocks_reserved_start - blocks_reserved_end - blocks_bitmap
    ri_off = vol.bs - 36
    struct.pack_into(">9I", raw_objc, ri_off, 0, 0, freeblocks, currentdate, 0, 0, 0, 0, 0)
    _fix_checksum(raw_objc, OBJECTCONTAINER_ID, block_root)
    _write_block(vol, block_root, raw_objc)
    
    # 3. Root HashTable
    raw_htab = bytearray(vol.bs)
    struct.pack_into(">I", raw_htab, 12, ROOTNODE)
    h = sfs_hash(b".recycled", False)
    htab_size = (vol.bs - 16) // 4
    h_idx = h % htab_size
    struct.pack_into(">I", raw_htab, 16 + h_idx * 4, RECYCLEDNODE)
    _fix_checksum(raw_htab, HASHTABLE_ID, block_root + 1)
    _write_block(vol, block_root + 1, raw_htab)
    
    # 4. Transaction Block
    raw_trok = bytearray(vol.bs)
    _fix_checksum(raw_trok, TRANSACTIONOK_ID, block_root + 2)
    _write_block(vol, block_root + 2, raw_trok)
    
    # 5. ExtentNode B-Tree root
    raw_bndc = bytearray(vol.bs)
    struct.pack_into(">HBB", raw_bndc, 12, 0, 1, 14) # nodecount=0, isleaf=1, nodesize=14
    _fix_checksum(raw_bndc, BNODECONTAINER_ID, block_extentbnoderoot)
    _write_block(vol, block_extentbnoderoot, raw_bndc)
    
    # 6. ObjectNode root
    raw_nodc = bytearray(vol.bs)
    struct.pack_into(">2I", raw_nodc, 12, 1, 1) # nodenumber=1, nodes=1
    struct.pack_into(">I", raw_nodc, 20, block_root)
    struct.pack_into(">I", raw_nodc, 28, block_recycled)
    struct.pack_into(">H", raw_nodc, 32, h)
    struct.pack_into(">I", raw_nodc, 36, 0xFFFFFFFF)
    struct.pack_into(">I", raw_nodc, 44, 0xFFFFFFFF)
    struct.pack_into(">I", raw_nodc, 52, 0xFFFFFFFF)
    struct.pack_into(">I", raw_nodc, 60, 0xFFFFFFFF)
    _fix_checksum(raw_nodc, NODECONTAINER_ID, block_objectnoderoot)
    _write_block(vol, block_objectnoderoot, raw_nodc)
    
    # 7. Recycled ObjectContainer
    raw_recy = bytearray(vol.bs)
    struct.pack_into(">I", raw_recy, 12, ROOTNODE)
    struct.pack_into(">HHII", raw_recy, 24, 0, 0, RECYCLEDNODE, 3) # read|write
    struct.pack_into(">2I", raw_recy, 36, 0, 0)
    struct.pack_into(">I", raw_recy, 44, currentdate)
    raw_recy[48] = 128 | 2 | 4 | 1 # OTYPE_DIR | undeletable | quickdir | hidden
    recy_name = b".recycled"
    raw_recy[49:49+len(recy_name)] = recy_name
    _fix_checksum(raw_recy, OBJECTCONTAINER_ID, block_recycled)
    _write_block(vol, block_recycled, raw_recy)
    
    # 8. Bitmap
    startfree = blocks_admin + blocks_bitmap + blocks_reserved_start
    sizefree = blocks_total - startfree - blocks_reserved_end
    
    block = block_bitmapbase
    cnt = blocks_bitmap
    while cnt > 0:
        raw_btmp = bytearray(vol.bs)
        for cnt2 in range((vol.bs - 12) // 4):
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
        _fix_checksum(raw_btmp, BITMAP_ID, block)
        _write_block(vol, block, raw_btmp)
        block += 1
        cnt -= 1
        
    # 9. Root blocks
    raw_root = bytearray(vol.bs)
    struct.pack_into(">H", raw_root, 12, 3) # version
    struct.pack_into(">H", raw_root, 14, 0) # seq
    struct.pack_into(">I", raw_root, 16, currentdate)
    raw_root[20] = 64 # ROOTBITS_RECYCLED
    firstbyte = 0
    lastbyte = blocks_total * vol.bs
    struct.pack_into(">4I", raw_root, 32, firstbyte >> 32, firstbyte & 0xFFFFFFFF, lastbyte >> 32, lastbyte & 0xFFFFFFFF)
    struct.pack_into(">2I", raw_root, 48, blocks_total, vol.bs)
    struct.pack_into(">5I", raw_root, 96, block_bitmapbase, block_adminspace, block_root, block_extentbnoderoot, block_objectnoderoot)
    
    _fix_checksum(raw_root, ROOT_ID, 0)
    _write_block(vol, 0, raw_root)
    
    _fix_checksum(raw_root, ROOT_ID, blocks_total - 1)
    _write_block(vol, blocks_total - 1, raw_root)
    
    vol.dev.flush()

if __name__ == "__main__":
    with open("sfs_format_test.hdf", "wb") as fh:
        fh.truncate(10 * 1024 * 1024)
    dev = ImageFileBlkDev("sfs_format_test.hdf", read_only=False)
    vol = SFSVolume(dev)
    vol.read_only = False
    
    format_sfs(vol, "TestSFS")
    dev.close()
    
    dev2 = ImageFileBlkDev("sfs_format_test.hdf", read_only=True)
    vol2 = SFSVolume(dev2).open()
    print("Mounted formatted SFS!")
    print(vol2.get_info())
    print("Files in root:")
    for e in vol2.list_dir(""):
        print(e.get_info())
    dev2.close()
