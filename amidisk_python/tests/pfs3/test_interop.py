"""Differential tests against hst-imager's independent C# PFS3 implementation.

Skipped when the hst.imager binary is unavailable (set HST_IMAGER to point
at it). These are the strongest correctness signals we have short of
mounting under the real pfs3aio handler in an emulator.

Note: hst.imager on Windows misinterprets forward slashes in image paths
like "img.hdf/rdb/dh0", so these tests run only on Unix.
"""

import os
import subprocess
import sys
import unittest

from .util import TempDirTest, hst_imager, HST_HDF

from amidisk import open_image


def _hst(*args):
    r = subprocess.run([hst_imager(), *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError("hst.imager %s failed:\n%s" % (args[0], r.stderr or r.stdout))
    return r.stdout


@unittest.skipIf(sys.platform == "win32", "hst.imager path syntax broken on Windows")
@unittest.skipUnless(hst_imager(), "hst.imager binary not available")
class TestInterop(TempDirTest):
    def make_rdb_pfs3(self, name="ip.hdf", size="48M", label="Interop"):
        """RDB image with one PFS3 partition, natively formatted by us."""
        import subprocess as sp
        import sys

        path = self.tpath(name)
        for cmd in (
            ["create", path, "--size", size],
            ["rdb-init", path],
            ["part-add", path, "DH0", "--dostype", "pfs3", "--format", label],
        ):
            r = sp.run([sys.executable, "-m", "amidisk.cli", *cmd],
                       capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
        return path

    def test_hst_reads_our_writes(self):
        path = self.copy_fixture(HST_HDF)
        payload = os.urandom(1_000_000)
        with open_image(path, writable=True) as img:
            vol = img.get_volume("DH0").mount()
            vol.mkdir("XChg")
            vol.write_file("XChg/ours.bin", payload)
        _hst("fs", "copy", path + "/rdb/dh0/XChg/ours.bin", self.tmp)
        with open(self.tpath("ours.bin"), "rb") as fh:
            self.assertEqual(fh.read(), payload)

    def test_hst_writes_after_ours(self):
        path = self.copy_fixture(HST_HDF)
        with open_image(path, writable=True) as img:
            vol = img.get_volume("DH0").mount()
            vol.write_file("marker.bin", b"ours first")
        theirs = self.tpath("theirs.txt")
        with open(theirs, "w") as fh:
            fh.write("hst second")
        _hst("fs", "copy", theirs, path + "/rdb/dh0")
        with open_image(path) as img:
            vol = img.get_volume("DH0").mount()
            self.assertEqual(vol.read_file_bytes("theirs.txt"), b"hst second")
            self.assertEqual(vol.read_file_bytes("marker.bin"), b"ours first")
            self.assertTrue(vol.check(deep=True)["ok"])

    def test_hst_accepts_our_format(self):
        path = self.make_rdb_pfs3()
        payload = os.urandom(300_000)
        with open_image(path, writable=True) as img:
            vol = img.get_volume("DH0").mount()
            vol.mkdir("Data")
            vol.write_file("Data/pl.bin", payload)
        out = _hst("fs", "dir", path + "/rdb/dh0/Data")
        self.assertIn("pl.bin", out)
        _hst("fs", "copy", path + "/rdb/dh0/Data/pl.bin", self.tmp)
        with open(self.tpath("pl.bin"), "rb") as fh:
            self.assertEqual(fh.read(), payload)
        # and hst can allocate on our fresh structures
        marker = self.tpath("m.txt")
        with open(marker, "w") as fh:
            fh.write("hst on our format")
        _hst("fs", "copy", marker, path + "/rdb/dh0")
        with open_image(path) as img:
            vol = img.get_volume("DH0").mount()
            self.assertEqual(vol.read_file_bytes("m.txt"), b"hst on our format")
            self.assertTrue(vol.check(deep=True)["ok"])

    def test_deep_equivalence_after_mixed_ops(self):
        """Alternate writers, then verify the full tree from both sides."""
        path = self.make_rdb_pfs3(label="Mixed")
        files = {}
        with open_image(path, writable=True) as img:
            vol = img.get_volume("DH0").mount()
            vol.mkdir("M")
            for i in range(10):
                data = os.urandom(5_000 + 1_000 * i)
                files["M/a%d" % i] = data
                vol.write_file("M/a%d" % i, data)
        for i in range(5):
            p = self.tpath("b%d" % i)
            data = os.urandom(3_000 + i)
            with open(p, "wb") as fh:
                fh.write(data)
            files["M/b%d" % i] = data
            _hst("fs", "copy", p, path + "/rdb/dh0/M")
        with open_image(path) as img:
            vol = img.get_volume("DH0").mount()
            for pth, data in files.items():
                self.assertEqual(vol.read_file_bytes(pth), data, pth)
            self.assertTrue(vol.check(deep=True)["ok"])


if __name__ == "__main__":
    unittest.main()
