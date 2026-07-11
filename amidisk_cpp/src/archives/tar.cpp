#include "tar.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <unordered_set>

namespace amidisk {

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static uint64_t parse_octal(const char* str, size_t max_len) {
    uint64_t val = 0;
    for (size_t i = 0; i < max_len && str[i]; ++i) {
        if (str[i] >= '0' && str[i] <= '7') {
            val = (val << 3) | (str[i] - '0');
        } else if (str[i] == ' ' || str[i] == '\0') {
            continue;
        } else {
            break;
        }
    }
    return val;
}

TarExtractor::TarExtractor(const std::string& tar_path) : tar_path_(tar_path) {}

TarExtractor::~TarExtractor() {}

void TarExtractor::extract_to(Volume* vol, const std::string& dest_path) {
    std::ifstream file(tar_path_, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Cannot open TAR file: " + tar_path_);

    std::string base_dest = dest_path;
    if (!base_dest.empty() && base_dest.back() != '/') {
        base_dest += '/';
    }

    // Cache of directories already created to avoid repeated makedirs calls
    std::unordered_set<std::string> made_dirs;
    auto ensure_dir = [&](const std::string& path) {
        if (path.empty() || made_dirs.count(path)) return;
        vol->makedirs(path);
        // Cache all parent paths too
        std::string p = path;
        while (!p.empty()) {
            made_dirs.insert(p);
            auto pos = p.rfind('/');
            if (pos == std::string::npos || pos == 0) break;
            p = p.substr(0, pos);
        }
    };

    while (true) {
        TarHeader hdr;
        if (!file.read(reinterpret_cast<char*>(&hdr), 512)) break;
        
        if (hdr.name[0] == '\0') break; // End of archive
        
        std::string name(hdr.name, strnlen(hdr.name, 100));
        if (hdr.prefix[0] != '\0') {
            std::string prefix(hdr.prefix, strnlen(hdr.prefix, 155));
            name = prefix + "/" + name;
        }
        
        uint64_t size = parse_octal(hdr.size, 12);
        char typeflag = hdr.typeflag;
        
        std::string full_path = base_dest + name;

        // Skip macOS AppleDouble files (._prefix) and PaxHeader entries
        bool skip_entry = false;
        if (name.find("/._") != std::string::npos || name.rfind("._", 0) == 0) {
            skip_entry = true;
        }
        if (name.find("PaxHeader/") != std::string::npos || typeflag == 'x' || typeflag == 'g') {
            skip_entry = true;
        }

        if (skip_entry) {
            uint64_t skip = size + ((512 - (size % 512)) % 512);
            if (skip > 0) file.seekg(skip, std::ios::cur);
            continue;
        }

        if (typeflag == '5' || (name.back() == '/')) {
            // Directory - use cached ensure_dir
            if (!full_path.empty() && full_path.back() == '/') full_path.pop_back();
            try {
                ensure_dir(full_path);
            } catch (const std::runtime_error& e) {
                if (std::string(e.what()).find("already exists") == std::string::npos) {
                    throw;
                }
            }
        } else if (typeflag == '0' || typeflag == '\0') {
            // File - ensure parent directory exists using cache
            auto last_slash = full_path.rfind('/');
            if (last_slash != std::string::npos && last_slash > 0) {
                std::string parent = full_path.substr(0, last_slash);
                try {
                    ensure_dir(parent);
                } catch (...) {}
            }

            std::vector<uint8_t> data(size);
            if (size > 0) {
                file.read(reinterpret_cast<char*>(data.data()), size);
                if (static_cast<size_t>(file.gcount()) != size) {
                    throw std::runtime_error("Unexpected end of TAR file reading " + name);
                }
            }
            vol->write_file(full_path, data);

            // Skip padding to next 512-byte boundary
            uint64_t padding = (512 - (size % 512)) % 512;
            if (padding > 0) {
                file.seekg(padding, std::ios::cur);
            }
        } else {
            // Unsupported type, skip data
            uint64_t skip = size + ((512 - (size % 512)) % 512);
            if (skip > 0) file.seekg(skip, std::ios::cur);
        }
    }
}

} // namespace amidisk
