import unittest
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)

from amidisk.cli import cmd_cp, cmd_format
from amidisk.blkdev import ImageFileBlkDev
from amidisk.fs.sfs import SFSVolume

class MockArgs:
    pass

class TestCliCopy(unittest.TestCase):
    def setUp(self):
        # Create two images
        with open("tests/sfs/src.hdf", "wb") as f:
            f.write(b'\x00' * (512 * 4096))
        with open("tests/sfs/dst.hdf", "wb") as f:
            f.write(b'\x00' * (512 * 4096))
            
        args = MockArgs()
        args.image = "tests/sfs/src.hdf"
        args.volume = None
        args.dostype = "sfs"
        args.label = "SRC"
        cmd_format(args)
        
        args.image = "tests/sfs/dst.hdf"
        args.label = "DST"
        cmd_format(args)
        
        # Write file to SRC
        dev = ImageFileBlkDev("tests/sfs/src.hdf", read_only=False)
        vol = SFSVolume(dev).open()
        vol.write_file("test.txt", b"Hello from SFS!")
        dev.close()
        
    def test_cp_sfs_to_sfs(self):
        args = MockArgs()
        args.src = "tests/sfs/src.hdf:DH0/test.txt"
        args.dst = "tests/sfs/dst.hdf:DH0/test_copied.txt"
        args.recursive = False
        args.checksum = False
        
        self.assertEqual(cmd_cp(args), 0)
        
        # Verify
        dev = ImageFileBlkDev("tests/sfs/dst.hdf")
        vol = SFSVolume(dev).open()
        data = vol.read_file_bytes("test_copied.txt")
        self.assertEqual(data, b"Hello from SFS!")
        dev.close()
        
    def tearDown(self):
        try: os.remove("tests/sfs/src.hdf")
        except: pass
        try: os.remove("tests/sfs/dst.hdf")
        except: pass

if __name__ == "__main__":
    unittest.main()
