import os
import shutil
import sys
import unittest

SCRATCH_BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scratch")
import tempfile
import subprocess
from datetime import datetime

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(os.path.dirname(TEST_DIR), "src")
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from amidisk import open_image
from amidisk.blkdev import ImageFileBlkDev
from amidisk.fs.ffs import FFSVolume

class TestSyncTool(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.join(SCRATCH_BASE, self.__class__.__name__)
        os.makedirs(self.tmp, exist_ok=True)
        self.addCleanup(shutil.rmtree, self.tmp, True)
        
    def fresh_volume(self, name):
        p = os.path.join(self.tmp, name + ".hdf")
        with open(p, "wb") as fh:
            fh.truncate(10 * 1024 * 1024)
        dev = ImageFileBlkDev(p, read_only=False)
        self.addCleanup(dev.close)
        vol = FFSVolume(dev, dos_type=0x444F5303)
        vol.format(b"Workbench", dos_type=0x444F5303)
        return p, vol.open()
        
    def run_cli(self, *args):
        r = subprocess.run(
            [sys.executable, "-m", "amidisk.cli", *args],
            capture_output=True, text=True, cwd=ROOT
        )
        return r.returncode, r.stdout, r.stderr

    def test_sync_host_to_image(self):
        p, vol = self.fresh_volume("img_dest")
        
        # Create source directory on host
        src_dir = os.path.join(self.tmp, "src_host")
        os.makedirs(src_dir)
        
        with open(os.path.join(src_dir, "file1.txt"), "w") as f:
            f.write("hello world")
        os.makedirs(os.path.join(src_dir, "subdir"))
        with open(os.path.join(src_dir, "subdir", "file2.txt"), "w") as f:
            f.write("nested content")
            
        # Run sync command
        ret, stdout, stderr = self.run_cli("sync", src_dir, f"{p}:Workbench")
        self.assertEqual(ret, 0, stderr)
        
        # Re-open volume to get fresh disk contents
        dev2 = ImageFileBlkDev(p, read_only=True)
        self.addCleanup(dev2.close)
        vol2 = FFSVolume(dev2, dos_type=0x444F5303).open()
        
        self.assertEqual(vol2.read_file_bytes("file1.txt"), b"hello world")
        self.assertEqual(vol2.read_file_bytes("subdir/file2.txt"), b"nested content")
        
        # Update a file on host and add a new one
        with open(os.path.join(src_dir, "file1.txt"), "w") as f:
            f.write("updated hello world")
        with open(os.path.join(src_dir, "file3.txt"), "w") as f:
            f.write("new file")
            
        # Run sync again
        ret, stdout, stderr = self.run_cli("sync", src_dir, f"{p}:Workbench")
        self.assertEqual(ret, 0, stderr)
        
        # Re-open volume to verify updates
        dev3 = ImageFileBlkDev(p, read_only=True)
        self.addCleanup(dev3.close)
        vol3 = FFSVolume(dev3, dos_type=0x444F5303).open()
        
        self.assertEqual(vol3.read_file_bytes("file1.txt"), b"updated hello world")
        self.assertEqual(vol3.read_file_bytes("file3.txt"), b"new file")
        
    def test_sync_host_to_image_delete(self):
        p, vol = self.fresh_volume("img_dest_del")
        
        # Create source directory on host
        src_dir = os.path.join(self.tmp, "src_host")
        os.makedirs(src_dir)
        with open(os.path.join(src_dir, "keep.txt"), "w") as f:
            f.write("keep me")
            
        # Manually create a file in the destination image that should be deleted
        vol.write_file("delete_me.txt", b"extraneous")
        vol.bitmap.flush()
        vol.dev.flush()
        
        # Run sync with --delete
        ret, stdout, stderr = self.run_cli("sync", src_dir, f"{p}:Workbench", "--delete")
        self.assertEqual(ret, 0, stderr)
        
        # Re-open volume to verify
        dev2 = ImageFileBlkDev(p, read_only=True)
        self.addCleanup(dev2.close)
        vol2 = FFSVolume(dev2, dos_type=0x444F5303).open()
        
        self.assertEqual(vol2.read_file_bytes("keep.txt"), b"keep me")
        with self.assertRaises(Exception):
            vol2.read_file_bytes("delete_me.txt")
            
    def test_sync_image_to_host(self):
        p, vol = self.fresh_volume("img_src")
        
        # Populate source image
        vol.write_file("file1.txt", b"hello world")
        vol.makedirs("subdir")
        vol.write_file("subdir/file2.txt", b"nested content")
        vol.bitmap.flush()
        vol.dev.flush()
        
        # Destination directory on host
        dst_dir = os.path.join(self.tmp, "dst_host")
        
        # Run sync
        ret, stdout, stderr = self.run_cli("sync", f"{p}:Workbench", dst_dir)
        self.assertEqual(ret, 0, stderr)
        
        # Verify host
        with open(os.path.join(dst_dir, "file1.txt"), "rb") as f:
            self.assertEqual(f.read(), b"hello world")
        with open(os.path.join(dst_dir, "subdir", "file2.txt"), "rb") as f:
            self.assertEqual(f.read(), b"nested content")

if __name__ == "__main__":
    unittest.main()
