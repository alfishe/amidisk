"""PFS3 reader unit tests against real-handler and hst-imager fixtures."""

import unittest

from .util import TempDirTest, REAL_HDF, HST_HDF

from amidisk import open_image
from amidisk.fs.ffs import FSError


class TestMountReal(TempDirTest):
    def open_real(self):
        if not __import__("os").path.exists(REAL_HDF):
            self.skipTest("pfs3-real.hdf missing")
        img = open_image(REAL_HDF)
        self.addCleanup(img.close)
        return img.volumes[0].mount()

    def test_info(self):
        vol = self.open_real()
        info = vol.get_info()
        self.assertEqual(info["label"], "PFS3AIO Volume")
        self.assertEqual(info["filesystem"], "PFS3")
        self.assertGreater(info["free_blocks"], 0)
        self.assertTrue(info["read_only"])  # fixture opened read-only

    def test_list_root(self):
        vol = self.open_real()
        names = [e.name_str() for e in vol.list_dir("")]
        self.assertIn("MMULib", names)
        self.assertEqual(names, sorted(names, key=str.lower))

    def test_resolve_case_insensitive(self):
        vol = self.open_real()
        a = vol.resolve("MMULib/MuTools/MuForce")
        b = vol.resolve("mmulib/mutools/MUFORCE")
        self.assertEqual(a.anode, b.anode)

    def test_read_file_content(self):
        vol = self.open_real()
        data = vol.read_file_bytes("MMULib/MuTools/MuForce.guide")
        self.assertEqual(len(data), 96576)
        self.assertTrue(data.startswith(b"@DATABASE"))

    def test_empty_file(self):
        vol = self.open_real()
        e = vol.resolve("MMULib/MuTools/MuForce_Off")
        self.assertEqual(e.size, 0)
        self.assertEqual(vol.read_file_bytes(e), b"")

    def test_streaming_matches_bytes(self):
        vol = self.open_real()
        chunks = b"".join(vol.read_file("MMULib/MuTools/MuForce"))
        self.assertEqual(chunks, vol.read_file_bytes("MMULib/MuTools/MuForce"))

    def test_walk_counts(self):
        vol = self.open_real()
        files = dirs = 0
        for prefix, e in vol.walk(""):
            files += e.is_file()
            dirs += e.is_dir()
        self.assertEqual((files, dirs), (281, 40))

    def test_errors(self):
        vol = self.open_real()
        with self.assertRaises(FSError):
            vol.resolve("No/Such/Path")
        with self.assertRaises(FSError):
            vol.read_file("MMULib")  # a directory
        with self.assertRaises(FSError):
            vol.list_dir("MMULib/MuTools/MuForce")  # a file

    def test_deep_check(self):
        vol = self.open_real()
        rep = vol.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])
        self.assertEqual(rep["files"], 281)

    def test_readonly_guard(self):
        vol = self.open_real()
        with self.assertRaises(FSError):
            vol.mkdir("Nope")
        with self.assertRaises(FSError):
            vol.write_file("Nope", b"x")


class TestMountHst(TempDirTest):
    def test_hst_volume(self):
        if not __import__("os").path.exists(HST_HDF):
            self.skipTest("pfs3-hst.hdf missing")
        with open_image(HST_HDF) as img:
            vol = img.get_volume("DH0").mount()
            self.assertEqual(vol.label, "TestPFS")
            self.assertEqual(len(vol.list_dir("Many")), 120)
            rep = vol.check(deep=True)
            self.assertTrue(rep["ok"], rep["errors"][:3])


if __name__ == "__main__":
    unittest.main()
