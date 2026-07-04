import sys
import os
import subprocess
from datetime import datetime
from .base import ArchiveHandler, find_executable, read_chunks

class RarHandler(ArchiveHandler):
    @classmethod
    def can_handle(cls, path):
        with open(path, "rb") as f:
            data = f.read(7)
            if data in (b'Rar!\x1a\x07\x00', b'Rar!\x1a\x07\x01'):
                return True
        return False
        
    def test_archive(self):
        exe = find_executable(["7z", "7za", "unrar"])
        if not exe:
            sys.stdout.write("verifying archive integrity: %s... " % self.path)
            sys.stdout.flush()
            print("ERROR: RAR extraction tool (7z, 7za, unrar) not found")
            return False
            
        sys.stdout.write("verifying archive integrity: %s... " % self.path)
        sys.stdout.flush()
        try:
            # unrar uses 't' for testing; 7z also uses 't'
            res = subprocess.run([exe, "t", self.path], capture_output=True)
            if res.returncode == 0:
                print("OK")
                return True
            else:
                err = res.stderr.decode("utf-8", errors="replace").strip() or res.stdout.decode("utf-8", errors="replace").strip()
                print("ERROR: %s" % err)
                return False
        except Exception as e:
            print("ERROR: %s" % e)
            return False
            
    def stream_to_volume(self, vol, base_amiga_path, truncate_func, max_len, protect, comment):
        from ..cli import print_progress
        from ..fs.ffs import FSError
        
        exe = find_executable(["7z", "7za", "unrar"])
        if not exe:
            raise FSError("RAR extraction tool (7z, 7za, unrar) not found")
            
        is_unrar = "unrar" in os.path.basename(exe).lower()
        
        try:
            if is_unrar:
                entries = self._parse_unrar_list(exe)
            else:
                entries = self._parse_7z_list(exe)
        except Exception as e:
            raise FSError("failed to list archive: %s" % e)
            
        n = 0
        dir_count = 0
        total_bytes = 0
        total_archive_size = os.path.getsize(self.path)
        
        try:
            for entry in entries:
                name = entry["name"]
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
                
                if entry["isdir"]:
                    if amiga_dir:
                        vol.makedirs(amiga_dir)
                        dir_count += 1
                else:
                    parent_dir = "/".join(amiga_dir.split("/")[:-1])
                    if parent_dir:
                        vol.makedirs(parent_dir)
                        
                    # Stream file from tool stdout
                    if is_unrar:
                        # unrar p -inul <archive> <file>
                        cmd = [exe, "p", "-inul", self.path, entry["raw_name"]]
                    else:
                        # 7z x -so <archive> <file>
                        cmd = [exe, "x", "-so", self.path, entry["raw_name"]]
                        
                    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    
                    try:
                        vol.write_file(
                            amiga_dir, read_chunks(proc.stdout), size=entry["size"],
                            protect=protect, comment=comment, mtime=entry["mtime"]
                        )
                        n += 1
                        total_bytes += entry["size"]
                    finally:
                        proc.stdout.close()
                        if proc.stderr:
                            proc.stderr.close()
                        proc.wait()
                        
                print_progress(min(total_bytes, total_archive_size), total_archive_size, parts[-1])
                
        except Exception as e:
            print("") # newline to lock progress bar before error message
            print("error: streaming interrupted: %s" % e, file=sys.stderr)
            raise
            
        # Force final 100% update
        print_progress._last_t = 0
        print_progress(total_bytes, total_bytes, "")
        print("") # newline to lock progress bar
        
        return n, dir_count, total_bytes

    def _parse_7z_list(self, exe):
        cmd = [exe, "l", "-slt", self.path]
        res = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        if res.returncode != 0:
            raise RuntimeError("7z list failed: %s" % res.stderr)
            
        entries = []
        current = {}
        in_entries = False
        for line in res.stdout.splitlines():
            line = line.strip()
            if not line:
                if current and "Path" in current and in_entries:
                    entries.append(current)
                current = {}
                continue
            if line.startswith("----------"):
                in_entries = True
                current = {}
                continue
            if " = " in line:
                k, v = line.split(" = ", 1)
                current[k.strip()] = v.strip()
        if current and "Path" in current and in_entries:
            entries.append(current)
                
        parsed_entries = []
        for entry in entries:
            name = entry.get("Path")
            if not name:
                continue
            if "Type" in entry or "Physical Size" in entry:
                continue
                
            attr = entry.get("Attributes", "")
            folder = entry.get("Folder", "")
            is_dir = folder == "+" or attr.startswith("d") or "D" in attr
            
            size_str = entry.get("Size", "0")
            try:
                size = int(size_str)
            except ValueError:
                size = 0
                
            mtime_str = entry.get("Modified", "")[:19]
            try:
                dt = datetime.strptime(mtime_str, "%Y-%m-%d %H:%M:%S")
            except Exception:
                dt = datetime.now()
                
            parsed_entries.append({
                "name": name,
                "raw_name": name,
                "size": size,
                "isdir": is_dir,
                "mtime": dt,
            })
        return parsed_entries

    def _parse_unrar_list(self, exe):
        cmd = [exe, "v", self.path]
        res = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        if res.returncode != 0:
            raise RuntimeError("unrar list failed: %s" % res.stderr)
            
        entries = []
        lines = res.stdout.splitlines()
        
        # Look for the dashed line indicating the start of table
        start_idx = -1
        for idx, line in enumerate(lines):
            if line.startswith("-------------"):
                start_idx = idx + 1
                break
                
        if start_idx == -1 or start_idx >= len(lines):
            return entries
            
        # Parse lines until we hit another dashed line or the end
        for idx in range(start_idx, len(lines)):
            line = lines[idx]
            if line.startswith("-------------"):
                break
            parts = line.strip().split(None, 7)
            if len(parts) < 8:
                continue
                
            attr = parts[0]
            is_dir = attr.startswith("d") or attr.startswith("D")
            
            try:
                size = int(parts[1])
            except ValueError:
                size = 0
                
            # Parse mtime
            date_str = parts[4] + " " + parts[5]
            try:
                # Format: DD-MM-YY HH:MM or YYYY-MM-DD HH:MM
                if len(parts[4]) == 8: # DD-MM-YY or YY-MM-DD
                    dt = datetime.strptime(date_str, "%d-%m-%y %H:%M")
                else:
                    dt = datetime.strptime(date_str, "%Y-%m-%d %H:%M")
            except Exception:
                dt = datetime.now()
                
            name = parts[7]
            parsed_entries.append({
                "name": name,
                "raw_name": name,
                "size": size,
                "isdir": is_dir,
                "mtime": dt,
            })
            
        return parsed_entries
