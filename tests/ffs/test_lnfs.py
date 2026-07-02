"""DOS\\6/\\7 long-filename (LNFS) tests.

Layout reference: FFS 45+/OS 3.1.4; combined name+comment field at
bs-184 (112 bytes), overflow comments in T_COMMENT blocks (type 64)
referenced at bs-72, entry dates at bs-60. Differential tests use
amitools' xdftool when available (note: amitools' LNFS *writer* has a
name-copied-as-comment bug, so oracle-written names are kept <= 54
chars; its reader is fine).
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from amidisk.blkdev import ImageFileBlkDev      # noqa: E402
from amidisk.fs.ffs import FFSVolume, FSError   # noqa: E402


def xdftool():
    cand = os.path.join(
        os.path.expanduser("~"), ".local", "bin", "xdftool"
    )
    return cand if os.access(cand, os.X_OK) else None


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="amidisk-lnfs-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def vol(self, flavor, mb=8):
        p = os.path.join(self.tmp, "v%d.hdf" % flavor)
        with open(p, "wb") as fh:
            fh.truncate(mb * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        self.addCleanup(dev.close)
        FFSVolume(dev).format(b"LnVol", dos_type=0x444F5300 + flavor)
        return p, FFSVolume(dev).open()


class TestLNFS(Base):
    def test_mount_flags(self):
        for flavor in (6, 7):
            _, v = self.vol(flavor)
            self.assertTrue(v.is_longname)
            self.assertTrue(v.intl)
            self.assertFalse(v.dircache)
            self.assertEqual(v.max_name_len, 107)
            self.assertEqual(v.ffs, flavor == 7)

    def test_long_names_roundtrip(self):
        for flavor in (6, 7):
            _, v = self.vol(flavor)
            name = "L" * 98 + ".data107x"  # exactly 107
            data = os.urandom(50_000)
            v.write_file(name, data)
            self.assertEqual(v.read_file_bytes(name), data)
            self.assertEqual(len(v.resolve(name).name), 107)
            with self.assertRaises(FSError):
                v.write_file("X" * 108, b"too long")
            rep = v.check(deep=True)
            self.assertTrue(rep["ok"], rep["errors"][:3])

    def test_comment_inline_and_overflow(self):
        _, v = self.vol(7)
        v.write_file("short.txt", b"a", comment=b"inline comment")
        e = v.resolve("short.txt")
        self.assertEqual((e.comment, e.comment_block), (b"inline comment", 0))
        long_name = "N" * 100
        v.write_file(long_name, b"b", comment=b"c" * 40)  # 2+100+40 > 112
        e = v.resolve(long_name)
        self.assertEqual(e.comment, b"c" * 40)
        self.assertNotEqual(e.comment_block, 0)
        # list_dir also resolves overflow comments
        by_name = {x.name_str(): x for x in v.list_dir("")}
        self.assertEqual(by_name[long_name].comment, b"c" * 40)
        rep = v.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])

    def test_rename_overflow_flip_and_dates(self):
        _, v = self.vol(7)
        ts = datetime(2004, 5, 6, 7, 8, 9)
        v.write_file("c.bin", b"z", comment=b"C" * 70, mtime=ts)
        self.assertEqual(v.resolve("c.bin").mtime().replace(microsecond=0), ts)
        newname = "R" * 99  # forces the inline comment into a block
        v.rename("c.bin", newname)
        e = v.resolve(newname)
        self.assertEqual(e.comment, b"C" * 70)
        self.assertNotEqual(e.comment_block, 0)
        rep = v.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])

    def test_delete_frees_comment_blocks(self):
        _, v = self.vol(7)
        before = v.bitmap.count_free()
        v.write_file("M" * 100, b"x", comment=b"y" * 30)
        v.makedirs("D" * 80 + "/Sub")
        v.write_file("D" * 80 + "/Sub/f", b"g")
        v.delete("M" * 100)
        v.delete("D" * 80, recursive=True)
        self.assertEqual(v.bitmap.count_free(), before)
        rep = v.check(deep=True)
        self.assertTrue(rep["ok"] and not rep["warnings"],
                        (rep["errors"][:3], rep["warnings"]))

    def test_dir_date_stamp_does_not_corrupt_long_name(self):
        # adding entries to a long-named dir stamps its date; on LNFS the
        # date must land at -15, not inside the name field
        _, v = self.vol(7)
        dname = "DirWithAReallyQuiteLongDirectoryName_" + "d" * 60
        v.mkdir(dname)
        for i in range(5):
            v.write_file("%s/f%d" % (dname, i), bytes([i]))
        e = v.resolve(dname)
        self.assertEqual(e.name_str(), dname)  # name survived date updates
        rep = v.check(deep=True)
        self.assertTrue(rep["ok"], rep["errors"][:3])


@unittest.skipUnless(xdftool(), "xdftool not available")
class TestLNFSInterop(Base):
    def run_xdf(self, *args):
        r = subprocess.run([xdftool(), *args], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError("xdftool failed: %s" % (r.stderr or r.stdout))
        return r.stdout

    def test_xdftool_reads_ours(self):
        p, v = self.vol(7)
        name = "OurLongName_" + "x" * 80
        data = os.urandom(40_000)
        v.write_file(name, data, comment=b"inline note")
        v.dev.flush()
        out = self.run_xdf(p, "list")
        self.assertIn(name, out)
        self.assertIn("inline note", out)
        back = os.path.join(self.tmp, "back.bin")
        self.run_xdf(p, "read", name, back)
        with open(back, "rb") as fh:
            self.assertEqual(fh.read(), data)

    def test_we_read_xdftool_volume(self):
        p = os.path.join(self.tmp, "their.adf")
        payload = os.path.join(self.tmp, "pl.bin")
        data = os.urandom(30_000)
        with open(payload, "wb") as fh:
            fh.write(data)
        name = "A" * 50  # within amitools' buggy-writer limit
        self.run_xdf(p, "create", "+", "format", "Their", "ffs+intl+longname")
        self.run_xdf(p, "write", payload, name)
        dev = ImageFileBlkDev(p)
        self.addCleanup(dev.close)
        v = FFSVolume(dev).open()
        self.assertTrue(v.is_longname)
        self.assertEqual(v.read_file_bytes(name), data)
        self.assertTrue(v.check(deep=True)["ok"])


if __name__ == "__main__":
    unittest.main()
