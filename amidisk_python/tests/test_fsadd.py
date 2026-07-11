"""RDB embedded filesystem drivers (FSHD/LSEG write support)."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRATCH_BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scratch")

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(os.path.dirname(TEST_DIR), "src")
REPO_ROOT = os.path.dirname(os.path.dirname(TEST_DIR))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from amidisk.blkdev import ImageFileBlkDev      # noqa: E402
from amidisk.rdb import RDisk, RDiskError       # noqa: E402

DRIVERS = os.path.join(REPO_ROOT, "data", "drivers")
PFS3AIO = os.path.join(DRIVERS, "pfs3aio")


@unittest.skipUnless(os.path.exists(PFS3AIO), "driver collection missing")
class TestFsAdd(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.join(SCRATCH_BASE, self.__class__.__name__)
        os.makedirs(self.tmp, exist_ok=True)
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.img = os.path.join(self.tmp, "t.hdf")
        with open(self.img, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    def test_roundtrip_and_chain(self):
        data = open(PFS3AIO, "rb").read()
        dev = ImageFileBlkDev(self.img, read_only=False)
        rd = RDisk.create(dev)
        fs = rd.add_filesystem(data, 0x50465303, version=(19, 2))
        self.assertEqual(fs.get_version_string(), "19.2")
        # duplicate dostype refused
        with self.assertRaises(RDiskError):
            rd.add_filesystem(data, 0x50465303)
        dev.close()
        # reopen from scratch: chain must parse and extract bit-perfect
        dev = ImageFileBlkDev(self.img)
        rd = RDisk(dev)
        self.assertTrue(rd.open())
        self.assertEqual(len(rd.get_filesystems()), 1)
        self.assertEqual(rd.get_filesystems()[0].get_data(), data)
        dev.close()

    def test_area_too_small(self):
        data = open(PFS3AIO, "rb").read()
        dev = ImageFileBlkDev(self.img, read_only=False)
        rd = RDisk.create(dev, rdb_cyls=1)  # 63 sectors: too small
        with self.assertRaises(RDiskError):
            rd.add_filesystem(data, 0x50465303)
        dev.close()

    def test_cli_auto_embed(self):
        env = dict(os.environ, AMIDISK_DRIVERS=DRIVERS)

        def run(*args):
            r = subprocess.run([sys.executable, "-m", "amidisk.cli", *args],
                               capture_output=True, text=True, cwd=ROOT,
                               env=env)
            self.assertEqual(r.returncode, 0, r.stderr or r.stdout)
            return r.stdout

        run("rdb-init", self.img)
        out = run("part-add", self.img, "DH0", "--dostype", "pfs3",
                  "--format", "Sys")
        self.assertIn("embedded PFS", out)
        out = run("info", self.img, "--json")
        info = json.loads(out)
        self.assertEqual(len(info["filesystems"]), 1)
        self.assertEqual(info["filesystems"][0]["version"], "19.2")


if __name__ == "__main__":
    unittest.main()
