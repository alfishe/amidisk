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
        return info

    def close(self):
        self.blkdev.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def open_image(path, writable=False):
    if not os.path.exists(path):
        raise ImageError("no such file: %s" % path)
    return DiskImage(path, writable=writable)
