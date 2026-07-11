#include <iostream>
#include <CLI/CLI.hpp>
#include "image.h"
#include "cli_util.h"
#include "safety.h"
#include "output.h"

#include <filesystem>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif

struct ParsedArg {
    std::string image;
    std::string vol;
    std::string path;
};

inline ParsedArg parse_image_arg(const std::string& image_arg, const std::string& vol_arg = "") {
    size_t idx = image_arg.rfind(':');
    if (idx != std::string::npos && idx > 0) {
        if (idx == 1 && std::isalpha(image_arg[0])) {
            return {image_arg, vol_arg, ""};
        }
        std::string img_path = image_arg.substr(0, idx);
        std::string vol_and_path = image_arg.substr(idx + 1);

        std::error_code ec;
        if (std::filesystem::exists(img_path, ec)) {
            // Split vol_and_path into volume and path at first /
            auto slash = vol_and_path.find('/');
            if (slash != std::string::npos) {
                return {img_path, vol_and_path.substr(0, slash), vol_and_path.substr(slash + 1)};
            }
            return {img_path, vol_and_path, ""};
        }

        size_t start = (image_arg.length() > 2 && image_arg[1] == ':') ? 2 : 0;
        size_t idx2 = image_arg.find(':', start);
        if (idx2 != std::string::npos && idx2 > start) {
            std::string vol_and_path2 = image_arg.substr(idx2 + 1);
            auto slash = vol_and_path2.find('/');
            if (slash != std::string::npos) {
                return {image_arg.substr(0, idx2), vol_and_path2.substr(0, slash), vol_and_path2.substr(slash + 1)};
            }
            return {image_arg.substr(0, idx2), vol_and_path2, ""};
        }
    }
    return {image_arg, vol_arg, ""};
}

using namespace amidisk;

// Forward declarations
static int64_t get_file_mtime_unix(const std::string& path);

void cmd_info(const std::string& image_path, const std::string& vol_name) {
    try {
        auto img = DiskImage::open(image_path);
        
        if (!vol_name.empty()) {
            auto v_ref = img->get_volume(vol_name);
            if (!v_ref) {
                std::cerr << "Volume " << vol_name << " not found." << std::endl;
                return;
            }
            std::cout << "\n[ RDB Partition Configuration ]\n";
            std::cout << "  drive:      " << v_ref->name() << "\n";
            if (v_ref->partition()) {
                auto part = v_ref->partition();
                auto info = part->get_info();
                std::cout << "  dos type:   " << part->get_dos_type_str() << "\n";
                std::cout << "  size:       " << human_size(part->get_byte_size()) << " (cylinders " 
                          << info["low_cyl"].get<uint32_t>() << " to " << info["high_cyl"].get<uint32_t>() << ")\n";
                std::cout << "  boot:       " << (part->is_bootable() ? "Yes (Priority: " + std::to_string(info["boot_pri"].get<int>()) + ")" : "No") << "\n";
                std::cout << "  blksize:    " << part->get_block_size() << " bytes\n";
                std::cout << "  max trans:  0x" << std::hex << std::setfill('0') << std::setw(8) << info["max_transfer"].get<uint32_t>() << std::dec << "\n";
                std::cout << "  mask:       0x" << std::hex << std::setfill('0') << std::setw(8) << info["mask"].get<uint32_t>() << std::dec << "\n";
                std::cout << "  buffers:    " << info["num_buffer"].get<uint32_t>() << "\n";
            }
            
            std::cout << "\n[ Filesystem Statistics ]\n";
            try {
                auto vol = v_ref->mount();
                std::cout << "  label:      '" << vol->get_label() << "'\n";
                
                auto vinfo = vol->get_info();
                uint64_t total = vinfo.total_blocks * vinfo.block_size;
                uint64_t free = vinfo.free_bytes;
                uint64_t used = total > free ? total - free : 0;
                
                std::cout << "  used:       " << human_size(used) << "\n";
                std::cout << "  free:       " << human_size(free) << "\n";
                
                if (v_ref->partition()) {
                    auto info = v_ref->partition()->get_info();
                    try {
                        std::vector<uint8_t> boot(1024);
                        img->blkdev()->read(v_ref->partition()->get_byte_offset(), boot);
                        uint32_t sum = 0;
                        for (int i = 0; i < 256; ++i) {
                            sum += (boot[i*4]<<24) | (boot[i*4+1]<<16) | (boot[i*4+2]<<8) | boot[i*4+3];
                        }
                        sum = (~sum + 1) & 0xFFFFFFFF;
                        uint32_t got = (boot[4]<<24) | (boot[5]<<16) | (boot[6]<<8) | boot[7];
                        if (sum == got && got != 0) {
                            std::cout << "  bootblock:  Custom Executable (chksum OK: 0x" << std::hex << std::setw(8) << std::setfill('0') << got << std::dec << ")\n";
                        } else if (got == 0) {
                            std::cout << "  bootblock:  Standard (ID: " << v_ref->partition()->get_dos_type_str() << ", no custom bootcode)\n";
                        } else {
                            std::cout << "  bootblock:  INVALID CHECKSUM (ID: " << v_ref->partition()->get_dos_type_str() << ", got: 0x" 
                                      << std::hex << std::setw(8) << std::setfill('0') << got << ", exp: 0x" << sum << std::dec << ")\n";
                        }
                    } catch (...) {}
                }
                
                std::cout << "  Scanning...\r" << std::flush;
                auto rep = vol->check(false);
                std::cout << "             \r"; // clear line
                
                std::cout << "  files:      " << rep.files << "\n";
                std::cout << "  dirs:       " << rep.dirs << "\n";
                std::cout << "  state:      " << (rep.ok ? "OK" : "ERRORS") << "\n";
            } catch (const std::exception& e) {
                std::cout << "  Could not mount: " << e.what() << "\n";
            }
            return;
        }

        if (img->rdisk()) {
            auto r = img->rdisk()->rdsk();
            std::cout << "\n[ Global Image Info ]\n";
            std::cout << "image:  " << image_path << "\n";
            std::cout << "kind:   RDB  (" << (img->blkdev()->size_bytes()) / (1024*1024) << " MB)\n";
            std::cout << "disk:   " << r->disk_vendor << " " << r->disk_product << " " << r->disk_revision
                      << "  chs=" << r->cylinders << "/" << r->heads << "/" << r->sectors 
                      << "  rdb@block " << r->blk_num() << "\n";
            
            std::cout << "\n[ RDB Partition Configuration ]\n";
            for (const auto& part : img->rdisk()->partitions()) {
                auto info = part->get_info();
                std::string boot_str = part->is_bootable() ? "Yes (Priority: " + std::to_string(info["boot_pri"].get<int>()) + ")" : "No";
                
                std::cout << "  " << std::left << std::setw(6) << part->drv_name()
                          << " " << part->get_dos_type_str() 
                          << " " << std::left << std::setw(16) << (std::to_string(part->get_byte_size() / (1024 * 1024)) + " MB")
                          << " (cylinders " << info["low_cyl"].get<uint32_t>() << " to " << info["high_cyl"].get<uint32_t>() << ")"
                          << " boot=" << boot_str << "\n";
            }
        } else {
            std::cout << "\n[ Global Image Info ]\n";
            std::cout << "image:  " << image_path << "\n";
            std::cout << "kind:   Floppy / Single Volume (" << (img->blkdev()->size_bytes()) / 1024 << " KB)\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

#include "fs/ffs.h"
#include "fs/pfs3.h"
#include "fs/sfs.h"
#include "fs/util.h"
#include "rdb/rdisk.h"
#include "rdb/rescue.h"
#include "blkdev/overlay.h"
#include "blkdev/bulk_cache.h"
#include "archives/tar.h"
#include "archives/base.h"
#include <iostream>
#include <iomanip>
#include <fstream>

uint64_t parse_size_string(const std::string& s) {
    if (s.empty()) return 10 * 1024 * 1024;
    uint64_t mult = 1;
    std::string num_part = s;
    char suffix = std::toupper(s.back());
    if (suffix == 'K') { mult = 1024; num_part = s.substr(0, s.size() - 1); }
    else if (suffix == 'M') { mult = 1024 * 1024; num_part = s.substr(0, s.size() - 1); }
    else if (suffix == 'G') { mult = 1024ULL * 1024 * 1024; num_part = s.substr(0, s.size() - 1); }
    return std::stoull(num_part) * mult;
}

void cmd_create(const std::string& image_path, const std::string& size_str, const std::string& format_label, const std::string& dostype_str) {
    try {
        uint64_t size_bytes = parse_size_string(size_str);
        std::ofstream file(image_path, std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Cannot create file: " + image_path);
        file.seekp(size_bytes - 1);
        file.write("", 1);
        file.close();
        std::cout << "Created raw image: " << image_path << " (" << (size_bytes / (1024 * 1024)) << " MB)" << std::endl;

        if (!format_label.empty()) {
            auto img = DiskImage::open(image_path, false);
            auto vol_ref = img->get_volume("DH0");
            if (vol_ref) {
                auto bdev = vol_ref->create_blkdev();
                uint32_t dos_type = 0x444F5307;
                if (!dostype_str.empty()) {
                    if (dostype_str == "ofs") dos_type = 0x444F5300;
                    else if (dostype_str == "ffs") dos_type = 0x444F5301;
                    else if (dostype_str == "ofs-intl") dos_type = 0x444F5302;
                    else if (dostype_str == "ffs-intl") dos_type = 0x444F5303;
                    else if (dostype_str == "ofs-intl-dc") dos_type = 0x444F5304;
                    else if (dostype_str == "ffs-intl-dc") dos_type = 0x444F5305;
                    else if (dostype_str == "ofs-intl-lnfs") dos_type = 0x444F5306;
                    else if (dostype_str == "ffs-intl-lnfs" || dostype_str == "dos7") dos_type = 0x444F5307;
                }
                FFSVolume vol(bdev);
                vol.format(format_label, dos_type);
                std::cout << "Formatted as " << format_label << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_format(const std::string& image_path, const std::string& volume_name, const std::string& label, const std::string& dostype_str) {
    try {
        auto img = DiskImage::open(image_path, false);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            std::cerr << "Error: Volume not found" << std::endl;
            return;
        }
        
        uint32_t dos_type = 0;
        if (!dostype_str.empty()) {
            dos_type = std::stoul(dostype_str, nullptr, 16);
        } else if (vol_ref->partition()) {
            dos_type = vol_ref->partition()->dos_env().dos_type;
        }
        
        if (!dos_type) {
            std::cerr << "Error: No DOS type provided and no partition table to derive it from." << std::endl;
            return;
        }
        
        auto bdev = vol_ref->create_blkdev();
        uint32_t spb = 1, reserved = 2;
        if (vol_ref->partition()) {
            spb = std::max<uint32_t>(vol_ref->partition()->dos_env().sec_per_blk, 1);
            reserved = vol_ref->partition()->dos_env().reserved;
        }
        
        if ((dos_type & 0xFFFFFF00) == 0x50465300 || (dos_type & 0xFFFFFF00) == 0x50445300) {
            PFS3Volume vol(bdev, spb, reserved, dos_type);
            vol.format(label, dos_type);
        } else if ((dos_type & 0xFFFFFF00) == 0x53465300) {
            SFSVolume vol(bdev, dos_type);
            vol.format(label, dos_type);
        } else {
            FFSVolume vol(bdev, spb, reserved, dos_type);
            vol.format(label, dos_type);
        }
        std::cout << "Formatted volume '" << label << "' with DOS type 0x" << std::hex << std::uppercase << dos_type << std::dec << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

// Implementation of put logic that works on an already-mounted Volume
// This allows cmd_cp to reuse the put logic for non-archive sources
void cmd_put_impl(Volume* vol, const std::string& src_path, const std::string& dest_path,
                  bool recursive, const std::string& protect_str, const std::string& comment) {
    uint32_t protect = 0;
    // Parse protect string if provided (e.g. "rwedspa")
    for (char c : protect_str) {
        switch (std::tolower(c)) {
            case 'r': protect |= 0x08; break;
            case 'w': protect |= 0x04; break;
            case 'e': protect |= 0x02; break;
            case 'd': protect |= 0x01; break;
            case 's': protect |= 0x40; break;
            case 'p': protect |= 0x20; break;
            case 'a': protect |= 0x10; break;
        }
    }

    uint32_t max_name_len = 107;

    if (std::filesystem::is_regular_file(src_path)) {
        std::string basename = truncate_name(std::filesystem::path(src_path).filename().string(), max_name_len);
        std::string amiga_path = dest_path;

        if (amiga_path.empty() || amiga_path.back() == '/') {
            amiga_path += basename;
        } else {
            try {
                auto entries = vol->list_dir(amiga_path);
                amiga_path = amiga_path + "/" + basename;
            } catch (...) {
                // dest_path is the target filename
            }
        }

        std::ifstream ifs(src_path, std::ios::binary);
        if (!ifs) {
            throw std::runtime_error("Cannot open file: " + src_path);
        }

        std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();

        WriteParams params;
        params.protect = protect;
        params.comment = comment;
        params.mtime = get_file_mtime_unix(src_path);

        vol->write_file(amiga_path, data, params);
        out::print((fmt() << "Wrote " << amiga_path << " (" << data.size() << " bytes)").str());
        return;
    }

    if (std::filesystem::is_directory(src_path)) {
        if (!recursive) {
            throw std::runtime_error(src_path + " is a directory (use -r)");
        }

        std::string base = dest_path;
        while (!base.empty() && base.back() == '/') base.pop_back();

        // Include the source directory name in the destination (like cp -r behavior)
        std::string src_basename = truncate_name(std::filesystem::path(src_path).filename().string(), max_name_len);
        if (base.empty()) {
            base = src_basename;
        } else {
            base = base + "/" + src_basename;
        }
        vol->makedirs(base);

        uint64_t total_expected = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
            if (entry.is_regular_file()) {
                total_expected += entry.file_size();
            }
        }

        uint32_t file_count = 0;
        uint64_t total_bytes = 0;
        auto start = std::chrono::steady_clock::now();

        for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
            auto rel = std::filesystem::relative(entry.path(), src_path);
            std::string rel_str;
            for (const auto& part : rel) {
                if (!rel_str.empty()) rel_str += "/";
                rel_str += truncate_name(part.string(), max_name_len);
            }

            std::string amiga_path = base.empty() ? rel_str : (base + "/" + rel_str);

            if (entry.is_directory()) {
                vol->makedirs(amiga_path);
            } else if (entry.is_regular_file()) {
                std::string parent = amiga_path;
                auto slash = parent.rfind('/');
                if (slash != std::string::npos) {
                    parent = parent.substr(0, slash);
                    if (!parent.empty()) vol->makedirs(parent);
                }

                std::ifstream ifs(entry.path(), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                ifs.close();

                WriteParams params;
                params.protect = protect;
                params.comment = comment;
                params.mtime = get_file_mtime_unix(entry.path().string());

                vol->write_file(amiga_path, data, params);
                file_count++;
                total_bytes += data.size();
                print_progress(total_bytes, total_expected, rel_str);
            }
        }

        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        print_transfer_stats(file_count, 0, total_bytes, elapsed);
    }
}

void cmd_cp(const std::string& image_path, const std::string& volume_name, const std::string& src_path, const std::string& dest_path, bool bulk, bool recursive) {
    try {
        auto img = DiskImage::open(image_path, false);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            std::cerr << "Error: Volume not found" << std::endl;
            return;
        }

        Volume* vol = vol_ref->mount();

        // Use BulkGuard to wrap the volume's block device with caching
        std::unique_ptr<BulkGuard> bulk_guard;
        if (bulk) {
            bulk_guard = std::make_unique<BulkGuard>(vol->blkdev_ref());
        }

        // Check if src is an archive
        auto handler = create_archive_handler(src_path);
        if (handler) {
            // It's an archive - extract it
            if (!handler->test_archive()) {
                std::cerr << "Archive integrity check failed." << std::endl;
                return;
            }
            auto [files, dirs, bytes] = handler->stream_to_volume(vol, dest_path, 107, 0, "");
            std::cout << "\nExtracted " << files << " files, " << dirs << " directories (" << bytes << " bytes) to " << (dest_path.empty() ? "/" : dest_path) << std::endl;
            return;
        }

        // Check if it's a tar file (fallback for .tar without magic detection)
        std::string lower_src = src_path;
        std::transform(lower_src.begin(), lower_src.end(), lower_src.begin(), ::tolower);
        if (lower_src.ends_with(".tar")) {
            TarExtractor tar(src_path);
            tar.extract_to(vol, dest_path);
            std::cout << "Extracted " << src_path << " to " << (dest_path.empty() ? "/" : dest_path) << std::endl;
            return;
        }

        // Not an archive - treat as regular file/directory (like put command)
        std::filesystem::path src_fs(src_path);
        if (!std::filesystem::exists(src_fs)) {
            std::cerr << "Error: Source not found: " << src_path << std::endl;
            return;
        }

        if (std::filesystem::is_directory(src_fs)) {
            if (!recursive) {
                std::cerr << "Error: " << src_path << " is a directory, use -r for recursive copy" << std::endl;
                return;
            }
            // Recursive directory copy
            cmd_put_impl(vol, src_path, dest_path, true, "", "");
        } else {
            // Single file copy
            cmd_put_impl(vol, src_path, dest_path, false, "", "");
        }
        // BulkGuard destructor flushes automatically
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_ls(const std::string& image_path, const std::string& volume_name, const std::string& path, bool long_format) {
    try {
        auto img = DiskImage::open(image_path);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            out::error("Error: Volume not found");
            return;
        }
        auto vol = vol_ref->mount();
        auto entries = vol->list_dir(path);
        if (entries.empty()) {
            out::print("(empty)");
        } else {
            for (const auto& e : entries) {
                std::ostringstream line;
                if (long_format) {
                    std::string name = e.name_str();
                    if (e.is_dir()) name += "/";

                    char buf[256];
                    snprintf(buf, sizeof(buf), "%s %10llu %s  %s",
                             e.protect_str().c_str(),
                             (unsigned long long)e.size,
                             e.mtime_str().c_str(),
                             name.c_str());
                    line << buf;
                    if (!e.comment_str().empty()) {
                        line << "  (" << e.comment_str() << ")";
                    }
                } else {
                    std::string type = e.is_dir() ? "DIR " : (e.is_file() ? "FILE" : "LINK");
                    line << type << "  " << e.name_str();
                    if (!e.comment_str().empty()) {
                        line << "  (" << e.comment_str() << ")";
                    }
                }
                out::print(line.str());
            }
        }
    } catch (const std::exception& e) {
        out::error(fmt() << "Error: " << e.what());
    }
}

void cmd_cat(const std::string& image_path, const std::string& volume_name, const std::string& path) {
    try {
        auto img = DiskImage::open(image_path);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            out::error("Error: Volume not found");
            return;
        }
        auto vol = vol_ref->mount();
        vol->read_file(path, [](std::span<const uint8_t> chunk) {
            std::cout.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        });
    } catch (const std::exception& e) {
        out::error(fmt() << "Error: " << e.what());
    }
}

static std::string safe_filename(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            result += '_';
        } else {
            result += c;
        }
    }
    return result;
}

static void set_file_mtime(const std::string& path, int64_t unix_time) {
    if (unix_time <= 0) return;
#ifdef _WIN32
    struct _utimbuf ut;
    ut.actime = static_cast<time_t>(unix_time);
    ut.modtime = static_cast<time_t>(unix_time);
    _utime(path.c_str(), &ut);
#else
    struct utimbuf ut;
    ut.actime = static_cast<time_t>(unix_time);
    ut.modtime = static_cast<time_t>(unix_time);
    utime(path.c_str(), &ut);
#endif
}

static int64_t get_file_mtime_unix(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return static_cast<int64_t>(st.st_mtime);
    }
    return 0;
}

int cmd_extract(const std::string& image_path, const std::string& volume_name,
                const std::string& src_path, const std::string& dest_path, bool recursive) {
    try {
        auto img = DiskImage::open(image_path);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            out::error("Error: Volume not found");
            return 1;
        }
        auto vol = vol_ref->mount();

        bool is_dir = false;
        Entry target_entry;

        if (src_path.empty() || src_path == "/") {
            is_dir = true;
        } else {
            // Try to list src_path as a directory first
            try {
                auto entries = vol->list_dir(src_path);
                // Success - it's a directory
                is_dir = true;
            } catch (...) {
                // Not a directory or doesn't exist - check if it's a file
                is_dir = false;
            }

            if (!is_dir) {
                // Look up the entry in its parent directory
                std::string parent_path;
                std::string basename;
                auto slash = src_path.rfind('/');
                if (slash != std::string::npos) {
                    parent_path = src_path.substr(0, slash);
                    basename = src_path.substr(slash + 1);
                } else {
                    parent_path = "";
                    basename = src_path;
                }

                auto parent_entries = vol->list_dir(parent_path);
                bool found = false;
                for (const auto& e : parent_entries) {
                    if (e.name_str() == basename) {
                        target_entry = e;
                        is_dir = e.is_dir();
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    out::error(fmt() << "Error: Path not found: " << src_path);
                    return 1;
                }
            }
        }

        std::string dest = dest_path.empty() ? "." : dest_path;

        if (!is_dir) {
            std::string out_file = dest;
            if (std::filesystem::is_directory(dest)) {
                out_file = (std::filesystem::path(dest) / safe_filename(target_entry.name_str())).string();
            }

            auto start = std::chrono::steady_clock::now();
            std::ofstream ofs(out_file, std::ios::binary);
            if (!ofs) {
                out::error(fmt() << "Error: Cannot create file: " << out_file);
                return 1;
            }

            uint64_t written = 0;
            vol->read_file(src_path, [&](std::span<const uint8_t> chunk) {
                ofs.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                written += chunk.size();
            });
            ofs.close();

            set_file_mtime(out_file, target_entry.mtime_unix);

            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            print_transfer_stats(1, 0, written, elapsed);
            return 0;
        }

        if (!recursive) {
            out::error(fmt() << "Error: " << (src_path.empty() ? "volume root" : src_path) << " is a directory (use -r)");
            return 1;
        }

        std::filesystem::create_directories(dest);

        uint32_t file_count = 0, dir_count = 0;
        uint64_t total_bytes = 0;
        auto start = std::chrono::steady_clock::now();

        std::function<void(const std::string&, const std::string&)> extract_dir;
        extract_dir = [&](const std::string& amiga_path, const std::string& host_path) {
            auto entries = vol->list_dir(amiga_path);
            for (const auto& e : entries) {
                std::string name = safe_filename(e.name_str());
                std::string amiga_full = amiga_path.empty() ? name : (amiga_path + "/" + e.name_str());
                std::string host_full = (std::filesystem::path(host_path) / name).string();

                if (e.is_dir()) {
                    std::filesystem::create_directories(host_full);
                    dir_count++;
                    extract_dir(amiga_full, host_full);
                } else if (e.is_file()) {
                    std::ofstream ofs(host_full, std::ios::binary);
                    if (ofs) {
                        vol->read_file(amiga_full, [&](std::span<const uint8_t> chunk) {
                            ofs.write(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                        });
                        ofs.close();
                        set_file_mtime(host_full, e.mtime_unix);
                        file_count++;
                        total_bytes += e.size;
                        print_progress(total_bytes, total_bytes, name);
                    }
                }
            }
        };

        extract_dir(src_path, dest);

        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        print_transfer_stats(file_count, dir_count, total_bytes, elapsed);
        return 0;

    } catch (const std::exception& e) {
        out::error(fmt() << "Error: " << e.what());
        return 1;
    }
}

int cmd_put(const std::string& image_path, const std::string& volume_name,
            const std::string& src_path, const std::string& dest_path,
            bool recursive, bool bulk, uint32_t protect, const std::string& comment) {
    try {
        auto img = DiskImage::open(image_path, false);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            out::error("Error: Volume not found");
            return 1;
        }
        auto vol = vol_ref->mount();

        auto handler = create_archive_handler(src_path);
        if (handler) {
            if (!handler->test_archive()) {
                out::error("Archive integrity check failed.");
                return 1;
            }

            auto start = std::chrono::steady_clock::now();

            uint32_t max_len = 107;
            std::string base = dest_path;
            while (!base.empty() && base.back() == '/') base.pop_back();

            auto [files, dirs, bytes] = handler->stream_to_volume(vol, base, max_len, protect, comment);

            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            print_transfer_stats(files, dirs, bytes, elapsed);
            return 0;
        }

        if (!std::filesystem::exists(src_path)) {
            out::error(fmt() << "Error: No such file: " << src_path);
            return 1;
        }

        uint32_t max_name_len = 107;

        if (std::filesystem::is_regular_file(src_path)) {
            std::string basename = truncate_name(std::filesystem::path(src_path).filename().string(), max_name_len);
            std::string amiga_path = dest_path;

            if (amiga_path.empty() || amiga_path.back() == '/') {
                amiga_path += basename;
            } else {
                try {
                    auto entries = vol->list_dir(amiga_path);
                    amiga_path = amiga_path + "/" + basename;
                } catch (...) {
                }
            }

            std::ifstream ifs(src_path, std::ios::binary);
            if (!ifs) {
                out::error(fmt() << "Error: Cannot open file: " << src_path);
                return 1;
            }

            std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            ifs.close();

            WriteParams params;
            params.protect = protect;
            params.comment = comment;
            params.mtime = get_file_mtime_unix(src_path);

            vol->write_file(amiga_path, data, params);
            out::print((fmt() << "Wrote " << amiga_path << " (" << data.size() << " bytes)").str());
            return 0;
        }

        if (std::filesystem::is_directory(src_path)) {
            if (!recursive) {
                out::error(fmt() << "Error: " << src_path << " is a directory (use -r)");
                return 1;
            }

            std::string base = dest_path;
            while (!base.empty() && base.back() == '/') base.pop_back();

            uint64_t total_expected = 0;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
                if (entry.is_regular_file()) {
                    total_expected += entry.file_size();
                }
            }

            uint32_t file_count = 0;
            uint64_t total_bytes = 0;
            auto start = std::chrono::steady_clock::now();

            // Enable bulk mode: wrap the volume's block device with BulkWriteCache
            std::optional<BulkGuard> bulk_guard;
            if (bulk) {
                bulk_guard.emplace(vol->blkdev_ref());
            }

            for (const auto& entry : std::filesystem::recursive_directory_iterator(src_path)) {
                auto rel = std::filesystem::relative(entry.path(), src_path);
                std::string rel_str;
                for (const auto& part : rel) {
                    if (!rel_str.empty()) rel_str += "/";
                    rel_str += truncate_name(part.string(), max_name_len);
                }

                std::string amiga_path = base.empty() ? rel_str : (base + "/" + rel_str);

                if (entry.is_directory()) {
                    vol->makedirs(amiga_path);
                } else if (entry.is_regular_file()) {
                    std::string parent = amiga_path;
                    auto slash = parent.rfind('/');
                    if (slash != std::string::npos) {
                        parent = parent.substr(0, slash);
                        if (!parent.empty()) vol->makedirs(parent);
                    }

                    std::ifstream ifs(entry.path(), std::ios::binary);
                    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    ifs.close();

                    WriteParams params;
                    params.protect = protect;
                    params.comment = comment;
                    params.mtime = get_file_mtime_unix(entry.path().string());

                    vol->write_file(amiga_path, data, params);
                    file_count++;
                    total_bytes += data.size();
                    print_progress(total_bytes, total_expected, rel_str);
                }
            }

            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            print_transfer_stats(file_count, 0, total_bytes, elapsed);
            return 0;
        }

        out::error(fmt() << "Error: Unsupported file type: " << src_path);
        return 1;

    } catch (const std::exception& e) {
        out::error(fmt() << "Error: " << e.what());
        return 1;
    }
}

void cmd_check(const std::string& image_path, const std::string& volume_name, bool deep) {
    try {
        auto img = DiskImage::open(image_path);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            std::cerr << "Error: Volume not found" << std::endl;
            return;
        }
        auto vol = vol_ref->mount();
        std::cout << "  Scanning..." << std::flush;
        auto rep = vol->check(deep);
        std::cout << "\r\033[2K"; // clear the scanning line
        
        std::cout << "  files:      " << rep.files << std::endl;
        std::cout << "  dirs:       " << rep.dirs << std::endl;
        std::cout << "  state:      " << (rep.ok ? "OK" : "ERRORS") << std::endl;
        
        for (const auto& err : rep.errors) {
            std::cout << "  ERROR: " << err << std::endl;
        }
        for (const auto& warn : rep.warnings) {
            std::cout << "  WARNING: " << warn << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_rdb_init(const std::string& image_path, uint32_t sectors, uint32_t heads, uint32_t rdb_cyls) {
    try {
        auto img = DiskImage::open(image_path, false);
        if (img->rdisk()) {
            std::cerr << "Error: image already has an RDB" << std::endl;
            return;
        }
        auto rdisk = RDisk::create(img->blkdev(), sectors, heads, rdb_cyls);
        std::cout << "Initialized RDB" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_part_add(const std::string& image_path, const std::string& drv_name, uint32_t size_mb, const std::string& dostype_str, uint32_t sec_per_blk, bool bootable, int32_t boot_pri) {
    try {
        auto img = DiskImage::open(image_path, false);
        if (!img->rdisk()) {
            std::cerr << "Error: image has no RDB (run rdb-init first)" << std::endl;
            return;
        }
        uint32_t dos_type = dostype_str.empty() ? 0x444F5303 : std::stoul(dostype_str, nullptr, 16);
        uint64_t size_bytes = static_cast<uint64_t>(size_mb) * 1024 * 1024;
        
        auto part = img->rdisk()->add_partition(drv_name, size_bytes, dos_type, sec_per_blk, bootable, boot_pri);
        std::cout << "Added partition " << drv_name << " (" << part->get_num_cyls() << " cyls, " << part->get_byte_size() / (1024*1024) << " MB)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_part_del(const std::string& image_path, const std::string& drv_name) {
    try {
        auto img = DiskImage::open(image_path, false);
        if (!img->rdisk()) {
            std::cerr << "Error: image has no RDB" << std::endl;
            return;
        }
        img->rdisk()->delete_partition(drv_name);
        std::cout << "Deleted partition " << drv_name << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_fs_add(const std::string& image_path, const std::string& fs_path, const std::string& dostype_str, uint16_t version, uint16_t revision) {
    try {
        auto img = DiskImage::open(image_path, false);
        if (!img->rdisk()) {
            std::cerr << "Error: image has no RDB" << std::endl;
            return;
        }
        uint32_t dos_type = std::stoul(dostype_str, nullptr, 16);
        img->rdisk()->add_filesystem(fs_path, dos_type, version, revision);
        std::cout << "Embedded filesystem 0x" << std::hex << std::uppercase << dos_type << std::dec << " (v" << version << "." << revision << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_fs_extract(const std::string& image_path, const std::string& dostype_str, const std::string& out_path) {
    try {
        auto img = DiskImage::open(image_path, true);
        if (!img->rdisk()) {
            std::cerr << "Error: image has no RDB" << std::endl;
            return;
        }
        uint32_t dos_type = std::stoul(dostype_str, nullptr, 16);
        img->rdisk()->extract_filesystem(dos_type, out_path);
        std::cout << "Extracted filesystem to " << out_path << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_bootblock(const std::string& image_path, const std::string& volume_name, const std::string& bb_path) {
    try {
        auto img = DiskImage::open(image_path, false);
        auto vol_ref = img->get_volume(volume_name);
        if (!vol_ref) {
            std::cerr << "Error: Volume not found" << std::endl;
            return;
        }
        
        std::ifstream file(bb_path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open bootblock file: " + bb_path);
        std::vector<uint8_t> bb_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        
        auto bdev = vol_ref->create_blkdev();
        uint32_t bb_size = bdev->sector_size() * 2; // 2 blocks
        if (bb_data.size() > bb_size) {
            std::cerr << "Error: bootcode is too large (" << bb_data.size() << " > " << bb_size << " bytes)" << std::endl;
            return;
        }
        
        bb_data.resize(bb_size, 0); // pad with zeros
        bdev->write(0, bb_data); // write to block 0 (and 1) of the partition
        
        std::cout << "Installed bootblock (" << bb_data.size() << " bytes) to " << vol_ref->name() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void cmd_rdb_scan(const std::string& image_path, uint64_t rdb_area_secs) {
    auto dev = open_blkdev(image_path, true);
    auto result = amidisk::Rescue::scan(dev, rdb_area_secs, [](uint64_t pos, uint64_t total) {
        if (pos % (8192 * 4) == 0 || pos == total) {
            std::cout << "\rScanning: " << (pos * 100 / total) << "% (" << pos << "/" << total << " blocks)     " << std::flush;
        }
    });
    std::cout << "\n\nFound " << result.first.size() << " partition candidates:\n";
    for (const auto& c : result.first) {
        std::cout << "  " << c.describe() << "\n";
    }
    if (!result.second.empty()) {
        std::cout << "\nNotes:\n";
        for (const auto& n : result.second) {
            std::cout << "  - " << n << "\n";
        }
    }
}

void cmd_rdb_rebuild(const std::string& image_path, const std::string& output_path, bool dry_run) {
    auto dev = open_blkdev(image_path, true);
    std::cout << "Scanning for candidates...\n";
    auto result = amidisk::Rescue::scan(dev);
    
    if (result.first.empty()) {
        std::cout << "No candidates found to rebuild.\n";
        return;
    }
    
    std::cout << "Found " << result.first.size() << " partition candidates:\n";
    for (const auto& c : result.first) {
        std::cout << "  " << c.describe() << "\n";
    }
    
    std::shared_ptr<amidisk::BlockDevice> out_dev;
    if (dry_run) {
        std::cout << "Performing dry run (using memory overlay)...\n";
        out_dev = std::make_shared<amidisk::OverlayBlockDevice>(dev);
    } else if (output_path.empty() || output_path == image_path) {
        out_dev = open_blkdev(image_path, false);
    } else {
        std::cout << "Creating copy at " << output_path << "...\n";
        // Just open the source, read all data, write to output
        std::ofstream ofs(output_path, std::ios::binary);
        for (uint64_t i = 0; i < dev->num_blocks(); i += 8192) {
            uint64_t n = std::min<uint64_t>(8192, dev->num_blocks() - i);
            std::vector<uint8_t> buf(n * 512);
            dev->read(i * 512, buf);
            ofs.write((const char*)buf.data(), buf.size());
        }
        ofs.close();
        out_dev = open_blkdev(output_path, false);
    }
    
    std::cout << "Rebuilding RDB...\n";
    auto rd = amidisk::Rescue::rebuild(out_dev, result.first);
    std::cout << "Rebuild complete. Created new RDB with " << rd->partitions().size() << " partitions.\n";
    if (dry_run) {
        std::cout << "Dry run complete. No changes were written to disk.\n";
    }
}

int main(int argc, char** argv) {
    CLI::App app{"AmigaFSTool C++ Port"};
    
    app.require_subcommand(1);
    
    // Info command
    auto info = app.add_subcommand("info", "Show image and partition information");
    std::string image_path;
    info->add_option("image", image_path, "Path to the disk image")->required();
    std::string info_vol_name = "";
    info->add_option("-v,--volume", info_vol_name, "Volume name for detailed information (e.g. DH0)");
    info->callback([&]() {
        auto parsed = parse_image_arg(image_path, info_vol_name);
        cmd_info(parsed.image, parsed.vol);
    });

    // Ls command
    auto ls = app.add_subcommand("ls", "List directory contents");
    ls->add_option("image", image_path, "Path to the disk image")->required();
    std::string volume_name = "DH0";
    ls->add_option("-v,--volume", volume_name, "Volume name (default: DH0)");
    std::string path = "/";
    ls->add_option("path", path, "Directory path to list");
    bool long_format = false;
    ls->add_flag("-l,--long", long_format, "Long format with protect, size, mtime");
    ls->callback([&]() {
        auto parsed = parse_image_arg(image_path, volume_name);
        std::string ls_path = parsed.path.empty() ? path : parsed.path;
        cmd_ls(parsed.image, parsed.vol, ls_path, long_format);
    });

    // Cat command
    auto cat = app.add_subcommand("cat", "Print file contents to stdout");
    cat->add_option("image", image_path, "Path to the disk image")->required();
    cat->add_option("-v,--volume", volume_name, "Volume name (default: DH0)");
    cat->add_option("path", path, "File path to read")->required();
    cat->callback([&]() {
        auto parsed = parse_image_arg(image_path, volume_name);
        std::string cat_path = parsed.path.empty() ? path : parsed.path;
        cmd_cat(parsed.image, parsed.vol, cat_path);
    });

    // Check command
    auto check_cmd = app.add_subcommand("check", "Validate filesystem consistency");
    check_cmd->add_option("image", image_path, "Path to the disk image")->required();
    check_cmd->add_option("-v,--volume", volume_name, "Volume name (default: DH0)");
    bool deep = false;
    check_cmd->add_flag("--deep", deep, "Deep OFS verification");
    check_cmd->callback([&]() {
        auto parsed = parse_image_arg(image_path, volume_name);
        cmd_check(parsed.image, parsed.vol, deep);
    });

    // Create command (matches Python: create image --size 27G --format LABEL --dostype ffs-intl-lnfs)
    auto create_cmd = app.add_subcommand("create", "Create a new blank image");
    create_cmd->add_option("image", image_path, "Path to the disk image")->required();
    std::string create_size = "10M";
    std::string create_format_label;
    std::string create_dostype;
    create_cmd->add_option("--size", create_size, "Image size (e.g. 100M, 1G)");
    create_cmd->add_option("--format", create_format_label, "Format after creating with this label");
    create_cmd->add_option("--dostype", create_dostype, "DOS type (ofs|ffs|ofs-intl|ffs-intl|ffs-intl-lnfs|dos7)");
    create_cmd->callback([&]() {
        cmd_create(image_path, create_size, create_format_label, create_dostype);
    });
    
    // Format command
    auto format_cmd = app.add_subcommand("format", "Format a volume");
    format_cmd->add_option("image", image_path, "Path to the disk image")->required();
    format_cmd->add_option("-v,--volume", volume_name, "Volume name (default: DH0)");
    std::string label;
    format_cmd->add_option("label", label, "Volume label")->required();
    std::string dostype_str;
    format_cmd->add_option("-t,--dostype", dostype_str, "DOS type in hex (e.g. 444F5303)");
    format_cmd->callback([&]() {
        auto parsed = parse_image_arg(image_path, volume_name);
        cmd_format(parsed.image, parsed.vol, label, dostype_str);
    });

    // Extract command (image → host)
    auto extract_cmd = app.add_subcommand("extract", "Extract files from volume to host");
    extract_cmd->add_option("image", image_path, "Path to the disk image (e.g. disk.hdf:DH0/path)")->required();
    extract_cmd->add_option("-v,--volume", volume_name, "Volume name (default: DH0)");
    std::string extract_dest = ".";
    extract_cmd->add_option("dest", extract_dest, "Destination path on host");
    bool extract_recursive = false;
    extract_cmd->add_flag("-r,--recursive", extract_recursive, "Recursively extract directories");
    extract_cmd->callback([&]() {
        auto parsed = parse_image_arg(image_path, volume_name);
        cmd_extract(parsed.image, parsed.vol, parsed.path, extract_dest, extract_recursive);
    });

    // Put command (host → image)
    auto put_cmd = app.add_subcommand("put", "Copy files from host to volume");
    put_cmd->add_option("image", image_path, "Path to the disk image")->required();
    put_cmd->add_option("-v,--volume", volume_name, "Volume name (default: DH0)");
    std::string put_src;
    put_cmd->add_option("src", put_src, "Source file/directory on host")->required();
    std::string put_dest = "";
    put_cmd->add_option("dest", put_dest, "Destination path on volume");
    bool put_recursive = false;
    put_cmd->add_flag("-r,--recursive", put_recursive, "Recursively copy directories");
    bool put_bulk = false;
    put_cmd->add_flag("--bulk", put_bulk, "Use bulk write cache for better performance");
    std::string put_protect;
    put_cmd->add_option("--protect", put_protect, "Protection bits (e.g. rwedspa)");
    std::string put_comment;
    put_cmd->add_option("--comment", put_comment, "File comment");
    put_cmd->callback([&]() {
        auto parsed = parse_image_arg(image_path, volume_name);
        std::string dest = parsed.path.empty() ? put_dest : parsed.path;
        uint32_t protect = put_protect.empty() ? 0 : str_to_protect(put_protect);
        cmd_put(parsed.image, parsed.vol, put_src, dest, put_recursive, put_bulk, protect, put_comment);
    });

    // Cp command (Archive extraction or file copy)
    // Matches Python: cp src dst (e.g., cp archive.tar image.hdf:DH0/ or cp file.txt image.hdf:DH0/)
    auto cp_cmd = app.add_subcommand("cp", "Copy files/directories/archives to the volume");
    std::string cp_src, cp_dst;
    cp_cmd->add_option("src", cp_src, "Source file, directory, or archive")->required();
    cp_cmd->add_option("dst", cp_dst, "Destination image:volume/path")->required();
    bool cp_bulk = false;
    bool cp_recursive = false;
    cp_cmd->add_flag("--bulk", cp_bulk, "Use bulk write cache for better performance");
    cp_cmd->add_flag("-r,--recursive", cp_recursive, "Recursively copy directories");
    cp_cmd->callback([&]() {
        auto parsed = parse_image_arg(cp_dst);
        std::string dest = parsed.path.empty() ? "/" : "/" + parsed.path;
        cmd_cp(parsed.image, parsed.vol, cp_src, dest, cp_bulk, cp_recursive);
    });

    // mkdir
    auto mkdir = app.add_subcommand("mkdir", "Create a directory");
    mkdir->add_option("image", image_path, "Path to disk image")->required();
    mkdir->add_option("-v,--volume", volume_name, "Volume name");
    mkdir->add_option("path", path, "Directory path")->required();
    bool parents = false;
    mkdir->add_flag("-p,--parents", parents, "Create parent directories as needed");
    mkdir->callback([&]() {
        try {
            auto img = DiskImage::open(image_path, false);
            auto vol_ref = img->get_volume(volume_name);
            if (!vol_ref) { std::cerr << "Volume not found\n"; return; }
            if (parents) vol_ref->mount()->makedirs(path);
            else vol_ref->mount()->mkdir(path);
            std::cout << "Created " << path << std::endl;
        } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; }
    });

    // rm
    auto rm = app.add_subcommand("rm", "Remove a file or directory");
    rm->add_option("image", image_path, "Path to disk image")->required();
    rm->add_option("-v,--volume", volume_name, "Volume name");
    rm->add_option("path", path, "File or directory path")->required();
    bool recursive = false;
    rm->add_flag("-r,--recursive", recursive, "Remove directories and their contents recursively");
    rm->callback([&]() {
        try {
            auto img = DiskImage::open(image_path, false);
            auto vol_ref = img->get_volume(volume_name);
            if (!vol_ref) { std::cerr << "Volume not found\n"; return; }
            vol_ref->mount()->delete_path(path, recursive);
            std::cout << "Deleted " << path << std::endl;
        } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; }
    });

    // mv
    auto mv = app.add_subcommand("mv", "Rename a file or directory");
    mv->add_option("image", image_path, "Path to disk image")->required();
    mv->add_option("-v,--volume", volume_name, "Volume name");
    std::string src, dst;
    mv->add_option("src", src, "Source path")->required();
    mv->add_option("dst", dst, "Destination path")->required();
    mv->callback([&]() {
        try {
            auto img = DiskImage::open(image_path, false);
            auto vol_ref = img->get_volume(volume_name);
            if (!vol_ref) { std::cerr << "Volume not found\n"; return; }
            vol_ref->mount()->rename(src, dst);
            std::cout << "Renamed " << src << " to " << dst << std::endl;
        } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; }
    });

    // repair
    auto repair = app.add_subcommand("repair", "Repair filesystem allocation bitmap");
    repair->add_option("image", image_path, "Path to disk image")->required();
    repair->add_option("-v,--volume", volume_name, "Volume name");
    bool apply = false;
    repair->add_flag("--apply", apply, "Apply fixes to disk");
    repair->callback([&]() {
        try {
            auto img = DiskImage::open(image_path, !apply);
            std::vector<VolumeRef*> vols;
            if (volume_name.empty()) {
                for (auto& v : img->volumes()) vols.push_back(v.get());
            } else {
                auto vr = img->get_volume(volume_name);
                if (vr) vols.push_back(vr);
            }
            for (auto vol_ref : vols) {
                if (!vol_ref) continue;
                std::cout << "Repairing " << vol_ref->name() << "..." << std::endl;
                try {
                    uint32_t fixes = vol_ref->mount()->repair(apply);
                    std::cout << "  " << fixes << " fixes applied" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "  error: " << e.what() << std::endl;
                }
            }
        } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; }
    });

    // rdb-init
    auto rdb_init = app.add_subcommand("rdb-init", "Initialize an RDB table");
    rdb_init->add_option("image", image_path, "Path to disk image")->required();
    uint32_t sectors = 63, heads = 0, rdb_cyls = 0;
    rdb_init->add_option("--sectors", sectors, "Sectors per track (default: 63)");
    rdb_init->add_option("--heads", heads, "Heads (0 = auto)");
    rdb_init->add_option("--rdb-cyls", rdb_cyls, "Cylinders reserved for RDB (0 = auto)");
    rdb_init->callback([&]() {
        auto parsed = parse_image_arg(image_path);
        cmd_rdb_init(parsed.image, sectors, heads, rdb_cyls);
    });
    
    // part-add
    auto part_add = app.add_subcommand("part-add", "Add a partition to RDB");
    part_add->add_option("image", image_path, "Path to disk image")->required();
    std::string drv_name;
    part_add->add_option("name", drv_name, "Drive name (e.g. DH0)")->required();
    uint32_t part_size = 0;
    part_add->add_option("-s,--size", part_size, "Size in MB (0 = rest of disk)");
    part_add->add_option("-t,--dostype", dostype_str, "DOS type in hex (e.g. 444F5303)");
    uint32_t spb = 1;
    part_add->add_option("--bs", spb, "Block size in multiples of 512 (default: 1)");
    bool bootable = false;
    part_add->add_flag("--bootable", bootable, "Make bootable");
    int32_t boot_pri = 0;
    part_add->add_option("--pri", boot_pri, "Boot priority");
    part_add->callback([&]() {
        auto parsed = parse_image_arg(image_path);
        cmd_part_add(parsed.image, drv_name, part_size, dostype_str, spb, bootable, boot_pri);
    });

    // part-del
    auto part_del = app.add_subcommand("part-del", "Delete a partition from RDB");
    part_del->add_option("image", image_path, "Path to disk image")->required();
    part_del->add_option("name", drv_name, "Drive name (e.g. DH0)")->required();
    part_del->callback([&]() {
        auto parsed = parse_image_arg(image_path);
        cmd_part_del(parsed.image, drv_name);
    });

    // fs-add
    auto fs_add = app.add_subcommand("fs-add", "Embed a filesystem into RDB");
    fs_add->add_option("image", image_path, "Path to disk image")->required();
    std::string fs_path;
    fs_add->add_option("fs", fs_path, "Path to filesystem handler executable")->required();
    fs_add->add_option("-t,--dostype", dostype_str, "DOS type in hex")->required();
    uint16_t fs_ver = 0, fs_rev = 0;
    fs_add->add_option("--version", fs_ver, "Version (default: 0)");
    fs_add->add_option("--revision", fs_rev, "Revision (default: 0)");
    fs_add->callback([&]() {
        auto parsed = parse_image_arg(image_path);
        cmd_fs_add(parsed.image, fs_path, dostype_str, fs_ver, fs_rev);
    });

    // fs-extract
    auto fs_ext = app.add_subcommand("fs-extract", "Extract a filesystem from RDB");
    fs_ext->add_option("image", image_path, "Path to disk image")->required();
    fs_ext->add_option("-t,--dostype", dostype_str, "DOS type in hex")->required();
    std::string out_path;
    fs_ext->add_option("dest", out_path, "Destination file path")->required();
    fs_ext->callback([&]() {
        auto parsed = parse_image_arg(image_path);
        cmd_fs_extract(parsed.image, dostype_str, out_path);
    });
    
    // bootblock
    auto bb_cmd = app.add_subcommand("bootblock", "Install bootblock to partition");
    bb_cmd->add_option("image", image_path, "Path to disk image")->required();
    bb_cmd->add_option("-v,--volume", volume_name, "Volume name (default: DH0)");
    std::string bb_path;
    bb_cmd->add_option("bootcode", bb_path, "Path to bootcode binary")->required();
    bb_cmd->callback([&]() {
        auto parsed = parse_image_arg(image_path, volume_name);
        cmd_bootblock(parsed.image, parsed.vol, bb_path);
    });
    
    // rdb-scan
    auto scan_cmd = app.add_subcommand("rdb-scan", "Scan disk image for orphaned partitions");
    scan_cmd->add_option("image", image_path, "Path to disk image")->required();
    uint64_t rdb_area_secs = 0;
    scan_cmd->add_option("--rdb-area", rdb_area_secs, "Sectors to limit PART block search (default 4096)");
    scan_cmd->callback([&]() {
        auto parsed = parse_image_arg(image_path);
        cmd_rdb_scan(parsed.image, rdb_area_secs);
    });
    
    // rdb-rebuild
    auto rebuild_cmd = app.add_subcommand("rdb-rebuild", "Rebuild missing RDB from scanned partitions");
    rebuild_cmd->add_option("image", image_path, "Path to disk image")->required();
    rebuild_cmd->add_option("output", out_path, "Path to write modified image (if empty, modifies in place)");
    bool dry_run = false;
    rebuild_cmd->add_flag("--dry-run", dry_run, "Simulate the rebuild using a memory overlay without writing to disk");
    rebuild_cmd->callback([&]() {
        auto parsed = parse_image_arg(image_path);
        cmd_rdb_rebuild(parsed.image, out_path, dry_run);
    });

    if (argc == 1) {
        std::cout << app.help();
        return 0;
    }

    CLI11_PARSE(app, argc, argv);
    return 0;
}
