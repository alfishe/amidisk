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
