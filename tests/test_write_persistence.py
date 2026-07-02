import os
import pytest
from amidisk.blkdev import ImageFileBlkDev
from amidisk.fs.ffs import FFSVolume
from amidisk.fs.sfs import SFSVolume
from amidisk.fs.pfs3 import PFS3Volume
from amidisk.rdb.rdisk import RDisk

def check_nonzero_bytes(path):
    with open(path, "rb") as f:
        count = 0
        while True:
            buf = f.read(1024 * 1024)
            if not buf:
                break
            count += len(buf) - buf.count(b'\x00')
        return count

def test_ffs_persistence(tmp_path):
    path = str(tmp_path / "ffs.hdf")
    with open(path, "wb") as f:
        f.truncate(10 * 1024 * 1024)
    dev = ImageFileBlkDev(path, read_only=False)
    vol = FFSVolume(dev)
    vol.format(b"TestVol", dos_type=0x444F5303) # FFS Intl
    
    # Write 5MB
    payload = os.urandom(5 * 1024 * 1024)
    vol.write_file("bigfile.bin", payload)
    
    dev.close()
    
    nz = check_nonzero_bytes(path)
    assert nz >= 4.5 * 1024 * 1024, "FFS failed to properly change file on disk!"

def test_sfs_persistence(tmp_path):
    path = str(tmp_path / "sfs.hdf")
    with open(path, "wb") as f:
        f.truncate(10 * 1024 * 1024)
    dev = ImageFileBlkDev(path, read_only=False)
    vol = SFSVolume(dev)
    vol.format(b"TestVol", dos_type=0x53465300)
    
    payload = os.urandom(5 * 1024 * 1024)
    vol.write_file("bigfile.bin", payload)
    
    dev.close()
    
    nz = check_nonzero_bytes(path)
    assert nz >= 4.5 * 1024 * 1024, "SFS failed to properly change file on disk!"

def test_pfs3_persistence(tmp_path):
    path = str(tmp_path / "pfs.hdf")
    with open(path, "wb") as f:
        f.truncate(10 * 1024 * 1024)
    dev = ImageFileBlkDev(path, read_only=False)
    vol = PFS3Volume(dev)
    vol.format(b"TestVol", dos_type=0x50465303)
    
    payload = os.urandom(5 * 1024 * 1024)
    vol.write_file("bigfile.bin", payload)
    
    dev.close()
    
    nz = check_nonzero_bytes(path)
    assert nz >= 4.5 * 1024 * 1024, "PFS3 failed to properly change file on disk!"
