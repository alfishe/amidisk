#pragma once

#include "volume.h"
#include "../blkdev/blkdev.h"
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <span>
#include <functional>

namespace amidisk {

class SFSVolume;

class SFSError : public std::runtime_error {
public:
    explicit SFSError(const std::string& msg) : std::runtime_error(msg) {}
};

class SFSEntry {
public:
    std::string name;
    uint32_t objectnode = 0;
    uint32_t size = 0;
    uint32_t protect = 0;
    std::string comment;
    uint32_t secs = 0;
    uint8_t bits = 0;
    
    // Directory specific
    uint32_t hashtable = 0;
    uint32_t firstdirblock = 0;
    
    // File specific
    uint32_t data = 0;
    
    uint16_t uid = 0;
    uint16_t gid = 0;
    uint32_t objc_block = 0;
    uint32_t objc_offset = 0;

    bool is_dir() const { return (bits & 128) != 0; }
    bool is_link() const { return (bits & 64) != 0; }
    bool is_hardlink() const { return (bits & 32) != 0; }
    bool is_file() const { return !is_dir() && !is_link(); }

    std::string type_str() const;
    std::string name_str() const { return name; }
    std::chrono::system_clock::time_point mtime() const;
    std::string protect_str() const;
};

class SFSVolume : public Volume {
public:
    SFSVolume(std::shared_ptr<BlockDevice> blkdev, uint32_t dos_type = 0x53465300);

    void open() override;
    
    // Volume Interface
    std::string get_label() const override { return label_; }
    std::string dos_type_str() const override;
    bool is_read_only() const override { return read_only_; }
    VolumeInfo get_info() const override;
    std::shared_ptr<BlockDevice>& blkdev_ref() override { return blkdev_; }
    std::vector<Entry> list_dir(const std::string& path) override;
    void read_file(const std::string& path, std::function<void(std::span<const uint8_t>)> callback) override;
    CheckReport check(bool deep = false) override;

    // Write methods (unsupported for now)
    void format(const std::string& name, uint32_t dos_type = 0) override;
    void write_file(const std::string& path, std::span<const uint8_t> data, const WriteParams& params = {}) override;
    void mkdir(const std::string& path) override;
    void makedirs(const std::string& path) override;
    void delete_path(const std::string& path, bool recursive = false) override;
    void rename(const std::string& old_path, const std::string& new_path) override { throw std::runtime_error("Unsupported"); }
    uint32_t repair(bool apply = true) override { throw std::runtime_error("Unsupported"); }

    // SFS specific
    void walk(const std::string& path, std::function<void(const std::string&, const SFSEntry&)> callback);
    std::vector<SFSEntry> dir_entries(const SFSEntry& entry);
    SFSEntry resolve(const std::string& path);
    SFSEntry root_entry() const { return root_obj_; }

private:
    std::shared_ptr<BlockDevice> blkdev_;
    uint32_t dos_type_;
    bool read_only_;
    uint32_t spb_ = 1;
    uint32_t bs_ = 512;
    uint32_t total_ = 0;
    
    std::string label_;
    SFSEntry root_obj_;
    
    // Root block fields
    uint32_t datecreated_ = 0;
    uint8_t root_bits_ = 0;
    uint32_t totalblocks_ = 0;
    uint32_t blocksize_ = 0;
    uint32_t bitmapbase_ = 0;
    uint32_t adminspacecontainer_ = 0;
    uint32_t rootobjectcontainer_ = 0;
    uint32_t extentbnoderoot_ = 0;
    uint32_t objectnoderoot_ = 0;
    uint32_t freeblocks_ = 0;
    bool case_sensitive_ = false;

    std::vector<uint8_t> read_block(uint32_t blocknr, uint32_t count = 1);
    std::vector<uint8_t> read_checked(uint32_t blocknr, uint32_t want_id);
    static bool checksum_ok(const std::vector<uint8_t>& raw);
    
    SFSEntry parse_object(const std::vector<uint8_t>& raw, uint32_t off, uint32_t block);
    void iter_container_objects(const std::vector<uint8_t>& raw, uint32_t block, std::function<void(const SFSEntry&)> callback);
    
    std::pair<uint32_t, uint16_t> find_extent(uint32_t key);
    void read_data(const SFSEntry& entry, std::function<void(std::span<const uint8_t>)> callback);
    std::string read_softlink(const SFSEntry& entry);
    
    bool names_equal(const std::string& a, const std::string& b) const;
    
    // Write support low-level
    void require_writable() const;
    void write_block(uint32_t blocknr, std::vector<uint8_t>& raw);
    void fix_checksum(std::vector<uint8_t>& raw, uint32_t id_val, uint32_t blocknr);
    void touch_volume();

    // Allocation
    void mark_space(uint32_t start, uint32_t count, bool free);
    std::vector<std::pair<uint32_t, uint32_t>> alloc_contiguous_blocks(uint32_t count);
    std::vector<std::pair<uint32_t, uint32_t>> alloc_blocks(uint32_t count);
    uint32_t alloc_adminspace();
    uint32_t alloc_objectnode();
    
    // Extents
    void insert_bnode(const std::vector<uint8_t>& path, uint32_t key, const std::vector<uint8_t>& node_data);
    void add_extent(uint32_t start, uint32_t nxt, uint32_t prev, uint32_t blocks);
    std::pair<uint32_t, uint32_t> remove_extent(uint32_t key);
    
    // Object mutation
    std::pair<SFSEntry, std::string> split_parent(const std::string& path);
    std::vector<uint8_t> create_fsobject(const std::string& name, uint32_t size, uint32_t data, uint32_t objectnode, bool is_dir, const std::string& comment = "");
    void update_object_firstdirblock(const SFSEntry& entry);
    void insert_object(const SFSEntry& parent_entry, const std::vector<uint8_t>& obj_data);
    void remove_object(const SFSEntry& parent, const SFSEntry& entry);
    void free_adminspace(uint32_t block);
    void free_objectnode(uint32_t nodeno);
    void delete_file_data(const SFSEntry& entry);
    void delete_by_name(const SFSEntry& parent, const std::string& name, bool recursive);
    
    // Node / Hash
    uint32_t node_shift() const;
    void set_node(uint32_t nodeno, uint32_t data, uint32_t nxt, uint16_t h16);
    void hash_insert(const SFSEntry& parent, uint32_t nodeno, uint16_t h16);
    void hash_remove(const SFSEntry& parent, const SFSEntry& entry);
    
    struct NodePathEl { uint32_t blk; std::vector<uint8_t> raw; uint32_t idx; };
    std::tuple<std::vector<NodePathEl>, uint32_t, std::vector<uint8_t>, uint32_t> node_path(uint32_t nodeno);
    void update_full_bits(const std::vector<NodePathEl>& path, bool child_full);
};

} // namespace amidisk
