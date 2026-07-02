"""DiskImage -- top-level facade over container + partition table + volumes.

Layout detection (see architecture doc section 5.1):
  1. RDSK magic within the first 16 blocks  -> RDB disk (HDF / VHD)
  2. ADF-sized image                        -> floppy volume
  3. DOS bootblock / valid mid-disk root    -> RDB-less single-volume HDF
"""

import os
import struct

from .blkdev import open_blkdev, PartBlockDevice, BlockDeviceError
from .rdb import RDisk
from .fs.ffs import FFSVolume, FSError
from .fs.pfs3 import PFS3Volume

ADF_SIZES = (901120, 1802240)  # DD, HD floppies


def engine_for_dostype(dos_type):
    """Pick the filesystem engine class for a dostype."""
    if dos_type is None:
        return FFSVolume
    tag3 = dos_type >> 8
    if tag3 in (0x504653, 0x504453):        # PFS\x, PDS\x
        return PFS3Volume
    if dos_type in (0x6D754146, 0x6D755046, 0x41465301):  # muAF, muPF, AFS\1
        return PFS3Volume
    if tag3 == 0x534653:                    # SFS\x
        from .fs.sfs import SFSVolume

        return SFSVolume
    return FFSVolume                        # DOS\x and default


class ImageError(Exception):
    pass


class VolumeRef:
    """One mountable volume inside an image."""

    def __init__(self, image, name, partition=None):
        self.image = image
        self.name = name          # drive name (DH0) or synthetic (DF0)
        self.partition = partition
        self._vol = None

    def raw_volume(self, engine=None):
        """Unmounted volume object (for format)."""
        if self.partition is not None:
            env = self.partition.dos_env
            cls = engine or engine_for_dostype(env.dos_type)
            return cls(
                self.partition.create_blkdev(),
                sec_per_blk=max(env.sec_per_blk, 1),
                reserved=env.reserved,
                dos_type=env.dos_type,
            )
        # bare volume: sniff the bootblock to pick an engine
        cls = engine
        if cls is None:
            try:
                boot = self.image.blkdev.read(0)
                import struct as _s

                cls = engine_for_dostype(_s.unpack(">I", boot[0:4])[0])
            except Exception:
                cls = FFSVolume
        return cls(self.image.blkdev)

    def mount(self):
        if self._vol is None:
            self._vol = self.raw_volume().open()
        return self._vol

    def identify(self):
        """Best-effort identification when mount() is not possible."""
        if self.partition is not None:
            return identify_volume(
                self.partition.create_blkdev(), self.partition.dos_env.dos_type
            )
        return identify_volume(self.image.blkdev, None)

    def label(self):
        try:
            return self.mount().label
        except FSError:
            return None

    def dos_type_str(self):
        if self.partition is not None:
            return self.partition.get_dos_type_str()
        try:
            return self.mount().dos_type_str()
        except FSError:
            return "?"


class DiskImage:
    def __init__(self, path, writable=False, blkdev=None):
        self.path = path
        self.writable = writable
        self.blkdev = blkdev or open_blkdev(path, read_only=not writable)
        self.rdisk = None
        self.kind = None
        self.volumes = []
        self._detect()

    def _detect(self):
        rd = RDisk(self.blkdev)
        if rd.open():
            self.rdisk = rd
            self.kind = (
                "vhd+rdb" if type(self.blkdev).__name__ == "VHDBlkDev" else "hdf+rdb"
            )
            for part in rd.get_partitions():
                self.volumes.append(VolumeRef(self, part.drv_name, part))
            return
        # hybrid: MBR-wrapped RDB (PiStorm / Emu68 SD cards)
        rd = self._probe_mbr_rdb()
        if rd is not None:
            self.rdisk = rd
            self.kind = "mbr+rdb"
            for part in rd.get_partitions():
                self.volumes.append(VolumeRef(self, part.drv_name, part))
            return
        size = self.blkdev.size_bytes()
        if size in ADF_SIZES:
            self.kind = "adf"
            self.volumes.append(VolumeRef(self, "DF0"))
            return
        # RDB-less hardfile: single volume spanning the whole image
        try:
            probe = VolumeRef(self, "DH0")
            probe.mount()
            self.kind = "hdf-bare"
            self.volumes.append(probe)
            return
        except (FSError, BlockDeviceError):
            pass
        # not recognized: keep it addressable so `format` can claim it
        self.kind = "blank"
        self.volumes.append(VolumeRef(self, "DH0"))

    def _probe_mbr_rdb(self):
        """Find an RDB nested inside an MBR partition (type 0x76 etc.)."""
        try:
            mbr = self.blkdev.read(0)
        except Exception:
            return None
        if struct.unpack_from("<H", mbr, 510)[0] != 0xAA55:
            return None
        for i in range(4):
            off = 446 + i * 16
            ptype = mbr[off + 4]
            start, size = struct.unpack_from("<II", mbr, off + 8)
            if not ptype or not size or start + size > self.blkdev.num_blocks:
                continue
            sub = PartBlockDevice(self.blkdev, start, size)
            if RDisk.peek(sub) is not None:
                rd = RDisk(sub)
                if rd.open():
                    return rd
        return None

    def get_volume(self, spec):
        """Find a volume by drive name (DH0), label (System) or index."""
        if spec is None or spec == "":
            if len(self.volumes) == 1:
                return self.volumes[0]
            raise ImageError(
                "image has %d volumes, specify one of: %s"
                % (len(self.volumes), ", ".join(v.name for v in self.volumes))
            )
        lo = spec.lower()
        for v in self.volumes:
            if v.name.lower() == lo:
                return v
        if lo.isdigit():
            idx = int(lo)
            if 0 <= idx < len(self.volumes):
                return self.volumes[idx]
        for v in self.volumes:
            lbl = v.label()
            if lbl and lbl.lower() == lo:
                return v
        raise ImageError("no volume %r in image (have: %s)"
                         % (spec, ", ".join(v.name for v in self.volumes)))

    def parse_path(self, spec):
        """'DH0:Some/Path' -> (VolumeRef, 'Some/Path'). No colon = sole volume."""
        if ":" in spec:
            volname, _, path = spec.partition(":")
        else:
            volname, path = "", spec
        return self.get_volume(volname), path

    def get_info(self):
        info = {
            "path": self.path,
            "kind": self.kind,
            "size_bytes": self.blkdev.size_bytes(),
            "writable": self.writable,
        }
        if self.rdisk:
            info["rdb"] = self.rdisk.get_info()
            info["partitions"] = [p.get_info() for p in self.rdisk.get_partitions()]
            info["filesystems"] = [f.get_info() for f in self.rdisk.get_filesystems()]
        # one authoritative per-volume section: mounted volume info when an
        # engine can serve it, best-effort identification otherwise
        vols = []
        for v in self.volumes:
            entry = {"name": v.name}
            try:
                entry["volume"] = v.mount().get_info()
            except Exception as ex:
                entry["error"] = str(ex)
                try:
                    entry["identify"] = v.identify()
                except Exception:
                    pass
            vols.append(entry)
        info["volumes"] = vols
        return info

    def close(self):
        self.blkdev.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def identify_volume(dev, dos_type=None):
    """Best-effort identification of a volume we cannot mount.

    Returns a dict with the declared dostype, the first block's magic and
    a list of human-readable guesses based on known on-disk signatures,
    so `info` can say WHAT lives in a partition even when no engine can
    serve it (CDFS, muFS, FAT, ext2, NDOS, foreign data...).
    """
    from .rdb.blocks import dos_type_to_str

    out = {
        "declared_dos_type": dos_type_to_str(dos_type) if dos_type else None,
        "size_bytes": dev.num_blocks * dev.block_bytes,
        "boot_magic": None,
        "guesses": [],
    }
    guesses = out["guesses"]
    try:
        b0 = dev.read(0)
    except Exception as ex:
        guesses.append("unreadable first block: %s" % ex)
        return out
    magic = b0[0:4]
    out["boot_magic"] = "".join(chr(c) if 32 <= c < 127 else "." for c in magic)

    if magic[:3] == b"DOS" and magic[3] <= 7:
        guesses.append("AmigaDOS bootblock (DOS\\%d)" % magic[3])
    elif magic == b"NDOS":
        guesses.append("NDOS marker: intentionally not a DOS disk")
    elif magic[:3] == b"DOS":
        guesses.append("AmigaDOS-style bootblock with unknown flavor %d" % magic[3])
    if magic == b"SFS\x00" or magic == b"SFS\x02":
        version = struct.unpack_from(">H", b0, 12)[0]
        guesses.append("SFS root block, structure version %d" % version)
    if magic == b"CDSF" or magic == b"CD01":
        guesses.append("possible CD filesystem marker")
    if magic in (b"muFS", b"muAF", b"muPF"):
        guesses.append("multiuser filesystem (muFS family) bootblock")
    try:
        if dev.num_blocks > 2:
            r2 = dev.read(2)
            if r2[0:4] in (b"PFS\x01", b"PFS\x02", b"AFS\x01"):
                guesses.append("PFS rootblock at block 2 (%r)" % r2[0:4])
    except Exception:
        pass
    # PC-world signatures
    if len(b0) >= 512 and b0[510:512] == b"\x55\xaa":
        if b0[54:59] == b"FAT12" or b0[54:59] == b"FAT16":
            guesses.append("FAT12/16 boot sector")
        elif b0[82:87] == b"FAT32":
            guesses.append("FAT32 boot sector")
        else:
            guesses.append("PC boot sector / MBR signature (0x55AA)")
    try:
        if dev.num_blocks * dev.block_bytes > 1084:
            sb = dev.read(2)  # bytes 1024.. hold the ext superblock
            if sb[56:58] == b"\x53\xef":
                guesses.append("Linux ext2/3/4 superblock")
    except Exception:
        pass
    # AmigaDOS structure without a valid bootblock: look for a root block
    try:
        total = dev.num_blocks
        for spb in (1, 2, 4, 8):
            tot = total // spb
            root = (2 + tot - 1) // 2
            raw = dev.read(root * spb, spb)
            bs = 512 * spb
            if (struct.unpack_from(">I", raw, 0)[0] == 2
                    and struct.unpack_from(">i", raw, bs - 4)[0] == 1):
                s = 0
                for (v,) in struct.iter_unpack(">I", raw[:bs]):
                    s = (s + v) & 0xFFFFFFFF
                if s == 0:
                    nl = raw[bs - 80]
                    label = raw[bs - 79 : bs - 79 + min(nl, 30)].decode(
                        "latin-1", "replace")
                    guesses.append(
                        "valid OFS/FFS root block at the midpoint (label %r, "
                        "block size %d) -- bootblock may be damaged" % (label, bs))
                    break
    except Exception:
        pass
    if not guesses:
        if magic == b"\x00\x00\x00\x00":
            guesses.append("first block is blank: likely unformatted")
        else:
            guesses.append("no known filesystem signature found")
    return out


def open_image(path, writable=False):
    if not os.path.exists(path):
        raise ImageError("no such file: %s" % path)
    return DiskImage(path, writable=writable)
