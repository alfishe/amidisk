"""bulk() mode: deferred metadata commits for mass imports.

Contract: identical on-disk result after a clean exit; bounded,
detectable, repairable damage on a simulated crash mid-bulk.
"""

import os
import shutil
import sys
import tempfile
import unittest

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
        self.tmp = tempfile.mkdtemp(prefix="amidisk-bulk-")
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

    def test_ffs_crash_is_detectable_and_repairable(self):
        """Simulated crash: skip bulk's exit flush entirely. The volume
        must come up marked not-validated, check must flag it, and
        repair must restore full consistency."""
        p = os.path.join(self.tmp, "crash.hdf")
        with open(p, "wb") as fh:
            fh.truncate(32 * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        FFSVolume(dev, dos_type=0x444F5303).format(b"C", dos_type=0x444F5303)
        vol = FFSVolume(dev, dos_type=0x444F5303).open()
        ctx = vol.bulk(flush_every=10_000)  # never reaches a batch point
        ctx.__enter__()
        for i in range(50):
            vol.write_file("f%d" % i, os.urandom(2000))
        # crash: close the device out from under the context, then let
        # its exit flush fail -- equivalent to dying before the commit
        dev.flush()
        dev.close()
        try:
            ctx.__exit__(None, None, None)
        except ValueError:
            pass  # flush against a closed device: the simulated crash

        dev = ImageFileBlkDev(p, read_only=False)
        vol2 = FFSVolume(dev, dos_type=0x444F5303).open()
        # bm_flag must still say 'not validated'
        root = vol2.read_buf(vol2.root_blk)
        self.assertEqual(root.slong(-50), 0, "bm_flag should be invalid")
        rep = vol2.check()
        self.assertFalse(rep["ok"], "check must flag the stale bitmap")
        fix = vol2.repair(apply=True)
        self.assertTrue(fix["applied"])
        rep = vol2.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:4])
        self.assertEqual(rep["files"], 50)  # nothing lost, all repaired
        for i in (0, 25, 49):
            self.assertEqual(len(vol2.read_file_bytes("f%d" % i)), 2000)
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
