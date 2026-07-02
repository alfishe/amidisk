"""Shared fixtures/helpers for the PFS3 test suite."""

import os
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

TESTDATA = os.path.join(ROOT, "data", "test")
REAL_HDF = os.path.join(TESTDATA, "pfs3-real.hdf")   # written by real pfs3aio
HST_HDF = os.path.join(TESTDATA, "pfs3-hst.hdf")     # written by hst-imager

PFS3_DOSTYPE = 0x50465303


def hst_imager():
    """Path to the hst-imager binary, or None (interop tests skip)."""
    cand = os.environ.get("HST_IMAGER")
    if cand and os.access(cand, os.X_OK):
        return cand
    for base in (
        "/private/tmp/claude-501/-Volumes-TB4-4Tb-Projects-Amiga-software-AmigaFSTool"
        "/d433e96e-18d1-482c-9589-3c0f0571949b/scratchpad/hst/hst.imager",
    ):
        if os.access(base, os.X_OK):
            return base
    return None


class TempDirTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="amidisk-pfs3-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def tpath(self, name):
        return os.path.join(self.tmp, name)

    def copy_fixture(self, src, name="work.hdf"):
        if not os.path.exists(src):
            self.skipTest("fixture missing: %s" % src)
        dst = self.tpath(name)
        shutil.copy(src, dst)
        return dst

    def blank_volume(self, size_mb=16, name="blank.hdf"):
        """Blank bare image + writable PFS3Volume (not formatted yet)."""
        from amidisk.blkdev import ImageFileBlkDev
        from amidisk.fs.pfs3 import PFS3Volume

        path = self.tpath(name)
        with open(path, "wb") as fh:
            fh.truncate(size_mb * 1024 * 1024)
        dev = ImageFileBlkDev(path, read_only=False)
        self.addCleanup(dev.close)
        return path, dev, PFS3Volume(dev, dos_type=PFS3_DOSTYPE)

    def formatted_volume(self, size_mb=16, label=b"Test"):
        from amidisk.fs.pfs3 import PFS3Volume

        path, dev, vol = self.blank_volume(size_mb)
        vol.format(label, dos_type=PFS3_DOSTYPE)
        return path, dev, PFS3Volume(dev, dos_type=PFS3_DOSTYPE).open()
