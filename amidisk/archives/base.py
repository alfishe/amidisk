import os
import shutil

class ArchiveHandler:
    """Base class for streaming archive contents into an Amiga volume."""
    
    @classmethod
    def can_handle(cls, path):
        """Returns True if this handler can process the given file."""
        return False
        
    def __init__(self, path):
        self.path = path
        
    def test_archive(self):
        """Perform a quick integrity check on the archive before streaming.
        Should raise an exception or return False if corrupt."""
        raise NotImplementedError()
        
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        """Iterates over archive members and streams them into the volume.
        Returns (num_files_streamed, total_bytes_streamed)."""
        raise NotImplementedError()


def find_executable(names):
    """Finds an executable in PATH or common search paths."""
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    for name in names:
        for loc in ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"]:
            path = os.path.join(loc, name)
            if os.path.isfile(path) and os.access(path, os.X_OK):
                return path
    return None


def read_chunks(fh, chunk_size=65536):
    """Reads binary chunks from a file-like object to prevent line-by-line iteration overhead."""
    while True:
        chunk = fh.read(chunk_size)
        if not chunk:
            break
        yield chunk
