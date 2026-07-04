"""Two tar reader engines must be interchangeable: identical inventories."""

import io
import os
import shutil
import sys
import tarfile
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from amidisk.tarreader import (  # noqa: E402
    ClassicTarReader, LibarchiveTarReader, LibarchiveError, open_tar)

REAL_TAR = os.path.join(ROOT, "data", "amiga_content.tar")


def _inventory(path, engine, with_data=True):
    inv = []
    with open_tar(path, engine) as rd:
        for e in rd:
            data = b"".join(e.chunks()) if (e.isfile and with_data) else None
            inv.append((e.name, e.kind, e.size, e.linkname, data))
    return inv


def _have_libarchive():
    try:
        LibarchiveTarReader._load()
        return True
    except (LibarchiveError, OSError):
        return False


class TestTarReaders(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="amidisk-tar-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def _synthetic_tar(self):
        p = os.path.join(self.tmp, "t.tar")
        with tarfile.open(p, "w") as tf:
            for d in ("Games", "Games/Sub"):
                ti = tarfile.TarInfo(d); ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            for name, payload in (
                    ("Games/RevengeOfMutantCamelsNTSC.Slave", b"x" * 3111),
                    ("Games/Sub/tiny", b""),
                    ("Games/Sub/a" * 3, os.urandom(70000)),
                    ("._AppleDouble.info", b"\x00\x05\x16\x07" + b"j" * 100)):
                ti = tarfile.TarInfo(name); ti.size = len(payload)
                tf.addfile(ti, io.BytesIO(payload))
            lnk = tarfile.TarInfo("Games/link")
            lnk.type = tarfile.LNKTYPE
            lnk.linkname = "Games/Sub/tiny"
            tf.addfile(lnk)
        return p

    def test_classic_reads_synthetic(self):
        inv = _inventory(self._synthetic_tar(), "classic")
        kinds = [k for _, k, _, _, _ in inv]
        self.assertEqual(kinds.count("file"), 4)
        self.assertEqual(kinds.count("dir"), 2)
        self.assertEqual(kinds.count("hardlink"), 1)

    @unittest.skipUnless(_have_libarchive(), "libarchive unavailable")
    def test_engines_identical_synthetic(self):
        p = self._synthetic_tar()
        self.assertEqual(_inventory(p, "classic"), _inventory(p, "libarchive"))

    @unittest.skipUnless(_have_libarchive(), "libarchive unavailable")
    def test_auto_prefers_libarchive(self):
        with open_tar(self._synthetic_tar()) as rd:
            self.assertEqual(rd.engine, "libarchive")

    @unittest.skipUnless(_have_libarchive() and os.path.exists(REAL_TAR),
                         "real tar or libarchive unavailable")
    def test_engines_identical_real_headers(self):
        """Full-inventory header equality on the real 252k-entry tar
        (data hashes were verified once at integration; headers are
        enough to catch classification drift and run in seconds)."""
        a = _inventory(REAL_TAR, "classic", with_data=False)
        b = _inventory(REAL_TAR, "libarchive", with_data=False)
        self.assertEqual(len(a), len(b))
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
