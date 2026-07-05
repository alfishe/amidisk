"""bulk() mode: deferred metadata commits for mass imports.

Contract: identical on-disk result after a clean exit; bounded,
detectable, repairable damage on a simulated crash mid-bulk.
"""

import os
import shutil
import sys
import tempfile
import unittest

SCRATCH_BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scratch")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from amidisk.blkdev import ImageFileBlkDev      # noqa: E402
from amidisk.fs.ffs import FFSVolume            # noqa: E402
from amidisk.fs.pfs3 import PFS3Volume          # noqa: E402
from amidisk.fs.sfs import SFSVolume            # noqa: E402

ENGINES = [
    (FFSVolume, 0x444F5307, "FFS DOS\\7"),
    (PFS3Volume, 0x50465303, "PFS3"),
    (SFSVolume, 0x53465300, "SFS"),
]


class TestBulk(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.join(SCRATCH_BASE, self.__class__.__name__)
        os.makedirs(self.tmp, exist_ok=True)
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def fresh(self, cls, dt, name):
        p = os.path.join(self.tmp, name.replace("\\", "").replace(" ", "") + ".hdf")
        with open(p, "wb") as fh:
            fh.truncate(32 * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        self.addCleanup(dev.close)
        cls(dev, dos_type=dt).format(b"Bulk", dos_type=dt)
        return dev, cls(dev, dos_type=dt).open()

    def test_bulk_equivalence_all_engines(self):
        """Clean-exit bulk import must verify exactly like per-op mode."""
        for cls, dt, tag in ENGINES:
            with self.subTest(tag):
                dev, vol = self.fresh(cls, dt, "eq" + tag)
                data = {("D%d/f%02d" % (i // 40, i)): os.urandom(500 + i * 37)
                        for i in range(120)}
                with vol.bulk(flush_every=16):
                    made = set()
                    for path, payload in data.items():
                        d = path.split("/")[0]
                        if d not in made:
                            vol.mkdir(d)
                            made.add(d)
                        vol.write_file(path, payload)
                    # mutations inside bulk too
                    vol.rename("D0/f00", "D0/renamed")
                    vol.delete("D0/f01")
                data["D0/renamed"] = data.pop("D0/f00")
                del data["D0/f01"]
                # reopen from disk: everything must be there and clean
                vol2 = cls(dev, dos_type=dt).open()
                for path, payload in data.items():
                    self.assertEqual(vol2.read_file_bytes(path), payload, path)
                rep = vol2.check(deep=True)
                self.assertTrue(rep["ok"], (tag, rep["errors"][:4]))
                self.assertFalse(rep["warnings"], (tag, rep["warnings"]))

    def test_bulk_flushes_on_exception(self):
        dev, vol = self.fresh(FFSVolume, 0x444F5303, "exc")
        try:
            with vol.bulk():
                for i in range(20):
                    vol.write_file("f%d" % i, b"x" * 1000)
                raise RuntimeError("simulated caller failure")
        except RuntimeError:
            pass
        vol2 = FFSVolume(dev, dos_type=0x444F5303).open()
        rep = vol2.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:4])
        self.assertEqual(rep["files"], 20)

    def _crash_mid_bulk(self, wrap_cache):
        p = os.path.join(self.tmp, "crash%d.hdf" % wrap_cache)
        with open(p, "wb") as fh:
            fh.truncate(32 * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        FFSVolume(dev, dos_type=0x444F5303).format(b"C", dos_type=0x444F5303)
        vol = FFSVolume(dev, dos_type=0x444F5303).open()
        ctx = vol.bulk(flush_every=10_000)  # never reaches a batch point
        ctx.__enter__()
        if wrap_cache:
            # wrap after entering bulk so the not-validated marker is
            # already on disk (matches how activation used to order it)
            from amidisk.blkdev import BulkWriteCache
            vol.dev = BulkWriteCache(vol.dev)
        for i in range(50):
            vol.write_file("f%d" % i, os.urandom(2000))
        dev.flush()
        dev.close()
        try:
            ctx.__exit__(None, None, None)
        except ValueError:
            pass  # flush against a closed device: the simulated crash
        return p

    def test_ffs_crash_pagecached_rolls_back_consistently(self):
        """Page-cached bulk: dir links and bitmap are deferred in RAM, so
        a crash leaves written-but-unlinked headers as plain free space.
        The volume must be consistent, marked not-validated, and repair
        must revalidate it."""
        p = self._crash_mid_bulk(wrap_cache=False)
        dev = ImageFileBlkDev(p, read_only=False)
        vol2 = FFSVolume(dev, dos_type=0x444F5303).open()
        root = vol2.read_buf(vol2.root_blk)
        self.assertEqual(root.slong(-50), 0, "bm_flag should be invalid")
        rep = vol2.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:4])
        self.assertTrue(any("not validated" in w for w in rep["warnings"]),
                        rep["warnings"])
        self.assertEqual(rep["files"], 0)  # nothing was ever linked
        fix = vol2.repair(apply=True)
        self.assertTrue(fix["applied"])
        rep = vol2.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:4])
        self.assertFalse(rep["warnings"], rep["warnings"])
        dev.close()

    def test_ffs_crash_with_write_cache_rolls_back_atomically(self):
        """With an explicit BulkWriteCache wrap, a crash loses the RAM
        batch wholesale -- the on-disk volume stays CONSISTENT, marked
        not-validated, and repair revalidates it."""
        p = self._crash_mid_bulk(wrap_cache=True)
        dev = ImageFileBlkDev(p, read_only=False)
        vol2 = FFSVolume(dev, dos_type=0x444F5303).open()
        root = vol2.read_buf(vol2.root_blk)
        self.assertEqual(root.slong(-50), 0, "bm_flag should be invalid")
        rep = vol2.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:4])
        self.assertTrue(any("not validated" in w for w in rep["warnings"]),
                        rep["warnings"])
        self.assertEqual(rep["files"], 0)  # batch rolled back atomically
        fix = vol2.repair(apply=True)
        self.assertTrue(fix["applied"])
        rep = vol2.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:4])
        self.assertFalse(rep["warnings"], rep["warnings"])
        dev.close()

    def test_default_mode_unchanged(self):
        """Without bulk(), every operation still commits immediately."""
        dev, vol = self.fresh(FFSVolume, 0x444F5303, "def")
        vol.write_file("a", b"1")
        # a fresh mount (separate object) must see a valid, current state
        vol2 = FFSVolume(dev, dos_type=0x444F5303).open()
        root = vol2.read_buf(vol2.root_blk)
        self.assertEqual(root.slong(-50), -1)  # bitmap valid
        self.assertTrue(vol2.check(deep=True)["ok"])


if __name__ == "__main__":
    unittest.main()
