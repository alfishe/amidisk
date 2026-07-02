from .base import ArchiveHandler

class LhaHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        # We look for common lha signatures
        with open(path, "rb") as f:
            data = f.read(21)
            if len(data) >= 21 and data[2:7] in (b'-lh0-', b'-lh1-', b'-lh2-', b'-lh3-', b'-lh4-', b'-lh5-', b'-lh6-', b'-lh7-', b'-lhd-'):
                return True
        return False
        
    def test_archive(self):
        raise NotImplementedError("LHA support is currently stubbed pending backend decision.")
            
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        raise NotImplementedError("LHA support is currently stubbed pending backend decision.")
