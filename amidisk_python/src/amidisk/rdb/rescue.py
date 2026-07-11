"""RDB rescue: analyze a disk whose RDB was overwritten and rebuild it.

Scenario: the RDSK block (and possibly the PART chain) at the start of
the disk got trashed -- by a PC partitioner, an accidental dd, a format
of the wrong drive -- but the partition *contents* (the filesystems)
are still on disk. This module finds them again:

  evidence, best first:
  1. orphaned PART blocks in the old RDB area -- exact geometry
  2. SFS root blocks   -- live at partition start, carry exact size
  3. PFS3 root blocks  -- live at partition start + 2, carry exact size
  4. FFS boot + root block pairs -- root sits at the middle of the
     partition, so its distance from the boot block gives the size

The rebuilder then writes a fresh RDSK (reusing intact PART blocks when
found, synthesizing them otherwise), after backing up the old RDB area
to a file.
"""

import struct

from .blocks import PartitionBlock, dos_type_to_str
from .rdisk import RDisk, RDiskError, END

CHUNK_SECS = 8192  # 4 MB scan chunks


class Candidate:
    """One recovered partition candidate (all units: 512-byte sectors)."""

    def __init__(self, source, start, num_secs, dos_type, label=None,
                 sec_per_blk=1, part_blk=None, exact=True):
        self.source = source          # 'part' | 'sfs' | 'pfs3' | 'ffs'
        self.start = start
        self.num_secs = num_secs      # may be approximate for ffs
        self.dos_type = dos_type
        self.label = label
        self.sec_per_blk = sec_per_blk
        self.part_blk = part_blk      # orphaned PartitionBlock, if any
        self.exact = exact

    def end(self):
        return self.start + self.num_secs

    def describe(self):
        return "%-5s @sector %-9d %8.1f MB  %-7s %s%s" % (
            self.source, self.start, self.num_secs * 512 / 1e6,
            dos_type_to_str(self.dos_type),
            repr(self.label) if self.label else "-",
            "" if self.exact else "  (size estimated)",
        )


def _ffs_root_valid(raw, bs):
    """Is `raw[:bs]` a valid FFS root block?"""
    if len(raw) < bs:
        return False
    nl = bs // 4
    if struct.unpack_from(">I", raw, 0)[0] != 2:
        return False
    if struct.unpack_from(">i", raw, bs - 4)[0] != 1:
        return False
    s = 0
    for (v,) in struct.iter_unpack(">I", raw[:bs]):
        s = (s + v) & 0xFFFFFFFF
    return s == 0


def _sfs_checksum_ok(raw):
    s = 0
    for (v,) in struct.iter_unpack(">I", raw):
        s = (s + v) & 0xFFFFFFFF
    return s == 0xFFFFFFFF


def scan(blkdev, progress=None, rdb_area_secs=None):
    """Scan the device for partition evidence. Returns (candidates, notes)."""
    total = blkdev.num_blocks
    if rdb_area_secs is None:
        rdb_area_secs = min(total, 4096)
    notes = []
    parts = []
    anchors = []
    boots = {}  # sector -> dostype (FFS boot block candidates)

    roots = []  # (sector, spb, label) of validated FFS root blocks

    def find_aligned(chunk, needle, handler):
        i = chunk.find(needle)
        while i != -1:
            if i % 512 == 0:
                handler(i)
            i = chunk.find(needle, i + 1)

    # prefilter for an FFS root block: type=2, header_key=0, high_seq=0
    ROOT_PAT = b"\x00\x00\x00\x02" + b"\x00" * 8

    pos = 0
    while pos < total:
        n = min(CHUNK_SECS, total - pos)
        chunk = blkdev.read(pos, n)

        def on_part(off, pos=pos):
            sec = pos + off // 512
            if sec < rdb_area_secs:
                pb = PartitionBlock(blkdev, sec)
                if pb.read():
                    parts.append(pb)

        def on_dos(off, chunk=chunk, pos=pos):
            flavor = chunk[off + 3]
            if flavor <= 7:
                boots[pos + off // 512] = 0x444F5300 + flavor

        def on_sfs(off, chunk=chunk, pos=pos):
            raw = chunk[off : off + 512]
            if len(raw) < 512:
                return
            version = struct.unpack_from(">H", raw, 12)[0]
            ownblock = struct.unpack_from(">I", raw, 8)[0]
            if version == 3 and ownblock == 0:
                totalblocks, blocksize = struct.unpack_from(">2I", raw, 48)
                if 512 <= blocksize <= 32768 and totalblocks > 0:
                    spb = blocksize // 512
                    anchors.append(
                        Candidate(
                            "sfs", pos + off // 512, totalblocks * spb,
                            0x53465300, sec_per_blk=spb,
                        )
                    )

        def on_pfs(off, chunk=chunk, pos=pos):
            raw = chunk[off : off + 512]
            if len(raw) < 512:
                return
            disksize = struct.unpack_from(">I", raw, 84)[0]
            options = struct.unpack_from(">I", raw, 4)[0]
            sec = pos + off // 512
            if disksize and (options & 1) and sec >= 2:  # MODE_HARDDISK
                nlen = raw[20]
                label = raw[21 : 21 + min(nlen, 31)].decode("latin-1", "replace")
                anchors.append(
                    Candidate("pfs3", sec - 2, disksize, 0x50465303, label=label)
                )

        def on_root(off, pos=pos):
            sec = pos + off // 512
            for spb in (1, 2, 4, 8):
                if sec + spb > total:
                    break
                raw = blkdev.read(sec, spb)
                if _ffs_root_valid(raw, 512 * spb):
                    bs = 512 * spb
                    nlen = raw[bs - 80]
                    label = raw[bs - 79 : bs - 79 + min(nlen, 30)].decode(
                        "latin-1", "replace"
                    )
                    roots.append((sec, spb, label))
                    break

        find_aligned(chunk, b"PART", on_part)
        find_aligned(chunk, b"DOS", on_dos)
        find_aligned(chunk, b"SFS\x00", on_sfs)
        find_aligned(chunk, b"PFS\x01", on_pfs)
        find_aligned(chunk, b"AFS\x01", on_pfs)
        find_aligned(chunk, ROOT_PAT, on_root)
        pos += n
        if progress:
            progress(pos, total)

    # pair FFS boot blocks with their root blocks: the root block sits at
    # fs-block (2 + total - 1) // 2, so total ~= 2 * (root - boot) / spb
    ffs = []
    boot_list = sorted(boots)
    for i, bsec in enumerate(boot_list):
        flavor = boots[bsec]
        nxt = boot_list[i + 1] if i + 1 < len(boot_list) else total
        for rsec, spb, label in roots:
            if not (bsec < rsec < nxt):
                continue
            root_idx = (rsec - bsec) // spb
            if root_idx < 2 or (rsec - bsec) % spb:
                continue
            # (2 + tot - 1) // 2 == root_idx admits tot and tot+1; use the
            # smaller -- the rebuilder rounds the end up to the cylinder
            # boundary, which recovers the exact original size, while the
            # larger estimate would spill one block into the next cylinder
            tot = 2 * root_idx - 1
            ffs.append(
                Candidate(
                    "ffs", bsec, tot * spb, flavor,
                    label=label, sec_per_blk=spb, exact=False,
                )
            )
            break

    # orphaned PART blocks win over anchors covering the same region
    cands = []
    for pb in parts:
        env = pb.dos_env
        cands.append(
            Candidate(
                "part", env.start_sec(), env.num_secs(), env.dos_type,
                label=pb.drv_name, sec_per_blk=max(env.sec_per_blk, 1),
                part_blk=pb,
            )
        )
    for a in anchors + ffs:
        if not any(c.start <= a.start < c.end() for c in cands):
            cands.append(a)
    cands.sort(key=lambda c: c.start)

    # drop overlapping duplicates (keep the earlier, more exact one)
    pruned = []
    for c in cands:
        if pruned and c.start < pruned[-1].end():
            prev = pruned[-1]
            if c.exact and not prev.exact:
                pruned[-1] = c
            else:
                notes.append(
                    "dropped overlapping candidate: %s" % c.describe()
                )
            continue
        pruned.append(c)
    if not pruned:
        notes.append("no partition evidence found")
    return pruned, notes


def _pick_geometry(candidates, total_secs):
    """Choose heads/sectors such that all candidate starts are
    cylinder-aligned and the cylinder count fits in 16 bits."""
    starts = [c.start for c in candidates]
    options = []
    for heads in (16, 8, 4, 2, 1, 32, 64, 128, 255):
        for sectors in (63, 32, 127, 126, 16):
            cyl = heads * sectors
            if total_secs // cyl > 65535 or total_secs // cyl < 3:
                continue
            if all(s % cyl == 0 for s in starts):
                options.append((heads, sectors))
    if options:
        return options[0]
    # last resort: 1 sector per cylinder aligns with anything
    return 1, 1


def rebuild(blkdev, candidates, backup_path=None, drv_prefix="DH"):
    """Write a fresh RDB describing `candidates`. Returns the new RDisk."""
    if blkdev.read_only:
        raise RDiskError("device is read-only")
    if not candidates:
        raise RDiskError("nothing to rebuild")

    if backup_path:
        area = min(blkdev.num_blocks, 2048)
        with open(backup_path, "wb") as fh:
            fh.write(blkdev.read(0, area))

    reuse = all(c.part_blk is not None for c in candidates)
    if reuse:
        env = candidates[0].part_blk.dos_env
        heads, sectors = env.surfaces, env.blk_per_trk
    else:
        heads, sectors = _pick_geometry(candidates, blkdev.num_blocks)

    first_start = min(c.start for c in candidates)
    cyl = heads * sectors
    rdb_cyls = max(1, first_start // cyl)
    rdb_cyls = min(rdb_cyls, 2) if first_start >= cyl else 1
    rd = RDisk.create(blkdev, sectors=sectors, heads=heads, rdb_cyls=rdb_cyls)

    if reuse:
        # relink the surviving PART blocks under the fresh RDSK
        ordered = sorted(candidates, key=lambda c: c.start)
        for i, c in enumerate(ordered):
            pb = c.part_blk
            pb.next = ordered[i + 1].part_blk.blk_num if i + 1 < len(ordered) else END
            pb.write()
            rd.rdb.high_rdsk_block = max(rd.rdb.high_rdsk_block, pb.blk_num)
        rd.rdb.partition_list = ordered[0].part_blk.blk_num
        rd._write_rdb()
        rd.open()
        return rd

    ordered = sorted(candidates, key=lambda c: c.start)
    for i, c in enumerate(ordered):
        low = c.start // cyl
        high = (c.end() + cyl - 1) // cyl - 1
        high = min(high, rd.rdb.hi_cylinder)
        if i + 1 < len(ordered):  # never spill into the next partition
            high = min(high, ordered[i + 1].start // cyl - 1)
        name = c.label if (c.label and c.source == "part") else "%s%d" % (drv_prefix, i)
        rd.add_partition(
            name,
            num_cyls=None,
            size_bytes=None,
            low_cyl=low,
            high_cyl=high,
            dos_type=c.dos_type,
            sec_per_blk=c.sec_per_blk,
        )
    return rd
