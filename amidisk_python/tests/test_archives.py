import os
import shutil
import sys
import unittest

SCRATCH_BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scratch")
import tempfile
from datetime import datetime

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(os.path.dirname(TEST_DIR), "src")
REPO_ROOT = os.path.dirname(os.path.dirname(TEST_DIR))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from amidisk.archives.lha import LhaHandler
from amidisk.archives.sevenz import SevenZipHandler
from amidisk.archives.rar import RarHandler
from amidisk.archives.base import find_executable
from amidisk.blkdev import ImageFileBlkDev
from amidisk.fs.ffs import FFSVolume

REAL_LHA = os.path.join(TEST_DIR, "data", "pfs3aio.lha")
REAL_7Z = os.path.join(REPO_ROOT, "test.7z")

class TestSubprocessArchives(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.join(SCRATCH_BASE, self.__class__.__name__)
        os.makedirs(self.tmp, exist_ok=True)
        self.addCleanup(shutil.rmtree, self.tmp, True)
        
    def fresh_volume(self, name):
        p = os.path.join(self.tmp, name + ".hdf")
        with open(p, "wb") as fh:
            fh.truncate(4 * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        self.addCleanup(dev.close)
        vol = FFSVolume(dev, dos_type=0x444F5303)
        vol.format(b"TestVol", dos_type=0x444F5303)
        return vol.open()
        
    def test_lha_can_handle(self):
        if os.path.exists(REAL_LHA):
            self.assertTrue(LhaHandler.can_handle(REAL_LHA))
        self.assertFalse(LhaHandler.can_handle(__file__))
        
    def test_sevenz_can_handle(self):
        if os.path.exists(REAL_7Z):
            self.assertTrue(SevenZipHandler.can_handle(REAL_7Z))
        self.assertFalse(SevenZipHandler.can_handle(__file__))

    @unittest.skipUnless(find_executable(["7z", "7za"]), "7z tool not found")
    def test_sevenz_test_and_stream(self):
        if not os.path.exists(REAL_7Z):
            # Create synthetic 7z
            import subprocess
            a_txt = os.path.join(self.tmp, "a.txt")
            with open(a_txt, "w") as f:
                f.write("hello 7z")
            exe = find_executable(["7z", "7za"])
            archive = os.path.join(self.tmp, "synth.7z")
            subprocess.run([exe, "a", archive, a_txt], capture_output=True)
        else:
            archive = REAL_7Z
            
        handler = SevenZipHandler(archive)
        self.assertTrue(handler.test_archive())
        
        vol = self.fresh_volume("sevenz_vol")
        def trunc(name, max_len):
            return name[:max_len]
            
        n, dirs, size = handler.stream_to_volume(
            vol, base_amiga_path="", truncate_func=trunc,
            max_len=30, protect=0, comment=b""
        )
        self.assertTrue(n > 0)
        self.assertTrue(size > 0)
        
    @unittest.skipUnless(find_executable(["lha"]), "LHA tool not found")
    def test_lha_test_and_stream(self):
        if not os.path.exists(REAL_LHA):
            self.skipTest("pfs3aio.lha fixture not available")
            
        handler = LhaHandler(REAL_LHA)
        self.assertTrue(handler.test_archive())
        
        vol = self.fresh_volume("lha_vol")
        def trunc(name, max_len):
            return name[:max_len]
            
        n, dirs, size = handler.stream_to_volume(
            vol, base_amiga_path="", truncate_func=trunc,
            max_len=30, protect=0, comment=b""
        )
        self.assertEqual(n, 2)
        self.assertEqual(size, 118336)
        self.assertEqual(vol.read_file_bytes("pfs3aio")[:4], b"\x00\x00\x03\xf3") # Amiga hunk executable signature

if __name__ == "__main__":
    unittest.main()
