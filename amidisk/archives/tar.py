import sys
import tarfile
from datetime import datetime
from .base import ArchiveHandler

class TarHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        return tarfile.is_tarfile(path)
        
    def test_archive(self):
        print("verifying archive integrity: %s..." % self.path)
        try:
            with tarfile.open(self.path, "r") as tar:
                for _ in tar:
                    pass
            return True
        except tarfile.TarError as e:
            print("error: archive verification failed: %s" % e, file=sys.stderr)
            return False
            
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        from ..cli import print_progress  # Late import to avoid circular dependencies
        import os
        
        n = 0
        dir_count = 0
        total_bytes = 0
        last_flush_bytes = 0
        total_archive_size = os.path.getsize(self.path)
        
        try:
            with tarfile.open(self.path, "r") as tar:
                for member in tar:
                    name = member.name
                    if name.startswith("./"):
                        name = name[2:]
                    elif name == ".":
                        continue
                        
                    if not name:
                        continue
                        
                    parts = [truncate_func(p, max_len) for p in name.split("/")]
                    rel_amiga = "/".join(parts)
                    
                    amiga_dir = base_amiga_path if rel_amiga == "." else (
                        (base_amiga_path + "/" if base_amiga_path else "") + rel_amiga
                    )
                    
                    if member.isdir():
                        if amiga_dir:
                            vol.makedirs(amiga_dir)
                            dir_count += 1
                    elif member.isfile():
                        parent_dir = "/".join(amiga_dir.split("/")[:-1])
                        if parent_dir:
                            vol.makedirs(parent_dir)
                            
                        fh = tar.extractfile(member)
                        if fh is not None:
                            mtime = datetime.fromtimestamp(member.mtime)
                            vol.write_file(
                                amiga_dir, fh, size=member.size, 
                                protect=protect, comment=comment, mtime=mtime
                            )
                            n += 1
                            total_bytes += member.size
                            if (total_bytes - last_flush_bytes) > 50_000_000:
                                if hasattr(vol, "bitmap"):
                                    vol.bitmap.flush()
                                vol.dev.flush()
                                last_flush_bytes = total_bytes
                    # Update progress using the underlying tarfile read cursor
                    if tar.fileobj:
                        print_progress(tar.fileobj.tell(), total_archive_size, parts[-1])
                        
        except Exception as e:
            print("") # lock progress bar before error message
            print("error: streaming interrupted: %s" % e, file=sys.stderr)
            raise
            
        print("") # lock progress bar
        return n, dir_count, total_bytes
