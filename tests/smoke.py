#!/usr/bin/env python3
"""amidisk smoke test suite.

Run from the project root:  python3 tests/smoke.py

Read-only tests run against data/ images when present; write tests run
on temporary copies/new images under a scratch dir and never touch the
originals.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from amidisk import open_image                      # noqa: E402
from amidisk.blkdev import ImageFileBlkDev          # noqa: E402
from amidisk.fs.ffs import FFSVolume                # noqa: E402
from amidisk.fs.pfs3 import PFS3Volume
from amidisk.fs.sfs import SFSVolume
from amidisk.rdb import RDisk                       # noqa: E402

DATA = os.path.join(ROOT, "data")
TESTDATA = os.path.join(DATA, "test")

PASS = FAIL = SKIP = 0


def check(name, fn):
    global PASS, FAIL
    try:
        fn()
        PASS += 1
        print("  ok   %s" % name)
    except Exception as ex:
        FAIL += 1
        print("  FAIL %s: %s" % (name, ex))


def skip(name, why):
    global SKIP
    SKIP += 1
    print("  skip %s (%s)" % (name, why))


def exists(*p):
    return os.path.exists(os.path.join(*p))


# ---------------------------------------------------------------- read-only
def test_user_images():
    print("== user images (read-only)")
    for name, vols in (("HARDFILE.hdf", 2), ("OS-3.2.3.vhd", 4)):
        path = os.path.join(DATA, name)
        if not os.path.exists(path):
            skip(name, "not present")
            continue

        def t(path=path, vols=vols):
            with open_image(path) as img:
                assert img.rdisk, "no RDB"
                assert len(img.volumes) == vols, "expected %d volumes" % vols
                for v in img.volumes:
                    info = v.mount().get_info()
                    assert info["label"], "unlabelled volume"

        check(name, t)
    path = os.path.join(DATA, "OS-3.2.3.vhd")
    if os.path.exists(path):
        def t_read():
            with open_image(path) as img:
                vol = img.get_volume("GDH0").mount()
                data = vol.read_file_bytes("S/startup-sequence")
                assert data.startswith(b";"), "startup-sequence should be script"
        check("OS-3.2.3.vhd: read S/startup-sequence", t_read)


def test_fixture_images():
    print("== fixture images (read-only)")
    if not os.path.isdir(TESTDATA):
        skip("data/test", "not present")
        return
    for name in sorted(os.listdir(TESTDATA)):
        if not name.endswith((".hdf", ".adf", ".vhd")):
            continue
        path = os.path.join(TESTDATA, name)

        def t(path=path):
            with open_image(path) as img:
                assert img.volumes, "no volumes"
                for v in img.volumes:
                    vol = v.mount()
                    rep = vol.check()
                    assert rep["ok"], rep["errors"][:3]

        check(name, t)

    # known content in real handler-written volumes
    if exists(TESTDATA, "pfs3-real.hdf"):
        def t_pfs():
            with open_image(os.path.join(TESTDATA, "pfs3-real.hdf")) as img:
                vol = img.volumes[0].mount()
                data = vol.read_file_bytes("MMULib/MuTools/MuForce.guide")
                assert data.startswith(b"@DATABASE"), data[:20]
        check("pfs3-real.hdf: file content", t_pfs)
    if exists(TESTDATA, "sfs-real.hdf"):
        def t_sfs():
            with open_image(os.path.join(TESTDATA, "sfs-real.hdf")) as img:
                vol = img.volumes[0].mount()
                names = [e.name_str() for e in vol.list_dir("")]
                assert "Prefs" in names and "System" in names, names[:8]
        check("sfs-real.hdf: catalog", t_sfs)


# ---------------------------------------------------------------- write tests
def test_ffs_write(tmp):
    print("== FFS write (all dostypes)")
    for flavor in range(8):
        path = os.path.join(tmp, "dos%d.hdf" % flavor)

        def t(path=path, flavor=flavor):
            with open(path, "wb") as fh:
                fh.truncate(16 * 1024 * 1024)
            dev = ImageFileBlkDev(path, read_only=False)
            vol = FFSVolume(dev)
            vol.format(b"Vol%d" % flavor, dos_type=0x444F5300 + flavor)
            vol = FFSVolume(dev).open()
            vol.makedirs("A/B")
            payload = os.urandom(300_000)
            vol.write_file("A/B/blob.bin", payload, comment=b"hi")
            vol.write_file("A/B/empty", b"")
            assert vol.read_file_bytes("A/B/blob.bin") == payload
            vol.rename("A/B/blob.bin", "A/moved.bin")
            vol.delete("A/B/empty")
            rep = vol.check(deep=True)
            assert rep["ok"], rep["errors"][:3]
            vol.delete("A", recursive=True)
            rep = vol.check(deep=True)
            assert rep["ok"] and not rep["warnings"]
            dev.close()

        check("DOS\\%d round-trip" % flavor, t)


def test_rdb_lifecycle(tmp):
    print("== RDB lifecycle")
    path = os.path.join(tmp, "rdb.hdf")

    def t():
        with open(path, "wb") as fh:
            fh.truncate(100 * 1024 * 1024)
        dev = ImageFileBlkDev(path, read_only=False)
        rd = RDisk.create(dev)
        rd.add_partition("DH0", size_bytes=30 * 1024 * 1024, bootable=True)
        rd.add_partition("DH1", sec_per_blk=4)
        dev.close()
        with open_image(path, writable=True) as img:
            assert [v.name for v in img.volumes] == ["DH0", "DH1"]
            for v in img.volumes:
                vol = v.raw_volume()
                vol.format(b"T" + v.name.encode())
            vol = img.get_volume("DH1").mount()
            assert vol.bs == 2048, vol.bs
            vol.write_file("x.bin", b"abc" * 5000)
            assert vol.read_file_bytes("x.bin") == b"abc" * 5000
            img.rdisk.delete_partition("DH0")
        with open_image(path) as img:
            assert [v.name for v in img.volumes] == ["DH1"]
            rep = img.get_volume("DH1").mount().check(deep=True)
            assert rep["ok"]

    check("create/add/format/write/delete", t)


def test_repair(tmp):
    print("== repair")
    path = os.path.join(tmp, "repair.hdf")

    def t():
        with open(path, "wb") as fh:
            fh.truncate(16 * 1024 * 1024)
        dev = ImageFileBlkDev(path, read_only=False)
        vol = FFSVolume(dev)
        vol.format(b"Fix", dos_type=0x444F5303)
        vol = FFSVolume(dev).open()
        vol.write_file("f.bin", os.urandom(100_000))
        # corrupt: mark everything free
        idx = vol.root_blk - vol.reserved
        page = idx // vol.bitmap.bits_per_page
        pg = vol.bitmap.pages[page]
        for li in range(1, vol.nl):
            pg.put_long(li, 0xFFFFFFFF)
        pg.fix_checksum(0)
        vol.write_buf(vol.bitmap.page_blks[page], pg)
        vol = FFSVolume(dev).open()
        rep = vol.repair(apply=True)
        assert rep["applied"] and rep["alloc_missing"] > 0
        rep = vol.check(deep=True)
        assert rep["ok"] and not rep["warnings"]
        dev.close()

    check("bitmap rebuild", t)


def test_rescue(tmp):
    print("== RDB rescue")
    src = os.path.join(TESTDATA, "rdb-amidisk.hdf")
    if not os.path.exists(src):
        skip("rescue", "fixture missing")
        return
    path = os.path.join(tmp, "rescue.hdf")

    def t():
        shutil.copy(src, path)
        with open(path, "r+b") as fh:  # wipe the whole RDB area
            fh.write(b"\x00" * 512 * 16)
        from amidisk.blkdev import open_blkdev, OverlayBlockDevice
        from amidisk.rdb import rescue

        dev = open_blkdev(path)
        cands, _ = rescue.scan(dev)
        assert len(cands) == 2, [c.describe() for c in cands]
        overlay = OverlayBlockDevice(dev)
        rescue.rebuild(overlay, cands)
        # simulation must yield mountable, clean volumes
        from amidisk.image import DiskImage

        sim = DiskImage(path, writable=True, blkdev=overlay)
        assert len(sim.volumes) == 2
        for v in sim.volumes:
            rep = v.mount().check()
            assert rep["ok"], rep["errors"][:3]
        # base image untouched so far
        with open(path, "rb") as fh:
            assert fh.read(4) != b"RDSK"
        # commit and re-verify on the real image
        target = open_blkdev(path, read_only=False)
        overlay.commit(target)
        assert not overlay.verify_committed(target)
        target.close()
        dev.close()
        with open_image(path) as img:
            for v in img.volumes:
                assert v.mount().check()["ok"]

    check("wipe RDB area, scan, simulate, commit", t)


def test_cli(tmp):
    print("== CLI")
    adf = os.path.join(tmp, "cli.adf")
    payload = os.path.join(tmp, "payload.bin")
    with open(payload, "wb") as fh:
        fh.write(os.urandom(50_000))

    def run(*args):
        r = subprocess.run(
            [sys.executable, "-m", "amidisk.cli", *args],
            capture_output=True, text=True, cwd=ROOT,
        )
        if r.returncode != 0:
            raise AssertionError("cli %s failed: %s" % (args[0], r.stderr.strip()))
        return r.stdout

    def t():
        run("create", adf, "--adf", "--format", "CliVol", "--dostype", "ffs-intl")
        run("mkdir", adf, "Dir")
        run("put", adf, payload, "Dir/p.bin")
        out = run("ls", adf, "Dir")
        assert "p.bin" in out
        back = os.path.join(tmp, "back.bin")
        run("extract", adf, "Dir/p.bin", back)
        assert open(back, "rb").read() == open(payload, "rb").read()
        run("mv", adf, "Dir/p.bin", "renamed.bin")
        run("rm", adf, "Dir")
        out = run("check", adf)
        assert "OK" in out

    check("create/put/ls/extract/mv/rm/check", t)


def test_streaming(tmp):
    print("== streaming operations")
    src_hdf = os.path.join(tmp, "src_stream.hdf")
    dst_hdf = os.path.join(tmp, "dst_stream.hdf")
    payload = os.path.join(tmp, "stream_payload.bin")
    
    with open(payload, "wb") as fh:
        fh.write(os.urandom(200_000))
        
    def run(*args):
        r = subprocess.run(
            [sys.executable, "-m", "amidisk.cli", *args],
            capture_output=True, text=True, cwd=ROOT,
        )
        if r.returncode != 0:
            raise AssertionError("cli %s failed: %s" % (args[0], r.stderr.strip()))
        return r.stdout

    def t():
        run("create", src_hdf, "--format", "Src", "--dostype", "ffs", "--size", "1M")
        run("create", dst_hdf, "--format", "Dst", "--dostype", "ffs-intl", "--size", "1M")
        
        run("put", src_hdf, payload, "file.bin")
        out = run("cp", src_hdf, "file.bin", dst_hdf, "copied.bin", "--checksum")
        assert "md5:" in out
        
        out_file = os.path.join(tmp, "verify.bin")
        run("extract", dst_hdf, "copied.bin", out_file)
        
        assert open(out_file, "rb").read() == open(payload, "rb").read()

    check("stream copy with checksum between images", t)
    
    sfs_real = os.path.join(TESTDATA, "sfs-real.hdf")
    if os.path.exists(sfs_real):
        def t_sfs_stream():
            # Create a destination image to stream into
            run("create", dst_hdf, "--format", "Dst", "--dostype", "ffs", "--size", "1M", "--force")
            
            sfs_file = "Prefs/Env-Archive/deficons.prefs"
            run("cp", sfs_real, sfs_file, dst_hdf, "streamed.prefs", "--checksum")
            
            # verify
            out_file = os.path.join(tmp, "verify_sfs.bin")
            run("extract", dst_hdf, "streamed.prefs", out_file)
            run("extract", sfs_real, sfs_file, os.path.join(tmp, "sfs_orig.bin"))
            assert open(out_file, "rb").read() == open(os.path.join(tmp, "sfs_orig.bin"), "rb").read()
            
        check("stream copy from SFS image", t_sfs_stream)


def test_sfs_format(tmp):
    print("== SFS format")
    path = os.path.join(tmp, "sfs_format.hdf")
    
    def t():
        with open(path, "wb") as fh:
            fh.truncate(4 * 1024 * 1024)
        dev = ImageFileBlkDev(path, read_only=False)
        try:
            vol = SFSVolume(dev)
            vol.format("SFSTest")
            info = vol.get_info()
            assert info["filesystem"] == "SFS"
            assert info["label"] == "SFSTest"
            entries = vol.list_dir("")
            assert len(entries) == 1
            assert entries[0].name_str() == ".recycled"
            assert entries[0].is_dir()
        finally:
            dev.close()
            
    check("format SFS and verify", t)


def main():
    tmp = tempfile.mkdtemp(prefix="amidisk-test-")
    try:
        test_user_images()
        test_fixture_images()
        test_ffs_write(tmp)
        test_rdb_lifecycle(tmp)
        test_repair(tmp)
        test_rescue(tmp)
        test_cli(tmp)
        test_streaming(tmp)
        test_sfs_format(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed, %d skipped" % (PASS, FAIL, SKIP))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
