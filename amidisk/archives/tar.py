import sys
import tarfile
from datetime import datetime
from .base import ArchiveHandler

class TarHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        return tarfile.is_tarfile(path)
        
    def test_archive(self):
        from ..tarreader import open_tar, LibarchiveError
        print("verifying archive integrity: %s..." % self.path)
        try:
            with open_tar(self.path) as tar:
                for _ in tar:
                    pass
            return True
        except Exception as e:
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
        
        from ..tarreader import open_tar
        try:
            with open_tar(self.path) as tar:
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
                    
                    if member.isdir:
                        if amiga_dir:
                            vol.makedirs(amiga_dir)
                            dir_count += 1
                    elif member.isfile:
                        parent_dir = "/".join(amiga_dir.split("/")[:-1])
                        if parent_dir:
                            vol.makedirs(parent_dir)
                            
                        # Use member.chunks() directly
                        def stream_file(m):
                            for chk in m.chunks():
                                yield chk
                                
                        # tarreader doesn't directly expose mtime, wait, does it?
                        # Let's check if tarreader exposes mtime! If not we ignore it, but wait!
                        # Let's just use mtime=None for now to get it compiling if member has no mtime.
                        mtime = None
                        if hasattr(member, "mtime"):
                            mtime = datetime.fromtimestamp(member.mtime)
                            
                        vol.write_file(
                            amiga_dir, stream_file(member), size=member.size, 
                            protect=protect, comment=comment, mtime=mtime
                        )
                        n += 1
                        total_bytes += member.size
                        if (total_bytes - last_flush_bytes) > 50_000_000:
                            if hasattr(vol, "bitmap"):
                                vol.bitmap.flush()
                            vol.dev.flush()
                            last_flush_bytes = total_bytes
                    # For tarreader we don't track underlying file offset easily, 
                    # but we can track total_bytes of file content written.
                    print_progress(total_bytes, total_archive_size, parts[-1])
                        
        except Exception as e:
            print("") # lock progress bar before error message
            print("error: streaming interrupted: %s" % e, file=sys.stderr)
            raise
            
        print("") # lock progress bar
        return n, dir_count, total_bytes
