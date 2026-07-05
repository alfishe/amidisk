"""PFS3 native-write unit tests: mutations on fixture copies and fresh volumes."""

import os
import unittest

from .util import TempDirTest, REAL_HDF

from amidisk import open_image
from amidisk.fs.ffs import FSError


class _FakeHuge:
    """len() >= 4 GB without allocating it (guard-path test)."""

    def __len__(self):
        return 1 << 32


class TestMutationsOnRealFixture(TempDirTest):
    """Every mutation runs on a copy of the real pfs3aio-written volume."""

    def mount_rw(self):
        path = self.copy_fixture(REAL_HDF)
        img = open_image(path, writable=True)
        self.addCleanup(img.close)
        return img.volumes[0].mount()

    def test_write_read_roundtrip(self):
        vol = self.mount_rw()
        for size in (0, 1, 511, 512, 513, 100_000):
            data = os.urandom(size)
            vol.write_file("t%d.bin" % size, data)
            self.assertEqual(vol.read_file_bytes("t%d.bin" % size), data, size)
        rep = vol.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])

    def test_overwrite_shrinks_and_grows(self):
        vol = self.mount_rw()
        vol.write_file("f.bin", os.urandom(50_000))
        small = b"tiny"
        vol.write_file("f.bin", small)
        self.assertEqual(vol.read_file_bytes("f.bin"), small)
        big = os.urandom(200_000)
        vol.write_file("f.bin", big)
        self.assertEqual(vol.read_file_bytes("f.bin"), big)
        self.assertTrue(vol.check(deep=True)["ok"])

    def test_comment_protect_mtime(self):
        from datetime import datetime

        vol = self.mount_rw()
        ts = datetime(2001, 2, 3, 4, 5, 6)
        vol.write_file("meta.bin", b"x", protect=0x0E, comment=b"a comment",
                       mtime=ts)
        e = vol.resolve("meta.bin")
        self.assertEqual(e.comment, b"a comment")
        self.assertEqual(e.protect & 0xFF, 0x0E)
        self.assertEqual(e.mtime().replace(microsecond=0), ts)

    def test_mkdir_and_nested(self):
        vol = self.mount_rw()
        vol.makedirs("A/B/C/D")
        self.assertTrue(vol.resolve("A/B/C/D").is_dir())
        with self.assertRaises(FSError):
            vol.mkdir("A/B")  # already exists

    def test_delete_semantics(self):
        vol = self.mount_rw()
        vol.makedirs("D/Sub")
        vol.write_file("D/Sub/f.bin", b"data")
        with self.assertRaises(FSError):
            vol.delete("D")  # not empty, not recursive
        vol.delete("D", recursive=True)
        with self.assertRaises(FSError):
            vol.resolve("D")
        rep = vol.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])

    def test_rename_across_dirs(self):
        vol = self.mount_rw()
        vol.makedirs("R/One")
        vol.makedirs("R/Two")
        vol.write_file("R/One/f.bin", b"payload")
        vol.rename("R/One/f.bin", "R/Two/g.bin")
        self.assertEqual(vol.read_file_bytes("R/Two/g.bin"), b"payload")
        with self.assertRaises(FSError):
            vol.resolve("R/One/f.bin")
        # dir rename fixes parent pointers
        vol.rename("R/Two", "R/One/Moved")
        self.assertEqual(vol.read_file_bytes("R/One/Moved/g.bin"), b"payload")
        self.assertTrue(vol.check(deep=True)["ok"])

    def test_rename_into_own_subtree_refused(self):
        vol = self.mount_rw()
        vol.makedirs("X/Y")
        with self.assertRaises(FSError):
            vol.rename("X", "X/Y/X2")

    def test_fragmented_file_multi_anode(self):
        vol = self.mount_rw()
        vol.mkdir("Frag")
        # consume ALL free space with numbered files ...
        i = 0
        while True:
            try:
                vol.write_file("Frag/f%d" % i, b"\x55" * 100_000)
                i += 1
            except FSError:
                break
        self.assertGreater(i, 5)
        # ... then punch holes; the only free space left IS the holes
        for k in range(0, i, 2):
            vol.delete("Frag/f%d" % k)
        big = os.urandom(220_000)  # bigger than one 100K hole
        vol.write_file("Frag/big", big)
        self.assertEqual(vol.read_file_bytes("Frag/big"), big)
        e = vol.resolve("Frag/big")
        self.assertGreater(len(vol._anode_chain(e.anode)), 1)
        self.assertTrue(vol.check(deep=True)["ok"])

    def test_many_entries_extend_dirblocks(self):
        vol = self.mount_rw()
        vol.mkdir("Lots")
        for i in range(200):
            vol.write_file("Lots/file-with-a-longish-name-%03d" % i, b"x" * i)
        self.assertEqual(len(vol.list_dir("Lots")), 200)
        # 200 entries never fit one reserved block -> chain grew
        self.assertGreater(len(vol._dir_chain(vol.resolve("Lots").anode)), 1)
        for i in (0, 99, 199):
            self.assertEqual(
                vol.read_file_bytes("Lots/file-with-a-longish-name-%03d" % i),
                b"x" * i,
            )
        self.assertTrue(vol.check(deep=True)["ok"])

    def test_no_space_leak_after_cycle(self):
        vol = self.mount_rw()
        before = vol.get_info()["free_bytes"]
        vol.makedirs("Leak/Deep")
        for i in range(25):
            vol.write_file("Leak/Deep/f%d" % i, os.urandom(4_000 + i))
        vol.rename("Leak/Deep/f1", "Leak/g1")
        vol.write_file("Leak/Deep/f2", b"overwritten")
        vol.delete("Leak", recursive=True)
        self.assertEqual(vol.get_info()["free_bytes"], before)
        rep = vol.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])

    def test_untouched_files_unchanged(self):
        if not os.path.exists(REAL_HDF):
            self.skipTest("pfs3-real.hdf missing")
        with open_image(REAL_HDF) as img:
            pristine = img.volumes[0].mount().read_file_bytes(
                "MMULib/MuTools/MuForce.guide"
            )
        vol = self.mount_rw()
        vol.write_file("noise.bin", os.urandom(500_000))
        vol.delete("noise.bin")
        self.assertEqual(
            vol.read_file_bytes("MMULib/MuTools/MuForce.guide"), pristine
        )

    def test_name_validation(self):
        vol = self.mount_rw()
        for bad in (b"", b"a" * 40, b"has/slash", b"has:colon", b"ctl\x01"):
            with self.assertRaises(FSError, msg=bad):
                vol.write_file(bad, b"x")
        with self.assertRaises(FSError):
            vol.write_file("huge.bin", _FakeHuge())

    def test_delete_root_refused(self):
        vol = self.mount_rw()
        with self.assertRaises(FSError):
            vol.delete("")

    def test_volume_full(self):
        vol = self.mount_rw()
        free = vol.get_info()["free_bytes"]
        with self.assertRaises(FSError):
            vol.write_file("toobig.bin", b"\x00" * (free + 1_000_000))
        # failed alloc must roll back cleanly
        self.assertEqual(vol.get_info()["free_bytes"], free)
        self.assertTrue(vol.check()["ok"])


if __name__ == "__main__":
    unittest.main()
