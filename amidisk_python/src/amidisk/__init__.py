"""amidisk -- read/write access to Amiga HDF/VHD disk images.

Layers (see doc/amiga-disk-tool-architecture.md):
  blkdev  -- image containers (raw HDF/ADF, VHD) as block devices
  rdb     -- Rigid Disk Block partition table (port of amitools rdblib)
  fs      -- native OFS/FFS filesystem engine (read/write)
"""

__version__ = "0.1.0"


def __getattr__(name):
    if name in ("DiskImage", "open_image"):
        from . import image

        return getattr(image, name)
    raise AttributeError(name)
