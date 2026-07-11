import unittest
import os
import sys

SFS_DIR = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(SFS_DIR)
ROOT = os.path.join(os.path.dirname(TEST_DIR), "src")
sys.path.insert(0, ROOT)

from amidisk.cli import cmd_cp, cmd_format
from amidisk.blkdev import ImageFileBlkDev
from amidisk.fs.sfs import SFSVolume

SRC_HDF = os.path.join(SFS_DIR, "src.hdf")
DST_HDF = os.path.join(SFS_DIR, "dst.hdf")

class MockArgs:
    pass

class TestCliCopy(unittest.TestCase):
    def setUp(self):
        # Create two images
        with open(SRC_HDF, "wb") as f:
            f.write(b'\x00' * (512 * 4096))
        with open(DST_HDF, "wb") as f:
            f.write(b'\x00' * (512 * 4096))
            
        args = MockArgs()
        args.image = SRC_HDF
        args.volume = None
        args.dostype = "sfs"
        args.label = "SRC"
        cmd_format(args)
        
        args.image = DST_HDF
        args.label = "DST"
        cmd_format(args)
        
        # Write file to SRC
        dev = ImageFileBlkDev(SRC_HDF, read_only=False)
        vol = SFSVolume(dev).open()
        vol.write_file("test.txt", b"Hello from SFS!")
        dev.close()
        
    def test_cp_sfs_to_sfs(self):
        args = MockArgs()
        args.src = SRC_HDF + ":DH0/test.txt"
        args.dst = DST_HDF + ":DH0/test_copied.txt"
        args.recursive = False
        args.checksum = False
        
        self.assertEqual(cmd_cp(args), 0)
        
        # Verify
        dev = ImageFileBlkDev(DST_HDF)
        vol = SFSVolume(dev).open()
        data = vol.read_file_bytes("test_copied.txt")
        self.assertEqual(data, b"Hello from SFS!")
        dev.close()
        
    def tearDown(self):
        try: os.remove(SRC_HDF)
        except: pass
        try: os.remove(DST_HDF)
        except: pass

if __name__ == "__main__":
    unittest.main()
