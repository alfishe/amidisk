"""Shared fixtures/helpers for the PFS3 test suite."""

import os
import shutil
import sys
import tempfile
import unittest

SCRATCH_BASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scratch")

TEST_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.join(os.path.dirname(TEST_DIR), "src")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

TESTDATA = os.path.join(TEST_DIR, "data")
REAL_HDF = os.path.join(TESTDATA, "pfs3-real.hdf")   # written by real pfs3aio
HST_HDF = os.path.join(TESTDATA, "pfs3-hst.hdf")     # written by hst-imager

PFS3_DOSTYPE = 0x50465303


def hst_imager():
    """Path to the hst-imager binary, or None (interop tests skip)."""
    import shutil
    import subprocess
    cand = os.environ.get("HST_IMAGER")
    if cand and os.access(cand, os.X_OK):
        return cand
    is_win = sys.platform == "win32"
    names = ["hst.imager", "hst-imager"]
    if is_win:
        names = [n + ".exe" for n in names] + names
    # check tests/tools/<platform>
    tools_dir = os.path.join(ROOT, "tests", "tools", sys.platform)
    for name in names:
        cand = os.path.join(tools_dir, name)
        if os.path.isfile(cand) and (is_win or os.access(cand, os.X_OK)):
            # verify it actually runs (x64 binary on arm64 via rosetta may fail)
            try:
                r = subprocess.run([cand, "--version"], capture_output=True, timeout=5)
                if r.returncode == 0:
                    return cand
            except Exception:
                pass
    # check PATH
    for name in names:
        cand = shutil.which(name)
        if cand:
            return cand
    return None


class TempDirTest(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.join(SCRATCH_BASE, self.__class__.__name__)
        os.makedirs(self.tmp, exist_ok=True)
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def tpath(self, name):
        return os.path.join(self.tmp, name)

    def copy_fixture(self, src, name="work.hdf"):
        if not os.path.exists(src):
            self.skipTest("fixture missing: %s" % src)
        dst = self.tpath(name)
        shutil.copy(src, dst)
        return dst

    def blank_volume(self, size_mb=16, name=None):
        """Blank bare image + writable PFS3Volume (not formatted yet)."""
        from amidisk.blkdev import ImageFileBlkDev
        from amidisk.fs.pfs3 import PFS3Volume

        if name is None:
            # unique name per call avoids Windows handle conflicts in loops
            n = getattr(self, "_blank_counter", 0)
            self._blank_counter = n + 1
            name = "blank%d.hdf" % n
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
