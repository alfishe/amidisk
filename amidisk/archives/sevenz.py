from .base import ArchiveHandler

class SevenZipHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        with open(path, "rb") as f:
            data = f.read(6)
            if data == b'7z\xbc\xaf\x27\x1c':
                return True
        return False
        
    def test_archive(self):
        raise NotImplementedError("7Z support is currently stubbed pending backend decision.")
            
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        raise NotImplementedError("7Z support is currently stubbed pending backend decision.")
