#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <span>
#include <functional>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <memory>

namespace amidisk {

class BlockDevice;

struct VolumeInfo {
    std::string label;
    std::string dos_type;
    std::string filesystem;
    uint64_t total_blocks = 0;
    uint64_t free_blocks = 0;
    uint64_t used_blocks = 0;
    uint64_t free_bytes = 0;
    uint32_t block_size = 512;
    uint64_t root_block = 0;
    bool read_only = true;
};

struct WriteParams {
    uint32_t protect = 0;
    std::string comment;
    int64_t mtime = 0;  // 0 = use current time; non-zero = explicit Unix timestamp
};

struct Entry {
    std::vector<uint8_t> name;
    uint32_t type = 0; // 2=dir, 1=file, others=link/etc
    uint64_t size = 0;
    std::vector<uint8_t> comment;
    uint32_t blk = 0;
    uint32_t hash_chain = 0;
    uint32_t protect = 0;
    int64_t mtime_unix = 0;  // Unix timestamp (seconds since epoch), 0 = unset

    bool is_dir() const { return type == 2; }
    bool is_file() const { return type == 1; }
    bool is_link() const { return type != 2 && type != 1; }
    
    std::string name_str() const {
        return std::string(reinterpret_cast<const char*>(name.data()), name.size());
    }
    std::string comment_str() const {
        return std::string(reinterpret_cast<const char*>(comment.data()), comment.size());
    }
    std::string protect_str() const {
        // Amiga protection bits:rwedspaD
        std::string s("--------");
        if (protect & 0x80) s[0] = 'D';  // delete
        if (protect & 0x40) s[1] = 'S';  // script
        if (protect & 0x20) s[2] = 'P';  // pure
        if (protect & 0x10) s[3] = 'A';  // archived
        if (!(protect & 0x08)) s[4] = 'r';  // readable
        if (!(protect & 0x04)) s[5] = 'w';  // writable
        if (!(protect & 0x02)) s[6] = 'e';  // executable
        if (!(protect & 0x01)) s[7] = 'd';  // deletable
        return s;
    }
    std::string mtime_str() const {
        if (mtime_unix == 0) return "----.--.-- --:--:--";
        std::time_t t = static_cast<std::time_t>(mtime_unix);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
};

struct CheckReport {
    bool ok = false;
    uint32_t files = 0;
    uint32_t dirs = 0;
    uint32_t used_blocks = 0;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

class Volume {
public:
    virtual ~Volume() = default;

    virtual std::string dos_type_str() const = 0;
    virtual std::string get_label() const = 0;
    virtual bool is_read_only() const = 0;

    virtual std::vector<Entry> list_dir(const std::string& path) = 0;
    virtual void read_file(const std::string& path, std::function<void(std::span<const uint8_t>)> callback) = 0;
    virtual CheckReport check(bool deep = false) = 0;

    virtual void open() = 0;

    virtual void format(const std::string& name, uint32_t dos_type = 0) = 0;
    virtual void write_file(const std::string& path, std::span<const uint8_t> data, const WriteParams& params = {}) = 0;
    virtual void mkdir(const std::string& path) = 0;
    virtual void makedirs(const std::string& path) = 0;
    virtual void delete_path(const std::string& path, bool recursive = false) = 0;
    virtual void rename(const std::string& old_path, const std::string& new_path) = 0;
    virtual uint32_t repair(bool apply = true) = 0;

    virtual VolumeInfo get_info() const = 0;

    // For bulk mode support - returns a reference to the internal blkdev
    virtual std::shared_ptr<BlockDevice>& blkdev_ref() = 0;
};

} // namespace amidisk
