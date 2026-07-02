import sys
import zipfile
import os
from datetime import datetime
from .base import ArchiveHandler

class ZipHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        return zipfile.is_zipfile(path)
        
    def test_archive(self):
        print("verifying archive integrity: %s..." % self.path)
        try:
            with zipfile.ZipFile(self.path, "r") as zf:
                ret = zf.testzip()
                if ret is not None:
                    print("error: archive verification failed on file: %s" % ret, file=sys.stderr)
                    return False
            return True
        except zipfile.BadZipFile as e:
            print("error: archive verification failed: %s" % e, file=sys.stderr)
            return False
            
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        from ..cli import print_progress
        
        n = 0
        total_bytes = 0
        last_flush_bytes = 0
        total_archive_size = os.path.getsize(self.path)
        
        try:
            with zipfile.ZipFile(self.path, "r") as zf:
                # zipfile read cursor is not directly available per item in an easy linear way like tar
                # We can just use the total compressed size or uncompressed size as a heuristic for progress
                # but zipfile doesn't expose the archive read pointer. We will fake it with bytes processed.
                
                for info in zf.infolist():
                    parts = [truncate_func(p, max_len) for p in info.filename.split("/") if p]
                    if not parts:
                        continue
                    rel_amiga = "/".join(parts)
                    
                    amiga_dir = base_amiga_path if rel_amiga == "." else (
                        (base_amiga_path + "/" if base_amiga_path else "") + rel_amiga
                    )
                    
                    if info.is_dir():
                        if amiga_dir:
                            vol.makedirs(amiga_dir)
                    else:
                        parent_dir = "/".join(amiga_dir.split("/")[:-1])
                        if parent_dir:
                            vol.makedirs(parent_dir)
                            
                        with zf.open(info) as fh:
                            mtime = datetime(*info.date_time)
                            vol.write_file(
                                amiga_dir, fh, size=info.file_size, 
                                protect=protect, comment=comment, mtime=mtime
                            )
                            n += 1
                            total_bytes += info.file_size
                            
                            if (total_bytes - last_flush_bytes) > 50_000_000:
                                if hasattr(vol, "bitmap"):
                                    vol.bitmap.flush()
                                vol.dev.flush()
                                last_flush_bytes = total_bytes
                                
                    # Progress approx
                    print_progress(min(total_bytes, total_archive_size), total_archive_size, parts[-1])
        except Exception as e:
            import traceback
            traceback.print_exc()
            from ..fs.ffs import FSError
            raise FSError("streaming interrupted: %s" % e)
