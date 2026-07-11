import unittest
import os
import sys

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(os.path.dirname(TEST_DIR), "src")
sys.path.insert(0, ROOT)

from amidisk.cli import cmd_create, cmd_rdb_init, cmd_part_add, cmd_format
from amidisk import open_image

class MockArgs:
    def __init__(self, **kwargs):
        for k, v in kwargs.items():
            setattr(self, k, v)
    
    def __getattr__(self, name):
        return None

class TestCliFormat(unittest.TestCase):
    def test_format_all_fs(self):
        # We test OFS, FFS, OFS-INTL, FFS-INTL, SFS, PFS3
        fs_types = ["ofs", "ffs", "ofs-intl", "ffs-intl", "sfs", "pfs3"]
        
        for fs in fs_types:
            with self.subTest(fs=fs):
                img_path = os.path.join(TEST_DIR, f"scratch_format_{fs}.hdf")
                if os.path.exists(img_path):
                    os.remove(img_path)
                
                # 1. Create 2MB image
                args = MockArgs(image=img_path, size="2M", force=True, adf=False, format=None)
                self.assertEqual(cmd_create(args), 0)
                
                # 2. Init RDB
                args = MockArgs(image=img_path, force=True, sectors=17, heads=2, rdb_cyls=1)
                self.assertEqual(cmd_rdb_init(args), 0)
                
                # 3. Add Partition
                args = MockArgs(image=img_path, start=None, end=None, dostype=fs, name="DH0", bootable=False, buffers=30, sec_per_blk=1, bs=512, pri=0)
                self.assertEqual(cmd_part_add(args), 0)
                
                # 4. Format Partition
                args = MockArgs(image=img_path, volume="DH0", label=f"TEST_{fs.upper()}", dostype=fs)
                self.assertEqual(cmd_format(args), 0)
                
                # 5. Verify Mount and Label
                with open_image(img_path) as img:
                    vol_ref = img.get_volume("DH0")
                    vol = vol_ref.mount()
                    info = vol.get_info()
                    self.assertEqual(info["label"], f"TEST_{fs.upper()}")
                
                os.remove(img_path)

if __name__ == "__main__":
    unittest.main()
