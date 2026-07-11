"""Streaming tar readers for bulk imports.

Two interchangeable engines behind one interface:

  ClassicTarReader     -- pure-Python tarfile, works everywhere
  LibarchiveTarReader  -- ctypes binding to the system libarchive
                          (~6x faster parsing; ships with macOS, and
                          present on most Linux systems)

open_tar(path) picks libarchive when the library loads and falls back
to classic -- the project stays dependency-free either way.

Usage:
    with open_tar(path) as rd:
        for entry in rd:            # entry is valid until the next step
            if entry.isfile:
                for chunk in entry.chunks():
                    ...
Unconsumed data is skipped automatically on the next iteration.
"""

import os
import struct

CHUNK = 1 << 20


class TarEntry:
    __slots__ = ("name", "kind", "size", "mtime", "linkname", "_reader", "_serial")

    def __init__(self, name, kind, size, mtime, linkname, reader, serial):
        self.name = name
        self.kind = kind          # 'file' | 'dir' | 'hardlink' | 'other'
        self.size = size
        self.mtime = mtime
        self.linkname = linkname  # hardlink/symlink target, if any
        self._reader = reader
        self._serial = serial

    @property
    def isfile(self):
        return self.kind == "file"

    @property
    def isdir(self):
        return self.kind == "dir"

    def chunks(self, chunk=CHUNK):
        """Iterate the entry's data. Only valid for the current entry."""
        return self._reader._chunks(self._serial, chunk)


class ClassicTarReader:
    """Pure-Python engine (tarfile in stream mode)."""

    engine = "classic"

    def __init__(self, path):
        import tarfile

        self._tf = tarfile.open(path, "r|")
        self._serial = 0
        self._cur_fh = None

    def __iter__(self):
        return self

    def __next__(self):
        m = self._tf.next()
        if m is None:
            raise StopIteration
        self._serial += 1
        self._cur_fh = None
        if m.isfile():
            kind = "file"
            self._cur_fh = self._tf.extractfile(m)
        elif m.isdir():
            kind = "dir"
        elif m.islnk():
            kind = "hardlink"
        else:
            kind = "other"
        return TarEntry(m.name, kind, m.size, m.mtime, m.linkname or None,
                        self, self._serial)

    def _chunks(self, serial, chunk):
        if serial != self._serial:
            raise RuntimeError("tar entry data read out of order")
        fh = self._cur_fh
        if fh is None:
            return
        while True:
            data = fh.read(chunk)
            if not data:
                return
            yield data

    def close(self):
        self._tf.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class LibarchiveError(Exception):
    pass


class LibarchiveTarReader:
    """ctypes binding to the system libarchive (no Python dependency)."""

    engine = "libarchive"
    _lib = None

    AE_IFMT = 0o170000
    AE_IFREG = 0o100000
    AE_IFDIR = 0o040000

    @classmethod
    def _load(cls):
        if cls._lib is not None:
            return cls._lib
        import ctypes
        import ctypes.util

        # prefer an upstream build: Apple's /usr/lib copy force-merges
        # AppleDouble ._* members into the preceding entry (mac-ext) and
        # ignores the option that is supposed to disable it
        path = None
        for cand in ("/opt/homebrew/opt/libarchive/lib/libarchive.dylib",
                     "/usr/local/opt/libarchive/lib/libarchive.dylib"):
            if os.path.exists(cand):
                path = cand
                break
        if path is None:
            path = ctypes.util.find_library("archive")
        if path is None and os.path.exists("/usr/lib/libarchive.dylib"):
            path = "/usr/lib/libarchive.dylib"
        if path is None:
            raise LibarchiveError("libarchive not found")
        la = ctypes.CDLL(path)
        la.archive_read_new.restype = ctypes.c_void_p
        la.archive_read_support_format_all.argtypes = [ctypes.c_void_p]
        la.archive_read_support_filter_all.argtypes = [ctypes.c_void_p]
        la.archive_read_set_options.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p]
        la.archive_read_open_filename.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
        la.archive_read_next_header.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
        la.archive_read_data.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        la.archive_read_data.restype = ctypes.c_ssize_t
        la.archive_read_free.argtypes = [ctypes.c_void_p]
        la.archive_error_string.argtypes = [ctypes.c_void_p]
        la.archive_error_string.restype = ctypes.c_char_p
        la.archive_entry_pathname.argtypes = [ctypes.c_void_p]
        la.archive_entry_pathname.restype = ctypes.c_char_p
        la.archive_entry_size.argtypes = [ctypes.c_void_p]
        la.archive_entry_size.restype = ctypes.c_longlong
        la.archive_entry_filetype.argtypes = [ctypes.c_void_p]
        la.archive_entry_filetype.restype = ctypes.c_uint
        la.archive_entry_mtime.argtypes = [ctypes.c_void_p]
        la.archive_entry_mtime.restype = ctypes.c_longlong
        la.archive_entry_hardlink.argtypes = [ctypes.c_void_p]
        la.archive_entry_hardlink.restype = ctypes.c_char_p
        la.archive_entry_symlink.argtypes = [ctypes.c_void_p]
        la.archive_entry_symlink.restype = ctypes.c_char_p
        la.archive_entry_mtime.argtypes = [ctypes.c_void_p]
        la.archive_entry_mtime.restype = ctypes.c_longlong
        cls._lib = la
        return la

    def __init__(self, path):
        import ctypes

        la = self._load()
        self._la = la
        self._ct = ctypes
        a = la.archive_read_new()
        la.archive_read_support_format_all(a)
        la.archive_read_support_filter_all(a)
        # keep AppleDouble ._* members as plain entries: macOS's build
        # otherwise merges them into the preceding file's metadata and
        # they vanish from the stream (classic reader shows them)
        la.archive_read_set_options(a, b"!mac-ext")
        if la.archive_read_open_filename(a, os.fsencode(path), 4 << 20) != 0:
            err = la.archive_error_string(a)
            la.archive_read_free(a)
            raise LibarchiveError(err or b"open failed")
        self._a = a
        self._buf = ctypes.create_string_buffer(CHUNK)
        self._serial = 0
        self._remaining = 0

    def __iter__(self):
        return self

    def __next__(self):
        la, ct = self._la, self._ct
        entry = ct.c_void_p()
        r = la.archive_read_next_header(self._a, ct.byref(entry))
        if r == 1:  # ARCHIVE_EOF
            raise StopIteration
        if r != 0:
            raise LibarchiveError(la.archive_error_string(self._a))
        self._serial += 1
        name = os.fsdecode(la.archive_entry_pathname(entry) or b"")
        size = la.archive_entry_size(entry)
        hardlink = la.archive_entry_hardlink(entry)
        ftype = la.archive_entry_filetype(entry) & self.AE_IFMT
        if hardlink:
            kind = "hardlink"
            link = os.fsdecode(hardlink)
        elif ftype == self.AE_IFDIR or name.endswith("/"):
            kind = "dir"
            link = None
        elif ftype == self.AE_IFREG:
            kind = "file"
            link = None
        else:
            sym = la.archive_entry_symlink(entry)
            kind = "other"
            link = os.fsdecode(sym) if sym else None
        mtime = la.archive_entry_mtime(entry)
        self._remaining = size if kind == "file" else 0
        return TarEntry(name.rstrip("/"), kind, size, mtime, link, self, self._serial)

    def _chunks(self, serial, chunk):
        if serial != self._serial:
            raise RuntimeError("tar entry data read out of order")
        la = self._la
        buf = self._buf
        want = min(chunk, CHUNK)
        while self._remaining > 0:
            n = la.archive_read_data(self._a, buf, want)
            if n < 0:
                raise LibarchiveError(la.archive_error_string(self._a))
            if n == 0:
                break
            self._remaining -= n
            yield buf[:n]  # ctypes slice copies n bytes; .raw copies ALL

    def close(self):
        if self._a is not None:
            self._la.archive_read_free(self._a)
            self._a = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def open_tar(path, engine="auto"):
    """Open a tar for streaming. engine: 'auto' | 'classic' | 'libarchive'."""
    if engine == "classic":
        return ClassicTarReader(path)
    if engine == "libarchive":
        return LibarchiveTarReader(path)
    try:
        return LibarchiveTarReader(path)
    except (LibarchiveError, OSError):
        return ClassicTarReader(path)
