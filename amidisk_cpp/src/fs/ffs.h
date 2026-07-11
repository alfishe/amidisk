#pragma once

#include "../blkdev/blkdev.h"
#include "volume.h"
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <set>
#include <functional>

namespace amidisk {

class FSError : public std::runtime_error {
public:
    explicit FSError(const std::string& msg) : std::runtime_error(msg) {}
};

constexpr uint32_t T_HEADER = 2;
constexpr uint32_t T_DATA = 8;
constexpr uint32_t T_LIST = 16;
constexpr uint32_t T_DIRCACHE = 33;
constexpr uint32_t T_COMMENT = 64;

constexpr int32_t ST_ROOT = 1;
constexpr int32_t ST_USERDIR = 2;
constexpr int32_t ST_SOFTLINK = 3;
constexpr int32_t ST_LINKDIR = 4;
constexpr int32_t ST_FILE = -3;
constexpr int32_t ST_LINKFILE = -4;

constexpr uint32_t MAX_NAME = 30;
constexpr uint32_t MAX_COMMENT = 79;
constexpr uint32_t MAX_CHAIN = 1000000;

class BlockBuf {
public:
    BlockBuf(uint32_t bs);
    BlockBuf(std::span<const uint8_t> data, uint32_t bs);

    uint32_t long_val(int32_t idx) const;
    int32_t slong_val(int32_t idx) const;
    void put_long(int32_t idx, uint32_t val);
    void put_slong(int32_t idx, int32_t val);

    std::string bstr(uint32_t byte_off, uint32_t max_chars) const;
    void put_bstr(uint32_t byte_off, uint32_t max_chars, const std::string& val);

    uint32_t sum_longs() const;
    void fix_checksum(int32_t chk_long = 5);
    bool checksum_ok() const;

    std::vector<uint8_t> data_;
    uint32_t bs_;
    int32_t nl_;

private:
    uint32_t off(int32_t idx) const;
};

class FFSVolume;

struct FFSEntry {
    uint64_t blk = 0;
    uint32_t type = 0;
    int32_t sec_type = 0;
    std::vector<uint8_t> name;
    uint32_t size = 0;
    uint32_t protect = 0;
    std::vector<uint8_t> comment;
    uint32_t days = 0;
    uint32_t mins = 0;
    uint32_t ticks = 0;
    uint32_t hash_chain = 0;
    uint32_t parent = 0;
    uint32_t extension = 0;
    uint32_t high_seq = 0;
    uint32_t first_data = 0;
    uint32_t real_entry = 0;
    uint16_t uid = 0;
    uint16_t gid = 0;
    uint32_t comment_block = 0;

    static FFSEntry parse(const BlockBuf& buf, uint64_t blk, bool is_longname = false);
    
    bool is_dir() const;
    bool is_file() const;
    bool is_link() const;
    std::string type_str() const;
    std::string name_str() const;
    std::string comment_str() const;
    std::string protect_str() const;
    std::chrono::system_clock::time_point mtime() const;
};

class Bitmap {
public:
    Bitmap(FFSVolume* vol);

    void load();
    uint32_t alloc();
    std::vector<uint32_t> alloc_runs(uint32_t count);
    void free(uint32_t blk);
    void set_used(uint32_t blk);
    bool is_free(uint32_t blk) const;
    uint32_t count_free() const;
    void flush();  // Write all dirty pages to disk

private:
    friend class FFSVolume;
    FFSVolume* vol_;
    uint32_t bits_per_page_;
    std::vector<uint32_t> page_blks_;
    std::vector<BlockBuf> pages_;
    mutable std::optional<uint32_t> free_count_;
    // PERF: Deferred bitmap writes - instead of writing bitmap pages after
    // every allocation, we mark pages dirty and flush() writes them all at
    // once, coalescing consecutive pages into single I/O operations.
    std::set<uint32_t> dirty_;

    // PERF: Allocation cursor - resumes scanning from where last allocation
    // ended, avoiding O(total_blocks) scans from the beginning each time.
    // Critical for large volumes: 27GB @ 4K = 7M blocks to scan per alloc.
    uint32_t cursor_ = 0;

    std::pair<uint32_t, uint32_t> locate(uint32_t blk) const;
};

class FFSVolume : public Volume {
public:
    FFSVolume(std::shared_ptr<BlockDevice> blkdev, uint32_t sec_per_blk = 1, uint32_t reserved = 2, std::optional<uint32_t> dos_type = std::nullopt);

    void open() override;
    
    FFSEntry root_entry();
    FFSEntry resolve(const std::string& path);
    std::vector<uint32_t> data_block_table(const FFSEntry& entry);
    std::vector<Entry> list_dir(const std::string& path = "") override;
    void read_file(const std::string& path, std::function<void(std::span<const uint8_t>)> callback) override;
    std::vector<uint8_t> read_file_bytes(const std::string& path);
    std::vector<std::pair<uint64_t, uint32_t>> data_runs(const FFSEntry& entry);
    void walk(const std::string& path, std::function<void(const std::string& prefix, const FFSEntry& e)> callback);
    CheckReport check(bool deep = false) override;
    uint32_t repair(bool apply = true) override;
    
    // Write methods
    void format(const std::string& name, uint32_t dos_type = 0) override;
    void write_file(const std::string& path, std::span<const uint8_t> data, const WriteParams& params = {}) override;
    void mkdir(const std::string& path) override;
    void makedirs(const std::string& path) override;
    void delete_path(const std::string& path, bool recursive = false) override;
    void rename(const std::string& old_path, const std::string& new_path) override;
    
    void write_buf(uint64_t blk, const BlockBuf& buf);
    void now_stamp(BlockBuf& buf, int32_t long_offset, int64_t unix_ts = 0);
    
    std::pair<FFSEntry, std::string> resolve_parent(const std::string& path);
    BlockBuf new_header(uint32_t blk, int32_t sec_type, uint32_t parent_blk, const std::string& name, uint32_t protect = 0, const std::string& comment = "");
    void link_entry(uint32_t dir_blk, uint32_t new_blk, const std::string& name, BlockBuf* new_buf = nullptr);

    bool is_read_only() const override { return read_only_; }
    std::string get_label() const override { return label; }
    std::string dos_type_str() const override;
    VolumeInfo get_info() const override;
    std::shared_ptr<BlockDevice>& blkdev_ref() override { return dev; }

    std::shared_ptr<BlockDevice> dev;
    uint32_t spb;
    uint32_t bs;
    uint32_t nl;
    uint32_t tsz;
    uint32_t total;
    uint32_t reserved;
    uint64_t root_blk;
    bool read_only_;
    std::optional<uint32_t> dos_type;
    bool ffs = false;
    bool intl = false;
    bool dircache = false;
    bool is_longname = false;
    uint32_t max_name_len = 30;
    std::string label;
    std::unique_ptr<Bitmap> bitmap;

    BlockBuf read_buf(uint64_t blk);
    void write_buf(uint64_t blk, BlockBuf& buf);

private:
    std::vector<std::string> split(const std::string& path) const;
    std::string fold(const std::string& name) const;

    std::optional<uint64_t> last_blk_;
    std::unique_ptr<BlockBuf> last_buf_;

    // PERF: makedirs() optimization - caches directory block numbers to avoid
    // O(depth²) resolve() calls. last_makedirs_path_ skips work when called
    // repeatedly with the same path (common in tar extraction). dir_cache_
    // maps path strings to block numbers for O(1) existence checks.
    std::string last_makedirs_path_;
    std::unordered_map<std::string, uint32_t> dir_cache_;

    // PERF: resolve_parent() optimization - tar extraction writes many files
    // to the same directory consecutively. Caching the last resolved parent
    // avoids O(depth) resolve() calls for each file in the same dir.
    std::string last_parent_path_;
    FFSEntry last_parent_entry_;

    std::unordered_set<std::string> dir_name_set(const BlockBuf& dir_buf);
    std::optional<FFSEntry> find_in_dir(const BlockBuf& dir_buf, const std::string& name);
    std::vector<FFSEntry> list_entries(uint64_t dir_blk, bool sort = true);
    void fill_comment(FFSEntry& e);
    
    void unlink_entry(const FFSEntry& entry);
    void touch_dir(uint32_t dir_blk);
    std::vector<uint32_t> entry_blocks(const FFSEntry& entry);
};

} // namespace amidisk
