"""Comprehensive SFS native-write tests (v2 layer).

Runs against copies of the real SmartFilesystem-written fixture
(data/test/sfs-real.hdf) and freshly formatted volumes. check(deep=True)
includes the strict handler-visibility pass: objectnode registration,
node.data container pointers, hash16 values, and hashtable chain
reachability -- everything the real handler needs to see our objects.
"""

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

from amidisk import open_image                     # noqa: E402
from amidisk.blkdev import ImageFileBlkDev         # noqa: E402
from amidisk.fs.ffs import FSError                 # noqa: E402
from amidisk.fs.sfs import SFSVolume, _sfs_hash    # noqa: E402

REAL_HDF = os.path.join(ROOT, "tests", "data", "sfs-real.hdf")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.join(SCRATCH_BASE, self.__class__.__name__)
        os.makedirs(self.tmp, exist_ok=True)
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def tpath(self, n):
        return os.path.join(self.tmp, n)

    def mount_real_rw(self):
        if not os.path.exists(REAL_HDF):
            self.skipTest("sfs-real.hdf missing")
        p = self.tpath("real.hdf")
        shutil.copy(REAL_HDF, p)
        img = open_image(p, writable=True)
        self.addCleanup(img.close)
        return img.volumes[0].mount()

    def fresh(self, mb=16, label="Fresh"):
        p = self.tpath("fresh.hdf")
        with open(p, "wb") as fh:
            fh.truncate(mb * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        self.addCleanup(dev.close)
        SFSVolume(dev).format(label)
        return SFSVolume(dev).open()

    def assert_clean(self, vol, deep=True):
        rep = vol.check(deep=deep)
        self.assertTrue(rep["ok"], rep["errors"][:6])
        return rep


class TestHash(unittest.TestCase):
    def test_matches_real_handler_values(self):
        # hash16 values sampled from a volume written by SmartFilesystem
        for name, want in ((b".recycled", 18410), (b"Tools", 52662),
                           (b"WBStartup", 58997)):
            self.assertEqual(_sfs_hash(name, False), want, name)


class TestRealFixture(Base):
    def test_pristine_passes_strict_check(self):
        if not os.path.exists(REAL_HDF):
            self.skipTest("sfs-real.hdf missing")
        with open_image(REAL_HDF) as img:
            self.assert_clean(img.volumes[0].mount())

    def test_roundtrip_sizes(self):
        vol = self.mount_real_rw()
        for size in (0, 1, 511, 512, 513, 120_000):
            data = os.urandom(size)
            vol.write_file("t%d" % size, data)
            self.assertEqual(vol.read_file_bytes("t%d" % size), data, size)
        self.assert_clean(vol)

    def test_overwrite(self):
        vol = self.mount_real_rw()
        vol.write_file("f.bin", os.urandom(60_000))
        vol.write_file("f.bin", b"small now")
        self.assertEqual(vol.read_file_bytes("f.bin"), b"small now")
        self.assert_clean(vol)

    def test_meta(self):
        from datetime import datetime

        vol = self.mount_real_rw()
        ts = datetime(2003, 4, 5, 6, 7, 8)
        vol.write_file("m.bin", b"x", protect=0x0E, comment=b"note", mtime=ts)
        e = vol.resolve("m.bin")
        self.assertEqual(e.comment, b"note")
        self.assertEqual(e.protect_str(), "-------d")  # 0x0E: only D allowed
        self.assertEqual(e.mtime().replace(microsecond=0), ts)

    def test_mkdir_delete_rename(self):
        vol = self.mount_real_rw()
        vol.makedirs("D/E/F")
        vol.write_file("D/E/F/x", b"deep")
        with self.assertRaises(FSError):
            vol.delete("D")
        vol.rename("D/E/F/x", "D/y")
        self.assertEqual(vol.read_file_bytes("D/y"), b"deep")
        vol.rename("D/E", "D/Moved")
        vol.delete("D", recursive=True)
        with self.assertRaises(FSError):
            vol.resolve("D")
        self.assert_clean(vol)

    def test_many_entries_and_hash_chains(self):
        vol = self.mount_real_rw()
        vol.mkdir("Lots")
        for i in range(120):
            vol.write_file("Lots/entry-%03d" % i, bytes([i]) * (i + 1))
        self.assertEqual(len(vol.list_dir("Lots")), 120)
        for i in (0, 59, 119):
            self.assertEqual(
                vol.read_file_bytes("Lots/entry-%03d" % i), bytes([i]) * (i + 1)
            )
        self.assert_clean(vol)

    def test_no_space_leak(self):
        vol = self.mount_real_rw()
        before = vol.get_info()["free_bytes"]
        vol.makedirs("L/M")
        for i in range(20):
            vol.write_file("L/M/f%d" % i, os.urandom(5_000 + i))
        vol.rename("L/M/f3", "L/g3")
        vol.write_file("L/M/f4", b"overwrite")
        vol.delete("L", recursive=True)
        self.assertEqual(vol.get_info()["free_bytes"], before)
        self.assert_clean(vol)

    def test_untouched_files_stable(self):
        if not os.path.exists(REAL_HDF):
            self.skipTest("sfs-real.hdf missing")
        with open_image(REAL_HDF) as img:
            keep = img.volumes[0].mount().read_file_bytes(
                "Prefs/Env-Archive/Sys/screenmode.prefs")
        vol = self.mount_real_rw()
        vol.write_file("noise", os.urandom(200_000))
        vol.delete("noise")
        self.assertEqual(
            vol.read_file_bytes("Prefs/Env-Archive/Sys/screenmode.prefs"), keep)

    def test_volume_full_rolls_back(self):
        vol = self.mount_real_rw()
        free = vol.get_info()["free_bytes"]
        with self.assertRaises(FSError):
            vol.write_file("big", b"\x00" * (free + 1_000_000))
        self.assertEqual(vol.get_info()["free_bytes"], free)
        self.assert_clean(vol, deep=False)


class TestFormat(Base):
    def test_fresh_layout(self):
        vol = self.fresh()
        names = [e.name_str() for e in vol.list_dir("")]
        self.assertEqual(names, [".recycled"])
        self.assertTrue(vol.root_obj.hashtable)
        self.assert_clean(vol)

    def test_write_after_format(self):
        vol = self.fresh()
        data = os.urandom(400_000)
        vol.makedirs("A/B")
        vol.write_file("A/B/x.bin", data)
        self.assertEqual(vol.read_file_bytes("A/B/x.bin"), data)
        self.assert_clean(vol)

    def test_fill_and_reuse(self):
        vol = self.fresh(8)
        i = 0
        while True:
            try:
                vol.write_file("f%d" % i, b"\xAA" * 150_000)
                i += 1
            except FSError:
                break
        self.assertGreater(i, 10)
        vol.delete("f0")
        vol.write_file("again", b"\xBB" * 100_000)
        self.assertEqual(vol.read_file_bytes("again"), b"\xBB" * 100_000)
        self.assert_clean(vol)


class TestStreaming(Base):
    """Image-to-image streaming: no temp files, no whole-file buffering."""

    def test_stream_matrix(self):
        payload = os.urandom(1_500_000)
        # source: FFS volume
        src = self.tpath("src.hdf")
        with open(src, "wb") as fh:
            fh.truncate(8 * 1024 * 1024)
        from amidisk.fs.ffs import FFSVolume

        d0 = ImageFileBlkDev(src, read_only=False)
        self.addCleanup(d0.close)
        FFSVolume(d0).format(b"Src", dos_type=0x444F5303)
        v_ffs = FFSVolume(d0).open()
        v_ffs.write_file("payload.bin", payload)

        # FFS -> SFS -> PFS3 -> back, all streamed via read_file iterators
        v_sfs = self.fresh(8, "Mid")
        e = v_ffs.resolve("payload.bin")
        v_sfs.write_file("hop1.bin", v_ffs.read_file(e), size=e.size)

        pfs = self.tpath("pfs.hdf")
        with open(pfs, "wb") as fh:
            fh.truncate(8 * 1024 * 1024)
        from amidisk.fs.pfs3 import PFS3Volume

        d2 = ImageFileBlkDev(pfs, read_only=False)
        self.addCleanup(d2.close)
        PFS3Volume(d2, dos_type=0x50465303).format(b"Hop2", dos_type=0x50465303)
        v_pfs = PFS3Volume(d2, dos_type=0x50465303).open()
        e = v_sfs.resolve("hop1.bin")
        v_pfs.write_file("hop2.bin", v_sfs.read_file(e), size=e.size)

        e = v_pfs.resolve("hop2.bin")
        v_ffs.write_file("final.bin", v_pfs.read_file(e), size=e.size)
        self.assertEqual(v_ffs.read_file_bytes("final.bin"), payload)
        for v in (v_ffs, v_sfs, v_pfs):
            rep = v.check(deep=True)
            self.assertTrue(rep["ok"], rep["errors"][:4])

    def test_short_stream_rejected_cleanly(self):
        vol = self.fresh(8)
        free = vol.get_info()["free_bytes"]

        def short():
            yield b"only this"

        with self.assertRaises(FSError):
            vol.write_file("s.bin", short(), size=100_000)
        self.assertEqual(vol.get_info()["free_bytes"], free)
        self.assert_clean(vol, deep=False)

    def test_file_like_source(self):
        import io

        vol = self.fresh(8)
        data = os.urandom(300_000)
        vol.write_file("fh.bin", io.BytesIO(data), size=len(data))
        self.assertEqual(vol.read_file_bytes("fh.bin"), data)


if __name__ == "__main__":
    unittest.main()
