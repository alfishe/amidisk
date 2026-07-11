#pragma once

#include "../blkdev/blkdev.h"
#include <vector>
#include <memory>
#include <string>

namespace amidisk {

// Free functions
bool checksum_ok(std::span<const uint8_t> data, uint32_t num_longs);
void fix_checksum(std::span<uint8_t> data, uint32_t num_longs, uint32_t chk_loc = 2);
std::string dos_type_to_str(uint32_t dt);

class RDBBaseBlock {
public:
    RDBBaseBlock(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num);
    virtual ~RDBBaseBlock() = default;

    bool read();
    void write();

    uint64_t blk_num() const { return blk_num_; }
    bool is_valid() const { return valid_; }
    uint32_t size_longs() const { return size_longs_; }

    virtual const char* get_magic() const = 0;

    uint32_t get_long(uint32_t idx) const;
    int32_t get_slong(uint32_t idx) const;
    void put_long(uint32_t idx, uint32_t val);
    void put_slong(uint32_t idx, int32_t val);
    std::string get_bytes(uint32_t off, uint32_t size) const;
    std::string get_bstr(uint32_t off, uint32_t max_chars) const;
    void put_bstr(uint32_t off, uint32_t max_chars, const std::string& val);
    void put_padded(uint32_t off, uint32_t size, const std::string& text);

protected:
    std::shared_ptr<BlockDevice> blkdev_;
    uint64_t blk_num_;
    std::vector<uint8_t> data_;
    bool valid_;
    uint32_t size_longs_;

    virtual void unpack() = 0;
    virtual void pack() = 0;

    void init_new(uint32_t size_longs);
};

class RDBlock : public RDBBaseBlock {
public:
    RDBlock(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) : RDBBaseBlock(blkdev, blk_num) {}
    const char* get_magic() const override { return "RDSK"; }

    static std::unique_ptr<RDBlock> create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num);

    uint32_t host_id = 0;
    uint32_t block_bytes = 0;
    uint32_t flags = 0;
    uint32_t badblock_list = 0xFFFFFFFF;
    uint32_t partition_list = 0xFFFFFFFF;
    uint32_t fs_header_list = 0xFFFFFFFF;
    uint32_t drive_init = 0xFFFFFFFF;
    
    uint32_t cylinders = 0;
    uint32_t sectors = 0;
    uint32_t heads = 0;
    uint32_t interleave = 0;
    uint32_t park = 0;
    uint32_t write_precomp = 0;
    uint32_t reduced_write = 0;
    uint32_t step_rate = 0;

    uint32_t rdb_blocks_lo = 0;
    uint32_t rdb_blocks_hi = 0;
    uint32_t lo_cylinder = 0;
    uint32_t hi_cylinder = 0;
    uint32_t cyl_blocks = 0;
    uint32_t auto_park_seconds = 0;
    uint32_t high_rdsk_block = 0;

    std::string disk_vendor;
    std::string disk_product;
    std::string disk_revision;
    std::string ctrl_vendor;
    std::string ctrl_product;
    std::string ctrl_revision;

protected:
    void unpack() override;
    void pack() override;
};

struct DosEnvec {
    uint32_t table_size = 0;
    uint32_t size_block = 0;
    uint32_t sec_org = 0;
    uint32_t surfaces = 0;
    uint32_t sec_per_blk = 0;
    uint32_t blk_per_trk = 0;
    uint32_t reserved = 0;
    uint32_t pre_alloc = 0;
    uint32_t interleave = 0;
    uint32_t low_cyl = 0;
    uint32_t high_cyl = 0;
    uint32_t num_buffer = 0;
    uint32_t buf_mem_type = 0;
    uint32_t max_transfer = 0;
    uint32_t mask = 0;
    int32_t boot_pri = 0;
    uint32_t dos_type = 0;
    uint32_t baud = 0;
    uint32_t control = 0;
    uint32_t boot_blocks = 0;

    void parse(const RDBBaseBlock* blk, uint32_t base_long);
    void pack(RDBBaseBlock* blk, uint32_t base_long) const;

    uint32_t cyl_secs() const { return surfaces * blk_per_trk; }
    uint32_t num_cyls() const { return high_cyl - low_cyl + 1; }
    uint64_t start_sec() const { return static_cast<uint64_t>(low_cyl) * cyl_secs(); }
    uint64_t num_secs() const { return static_cast<uint64_t>(num_cyls()) * cyl_secs(); }
    std::string dos_type_str() const { return dos_type_to_str(dos_type); }
};

class PartitionBlock : public RDBBaseBlock {
public:
    PartitionBlock(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) : RDBBaseBlock(blkdev, blk_num) {}
    const char* get_magic() const override { return "PART"; }

    static std::unique_ptr<PartitionBlock> create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num);

    uint32_t host_id = 0;
    uint32_t next = 0xFFFFFFFF;
    uint32_t flags = 0;
    std::string drive_name;
    DosEnvec envec;

protected:
    void unpack() override;
    void pack() override;
};

class FSHeaderBlock : public RDBBaseBlock {
public:
    FSHeaderBlock(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) : RDBBaseBlock(blkdev, blk_num) {}
    const char* get_magic() const override { return "FSHD"; }

    static std::unique_ptr<FSHeaderBlock> create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num);

    uint32_t host_id = 0;
    uint32_t next = 0xFFFFFFFF;
    uint32_t flags = 0;
    uint16_t version = 0;
    uint16_t revision = 0;
    uint32_t patch_flags = 0;
    uint32_t type = 0;
    uint32_t task = 0;
    uint32_t lock = 0;
    uint32_t handler = 0;
    uint32_t stack_size = 0;
    int32_t priority = 0;
    int32_t startup = 0;
    int32_t seg_list_blk = 0;
    int32_t global_vec = 0;

protected:
    void unpack() override;
    void pack() override;
};

class LoadSegBlock : public RDBBaseBlock {
public:
    LoadSegBlock(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) : RDBBaseBlock(blkdev, blk_num) {}
    const char* get_magic() const override { return "LSEG"; }

    static std::unique_ptr<LoadSegBlock> create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num);

    uint32_t host_id = 0;
    uint32_t next = 0xFFFFFFFF;
    std::vector<uint8_t> load_data;

protected:
    void unpack() override;
    void pack() override;
};

class BadBlocksBlock : public RDBBaseBlock {
public:
    BadBlocksBlock(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) : RDBBaseBlock(blkdev, blk_num) {}
    const char* get_magic() const override { return "BADB"; }

    static std::unique_ptr<BadBlocksBlock> create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num);

    uint32_t host_id = 0;
    uint32_t next = 0xFFFFFFFF;

protected:
    void unpack() override;
    void pack() override;
};

} // namespace amidisk
