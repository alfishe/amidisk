"""PFS3 native format unit tests."""

import os
import struct
import unittest

from .util import TempDirTest, PFS3_DOSTYPE

from amidisk.fs.ffs import FSError
from amidisk.fs.pfs3 import PFS3Volume, ROOTBLOCK, ID_PFS_DISK


class TestFormat(TempDirTest):
    def test_bootblock_and_rootblock(self):
        path, dev, vol = self.formatted_volume(16, b"FmtVol")
        raw = dev.read(0)
        self.assertEqual(struct.unpack_from(">I", raw, 0)[0], ID_PFS_DISK)
        root = dev.read(ROOTBLOCK)
        self.assertEqual(struct.unpack_from(">I", root, 0)[0], ID_PFS_DISK)
        self.assertEqual(vol.label, "FmtVol")

    def test_counters_consistent(self):
        path, dev, vol = self.formatted_volume(32, b"Counters")
        info = vol.get_info()
        # free + reserved area + boot must equal the disk
        main_area = vol.total - (vol.lastreserved + 1)
        self.assertEqual(info["free_blocks"], main_area)
        self.assertGreater(vol.reserved_free, 0)
        self.assertEqual(vol.firstreserved, 2)

    def test_empty_root_lists_and_checks(self):
        path, dev, vol = self.formatted_volume(16)
        self.assertEqual(vol.list_dir(""), [])
        rep = vol.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])
        self.assertEqual((rep["files"], rep["dirs"]), (0, 0))

    def test_write_after_format_various_sizes(self):
        for mb in (8, 16, 96):
            path, dev, vol = self.formatted_volume(mb, b"S%d" % mb)
            data = os.urandom(300_000)
            vol.makedirs("A/B")
            vol.write_file("A/B/x.bin", data)
            self.assertEqual(vol.read_file_bytes("A/B/x.bin"), data)
            rep = vol.check(deep=True)
            self.assertTrue(rep["ok"], "%dMB: %s" % (mb, rep["errors"][:3]))

    def test_fill_volume_completely(self):
        path, dev, vol = self.formatted_volume(8, b"Full")
        i = 0
        while True:
            try:
                vol.write_file("f%d" % i, b"\xAA" * 200_000)
                i += 1
            except FSError:
                break
        self.assertGreater(i, 10)  # ~8MB / 200K
        self.assertTrue(vol.check(deep=True)["ok"])
        # free some and confirm the space is reusable
        vol.delete("f0")
        vol.write_file("again", b"\xBB" * 100_000)
        self.assertEqual(vol.read_file_bytes("again"), b"\xBB" * 100_000)

    def test_label_validation(self):
        path, dev, vol = self.blank_volume(8)
        with self.assertRaises(FSError):
            vol.format(b"", dos_type=PFS3_DOSTYPE)
        with self.assertRaises(FSError):
            vol.format(b"x" * 40, dos_type=PFS3_DOSTYPE)

    def test_too_small_volume(self):
        from amidisk.blkdev import ImageFileBlkDev

        # 16 KB cannot even hold the PFS3 reserved area (pfs3aio itself
        # has no minimum-size constant; even 64 KB volumes format legally)
        path = self.tpath("tiny.hdf")
        with open(path, "wb") as fh:
            fh.truncate(16 * 1024)
        dev = ImageFileBlkDev(path, read_only=False)
        self.addCleanup(dev.close)
        vol = PFS3Volume(dev, dos_type=PFS3_DOSTYPE)
        with self.assertRaises(FSError):
            vol.format(b"Tiny", dos_type=PFS3_DOSTYPE)

    def test_reformat_wipes(self):
        path, dev, vol = self.formatted_volume(8, b"First")
        vol.write_file("f.bin", b"old data")
        vol2 = PFS3Volume(dev, dos_type=PFS3_DOSTYPE)
        vol2.format(b"Second", dos_type=PFS3_DOSTYPE)
        vol2 = PFS3Volume(dev, dos_type=PFS3_DOSTYPE).open()
        self.assertEqual(vol2.label, "Second")
        self.assertEqual(vol2.list_dir(""), [])
        with self.assertRaises(FSError):
            vol2.resolve("f.bin")


if __name__ == "__main__":
    unittest.main()
