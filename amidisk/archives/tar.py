import sys
import tarfile
from datetime import datetime
from .base import ArchiveHandler

class TarHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        return tarfile.is_tarfile(path)
        
    def test_archive(self):
        from ..tarreader import open_tar
        import sys
        sys.stdout.write("verifying archive integrity: %s... " % self.path)
        sys.stdout.flush()
        try:
            with open_tar(self.path) as tar:
                for _ in tar:
                    pass
            print("OK")
            return True
        except Exception as e:
            print("ERROR: %s" % e)
            return False
            
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        from ..cli import print_progress
        import os
        
        n = 0
        dir_count = 0
        total_bytes = 0
        total_archive_size = os.path.getsize(self.path)
        
        try:
            from ..tarreader import open_tar
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

                        mtime = datetime.fromtimestamp(member.mtime or 0)
                        vol.write_file(
                            amiga_dir, member.chunks(), size=member.size,
                            protect=protect, comment=comment, mtime=mtime
                        )
                        n += 1
                        total_bytes += member.size
                    # progress: content bytes against total archive size
                    print_progress(total_bytes, total_archive_size, parts[-1])
        except Exception as e:
            print("") # newline to lock progress bar before error message
            print("error: streaming interrupted: %s" % e, file=sys.stderr)
            raise
            
        # Force final 100% update (throttle may have skipped the last real update)
        print_progress._last_t = 0  # reset throttle so it always fires
        print_progress(total_bytes, total_bytes, "")
        print("")  # newline to lock progress bar
        return n, dir_count, total_bytes
