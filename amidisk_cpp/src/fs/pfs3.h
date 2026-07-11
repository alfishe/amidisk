#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <optional>

#include "../blkdev/blkdev.h"
#include "../image.h"

namespace amidisk {

class PFS3Error : public std::runtime_error {
public:
    explicit PFS3Error(const std::string& msg) : std::runtime_error(msg) {}
};

struct PFS3Entry {
    std::string name;
    int8_t type;
    uint32_t anode;
    uint64_t size;
    uint32_t protect;
    std::string comment;
    uint16_t days;
    uint16_t mins;
    uint16_t ticks;
    uint32_t link;
    uint16_t uid;
    uint16_t gid;

    bool is_dir() const;
    bool is_file() const;
    bool is_link() const;
    std::string type_str() const;
    std::string name_str() const { return name; }
    std::string comment_str() const { return comment; }
};

class PFS3Volume : public Volume {
public:
    PFS3Volume(std::shared_ptr<BlockDevice> dev, uint32_t sec_per_blk = 1, uint32_t reserved = 2, uint32_t dos_type = 0);
    ~PFS3Volume() override = default;

    void open() override;
    
    std::string dos_type_str() const override;
    std::string get_label() const override { return label_; }
    bool is_read_only() const override { return read_only_; }
    VolumeInfo get_info() const override;
    std::shared_ptr<BlockDevice>& blkdev_ref() override { return dev_; }

    std::vector<Entry> list_dir(const std::string& path) override;
    void read_file(const std::string& path, std::function<void(std::span<const uint8_t>)> callback) override;
    CheckReport check(bool deep = false) override;
    void walk(const std::string& path, std::function<void(const std::string&, const PFS3Entry&)> callback);
    
    // Write methods (unsupported for now)
    void format(const std::string& name, uint32_t dos_type = 0) override;
    void write_file(const std::string& path, std::span<const uint8_t> data, const WriteParams& params = {}) override;
    void mkdir(const std::string& path) override;
    void makedirs(const std::string& path) override;
    void delete_path(const std::string& path, bool recursive = false) override;
    void rename(const std::string& old_path, const std::string& new_path) override { throw std::runtime_error("Unsupported"); }
    uint32_t repair(bool apply = true) override { throw std::runtime_error("Unsupported"); }

private:
    std::shared_ptr<BlockDevice> dev_;
    uint32_t spb_;
    uint32_t bytes_per_block_;
    uint64_t total_;
    uint32_t dos_type_;
    bool read_only_;
    std::string label_;
    
    uint32_t disktype_;
    uint32_t options_;
    uint32_t datestamp_;
    uint32_t lastreserved_;
    uint32_t firstreserved_;
    uint32_t reserved_free_;
    uint16_t reserved_blksize_;
    uint16_t rblkcluster_;
    uint32_t blocksfree_;
    uint32_t alwaysfree_;
    uint32_t roving_ptr_;
    uint32_t deldir_;
    uint32_t disksize_;
    uint32_t extension_;
    
    uint32_t rescluster_;
    uint32_t anodes_per_block_;
    uint32_t index_per_block_;
    bool super_index_;
    bool split_anodes_;
    bool large_file_;
    
    std::vector<uint8_t> root_raw_;
    std::vector<uint32_t> small_indexblocks_;
    std::vector<uint32_t> superindex_tab_;
    
    std::unordered_map<uint32_t, std::vector<uint8_t>> index_cache_;
    
    struct AnodeData {
        uint32_t cs;
        uint32_t bn;
        uint32_t nxt;
    };
    std::unordered_map<uint32_t, AnodeData> anode_cache_;

    // Transaction & Caching variables
    int bulk_depth_ = 0;
    int bulk_every_ = 1;
    int bulk_ops_ = 0;
    uint32_t roving_ = 0;
    
    std::unordered_map<uint32_t, std::pair<uint32_t, std::vector<uint8_t>>> bmb_cache_;
    std::vector<uint32_t> bmb_dirty_; // Store seqnr
    
    std::unordered_map<uint32_t, std::pair<uint32_t, std::vector<uint8_t>>> ab_cache_;
    std::vector<uint32_t> ab_dirty_; // Store seqnr
    
    // Directory caches
    struct DirTail {
        uint32_t nr;
        uint32_t bn;
        uint32_t end;
    };
    std::unordered_map<uint32_t, DirTail> dir_tail_;
    std::unordered_map<uint32_t, std::unordered_set<std::string>> dir_index_;
    std::string last_split_key_;
    std::optional<PFS3Entry> last_split_parent_;
    std::string last_makedirs_path_;

    bool has_rext_ = false;
    std::vector<uint8_t> rext_raw_;

    std::vector<uint8_t> read_blocks(uint64_t blocknr, uint32_t count);
    std::vector<uint8_t> read_reserved(uint64_t blocknr);
    
    void write_raw(uint64_t blocknr, std::span<const uint8_t> data);
    void write_reserved(uint64_t blocknr, std::vector<uint8_t>& data);
    
    void require_writable() const;
    void begin();
    void commit(bool touch_root_date = true);
    void flush_meta(bool touch_root_date);
    void drop_txn_caches();
    
    const std::vector<uint8_t>& get_index_block(uint32_t nr);
    AnodeData get_anode(uint32_t anodenr);
    std::vector<std::pair<uint32_t, uint32_t>> anode_chain(uint32_t anodenr);
    
    // Bitmap and Allocation
    std::pair<uint8_t*, size_t> res_bitmap_view();
    uint32_t numreserved() const;
    uint32_t alloc_reserved();
    void free_reserved(uint32_t blocknr);
    
    uint32_t lpb() const;
    uint32_t bitmapstart() const;
    uint32_t bitmapindex_capacity() const;
    uint32_t get_bitmapindex_blocknr(uint32_t nr);
    std::pair<uint32_t, std::vector<uint8_t>>& get_bmb(uint32_t seqnr);
    
    struct MainBit {
        uint32_t seq;
        uint32_t off;
        uint32_t mask;
    };
    MainBit main_bit(uint32_t blocknr) const;
    bool block_is_free(uint32_t blocknr);
    void set_main_bit(uint32_t blocknr, bool free);
    
    std::vector<std::pair<uint32_t, uint32_t>> alloc_main(uint32_t count);
    void free_main_run(uint32_t start, uint32_t length);
    
    // Anode Allocation
    uint32_t anode_index_blocknr(uint32_t inr, bool create = false);
    std::pair<uint32_t, std::vector<uint8_t>>& anode_block(uint32_t seqnr, bool create = false);
    void save_anode(uint32_t anodenr, uint32_t cs, uint32_t bn, uint32_t nxt);
    std::optional<uint32_t> try_alloc_anode_in(uint32_t s);
    uint32_t alloc_anode(uint32_t hint_seqnr = 0);
    void free_anode(uint32_t anodenr);
    
    // Directory Mutations
    std::vector<uint8_t> build_direntry(int8_t etype, uint32_t anode, uint32_t fsize, const std::string& name, const std::string& comment = "", uint32_t protect = 0);
    uint32_t entries_end(const std::vector<uint8_t>& raw);
    std::string entry_name(const std::vector<uint8_t>& entry_bytes);
    void add_entry(const PFS3Entry& dir_entry, const std::vector<uint8_t>& entry_bytes);
    uint32_t dirblock_parent(uint32_t dir_anodenr);
    std::string upper_key(const std::string& name) const;
    std::unordered_set<std::string>& dir_names(uint32_t dir_anodenr);
    void dir_cache_drop(std::optional<uint32_t> dir_anodenr = std::nullopt);
    
    struct DirLocation {
        uint32_t bn;
        uint32_t off;
        PFS3Entry entry;
    };
    std::optional<DirLocation> locate_entry(uint32_t dir_anodenr, const std::string& name);
    PFS3Entry remove_entry(uint32_t dir_anodenr, const std::string& name);
    PFS3Entry resolve_dir(const std::string& path);
    std::pair<PFS3Entry, std::string> split_parent(const std::string& path);
    void delete_file_data(const PFS3Entry& entry);
    
    PFS3Entry parse_direntry(const std::vector<uint8_t>& raw, uint32_t off);
    std::vector<PFS3Entry> dir_entries(uint32_t anodenr);
    
    PFS3Entry root_entry();
    PFS3Entry resolve(const std::string& path);
};

} // namespace amidisk
