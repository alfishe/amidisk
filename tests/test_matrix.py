"""The support-matrix guarantee: every supported dostype must
format + write + read + rename + delete + deep-check, and anything
else must at least be identified.
"""

import os
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from amidisk import open_image                       # noqa: E402
from amidisk.blkdev import ImageFileBlkDev           # noqa: E402
from amidisk.image import identify_volume            # noqa: E402
from amidisk.fs.ffs import FFSVolume                 # noqa: E402
from amidisk.fs.pfs3 import PFS3Volume               # noqa: E402
from amidisk.fs.sfs import SFSVolume                 # noqa: E402
from amidisk.rdb import RDisk                        # noqa: E402

# every dostype the README support matrix claims as read+write+format
MATRIX = [
    (0x444F5300, FFSVolume, "OFS DOS\\0"),
    (0x444F5301, FFSVolume, "FFS DOS\\1"),
    (0x444F5302, FFSVolume, "OFS-INTL DOS\\2"),
    (0x444F5303, FFSVolume, "FFS-INTL DOS\\3"),
    (0x444F5304, FFSVolume, "OFS-DC DOS\\4"),
    (0x444F5305, FFSVolume, "FFS-DC DOS\\5"),
    (0x444F5306, FFSVolume, "OFS-LNFS DOS\\6"),
    (0x444F5307, FFSVolume, "FFS-LNFS DOS\\7"),
    (0x50465303, PFS3Volume, "PFS\\3"),
    (0x50445303, PFS3Volume, "PDS\\3"),
    (0x53465300, SFSVolume, "SFS\\0"),
]


class TestSupportedMatrix(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="amidisk-matrix-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_format_write_read_all(self):
        for dt, engine, tag in MATRIX:
            with self.subTest(tag):
                p = os.path.join(self.tmp, "m%08x.hdf" % dt)
                with open(p, "wb") as fh:
                    fh.truncate(8 * 1024 * 1024)
                dev = ImageFileBlkDev(p, read_only=False)
                try:
                    fmt_label = b"Vol"
                    engine(dev, dos_type=dt).format(fmt_label, dos_type=dt)
                    vol = engine(dev, dos_type=dt).open()
                    self.assertFalse(vol.read_only, tag)
                    data = os.urandom(120_000)
                    vol.makedirs("A/B")
                    vol.write_file("A/B/x.bin", data, comment=b"m")
                    self.assertEqual(vol.read_file_bytes("A/B/x.bin"), data, tag)
                    vol.rename("A/B/x.bin", "A/y.bin")
                    self.assertEqual(vol.read_file_bytes("A/y.bin"), data, tag)
                    vol.delete("A", recursive=True)
                    rep = vol.check(deep=True)
                    self.assertTrue(rep["ok"], (tag, rep["errors"][:4]))
                finally:
                    dev.close()

    def test_matrix_inside_rdb_partitions(self):
        """One representative of each engine via the full RDB stack."""
        p = os.path.join(self.tmp, "rdb.hdf")
        with open(p, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        rd = RDisk.create(dev)
        rd.add_partition("DH0", size_bytes=12 << 20, dos_type=0x444F5307)
        rd.add_partition("DH1", size_bytes=12 << 20, dos_type=0x50445303)
        rd.add_partition("DH2", size_bytes=12 << 20, dos_type=0x53465300)
        dev.close()
        with open_image(p, writable=True) as img:
            for name in ("DH0", "DH1", "DH2"):
                v = img.get_volume(name)
                v.raw_volume().format(b"T" + name.encode())
            for name in ("DH0", "DH1", "DH2"):
                vol = img.get_volume(name).mount()
                vol.write_file("f.bin", b"matrix" * 1000)
                self.assertEqual(vol.read_file_bytes("f.bin"), b"matrix" * 1000)
                self.assertTrue(vol.check(deep=True)["ok"], name)


class TestUnknownIdentification(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="amidisk-ident-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def dev_with_block0(self, block0, size_mb=2):
        p = os.path.join(self.tmp, "u%d.hdf" % len(block0))
        with open(p, "wb") as fh:
            fh.truncate(size_mb * 1024 * 1024)
            fh.seek(0)
            fh.write(block0)
        d = ImageFileBlkDev(p)
        self.addCleanup(d.close)
        return d

    def test_ndos(self):
        ident = identify_volume(self.dev_with_block0(b"NDOS" + b"\x00" * 508))
        self.assertTrue(any("NDOS" in g for g in ident["guesses"]))

    def test_fat16(self):
        b = bytearray(512)
        b[0:3] = b"\xeb<\x90"
        b[54:59] = b"FAT16"
        b[510:512] = b"\x55\xaa"
        ident = identify_volume(self.dev_with_block0(bytes(b)))
        self.assertTrue(any("FAT12/16" in g for g in ident["guesses"]))

    def test_blank(self):
        ident = identify_volume(self.dev_with_block0(b"\x00" * 512))
        self.assertTrue(any("unformatted" in g for g in ident["guesses"]))

    def test_declared_dostype_reported(self):
        ident = identify_volume(
            self.dev_with_block0(b"\x00" * 512), dos_type=0x43443031
        )
        self.assertEqual(ident["declared_dos_type"], "CD0\\31")

    def test_damaged_bootblock_finds_root(self):
        # format FFS, then wipe the bootblock: identify should still spot
        # the valid root block at the midpoint
        p = os.path.join(self.tmp, "damaged.hdf")
        with open(p, "wb") as fh:
            fh.truncate(4 * 1024 * 1024)
        d = ImageFileBlkDev(p, read_only=False)
        FFSVolume(d).format(b"Damaged", dos_type=0x444F5303)
        d.write(0, b"\xde\xad\xbe\xef" + b"\x00" * 508)
        ident = identify_volume(d)
        d.close()
        self.assertTrue(
            any("root block at the midpoint" in g for g in ident["guesses"]),
            ident["guesses"],
        )

    def test_unknown_partition_in_info(self):
        """`info` must degrade to identification, not just an error."""
        import subprocess

        p = os.path.join(self.tmp, "mixed.hdf")
        for cmd in (
            ["create", p, "--size", "16M"],
            ["rdb-init", p],
            ["part-add", p, "DH0", "--size", "4M", "--dostype", "0x43444653"],
        ):
            r = subprocess.run(
                [sys.executable, "-m", "amidisk.cli", *cmd],
                capture_output=True, text=True, cwd=ROOT,
            )
            self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run(
            [sys.executable, "-m", "amidisk.cli", "info", p],
            capture_output=True, text=True, cwd=ROOT,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("unmountable", r.stdout)
        # dos_type_to_str renders 0x43444653 ('CDFS') as CDF\53
        self.assertIn("declared type CDF", r.stdout)
        self.assertIn("detected:", r.stdout)


if __name__ == "__main__":
    unittest.main()
