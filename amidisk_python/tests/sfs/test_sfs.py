import unittest
import os
import sys

TEST_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.join(os.path.dirname(TEST_DIR), "src")
sys.path.insert(0, ROOT)

from amidisk.blkdev import BlockDevice
from amidisk.fs.sfs import SFSVolume
from amidisk.fs.ffs import FSError

class MemoryBlkDev(BlockDevice):
    def __init__(self, block_bytes, num_blocks):
        super().__init__()
        self.block_bytes = block_bytes
        self.num_blocks = num_blocks
        self.read_only = False
        self.data = bytearray(block_bytes * num_blocks)

    def read(self, lba, count=1):
        start = lba * self.block_bytes
        end = start + count * self.block_bytes
        return self.data[start:end]

    def write(self, lba, data):
        start = lba * self.block_bytes
        self.data[start:start+len(data)] = data

    def flush(self):
        pass

class TestSFS(unittest.TestCase):
    def setUp(self):
        self.dev = MemoryBlkDev(512, 4096)
        self.vol = SFSVolume(self.dev)
        self.vol.format("SFS_Test")

    def test_format(self):
        info = self.vol.get_info()
        self.assertEqual(info["label"], "SFS_Test")
        self.assertEqual(info["filesystem"], "SFS")

    def test_write_read_bytes(self):
        data = b"Hello from SFS file writing!"
        self.vol.write_file("test.txt", data)
        read_data = self.vol.read_file_bytes("test.txt")
        self.assertEqual(data, read_data)
        
    def test_write_read_chunked(self):
        chunks = [b"Chunk1 ", b"Chunk2 ", b"Chunk3 ", b"Chunk4!"]
        expected = b"".join(chunks)
        self.vol.write_file("chunked.txt", iter(chunks), size=len(expected))
        
        read_data = self.vol.read_file_bytes("chunked.txt")
        self.assertEqual(expected, read_data)

    def test_write_read_large(self):
        # Write a file larger than 1 block (e.g. 1500 bytes)
        data = os.urandom(1500)
        self.vol.write_file("large.bin", data)
        read_data = self.vol.read_file_bytes("large.bin")
        self.assertEqual(data, read_data)

if __name__ == "__main__":
    unittest.main()
