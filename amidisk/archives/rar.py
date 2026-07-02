from .base import ArchiveHandler

class RarHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        with open(path, "rb") as f:
            data = f.read(7)
            if data in (b'Rar!\x1a\x07\x00', b'Rar!\x1a\x07\x01'):
                return True
        return False
        
    def test_archive(self):
        raise NotImplementedError("RAR support is currently stubbed pending backend decision.")
            
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        raise NotImplementedError("RAR support is currently stubbed pending backend decision.")
