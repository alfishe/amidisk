#pragma once

#include "blocks.h"
#include <nlohmann/json.hpp>
#include <vector>

namespace amidisk {

class RDiskError : public std::runtime_error {
public:
    explicit RDiskError(const std::string& msg) : std::runtime_error(msg) {}
};

class Partition {
public:
    Partition(std::shared_ptr<BlockDevice> blkdev, std::unique_ptr<PartitionBlock> part_blk, uint32_t num);
    
    std::shared_ptr<BlockDevice> blkdev() const { return blkdev_; }
    const PartitionBlock& part_blk() const { return *part_blk_; }
    uint32_t num() const { return num_; }
    const std::string& drv_name() const { return drv_name_; }
    const DosEnvec& dos_env() const { return dos_env_; }

    uint32_t get_num_cyls() const { return dos_env_.num_cyls(); }
    uint64_t get_num_secs() const { return dos_env_.num_secs(); }
    uint64_t get_num_blocks() const;
    uint32_t get_block_size() const;
    uint64_t get_byte_offset() const;
    uint64_t get_byte_size() const;
    std::string get_dos_type_str() const { return dos_env_.dos_type_str(); }
    bool is_bootable() const;

    std::shared_ptr<BlockDevice> create_blkdev() const;
    nlohmann::json get_info() const;

private:
    std::shared_ptr<BlockDevice> blkdev_;
    std::unique_ptr<PartitionBlock> part_blk_;
    uint32_t num_;
    std::string drv_name_;
    DosEnvec dos_env_;
};

class FileSystem {
public:
    FileSystem(std::shared_ptr<BlockDevice> blkdev, std::unique_ptr<FSHeaderBlock> fshd_blk, uint32_t num);

    const FSHeaderBlock& fshd_blk() const { return *fshd_blk_; }
    uint32_t num() const { return num_; }

    std::string get_dos_type_str() const;
    std::string get_version_string() const;
    std::vector<uint8_t> get_data() const;
    nlohmann::json get_info() const;

private:
    std::shared_ptr<BlockDevice> blkdev_;
    std::unique_ptr<FSHeaderBlock> fshd_blk_;
    uint32_t num_;
};

class RDisk {
public:
    static std::unique_ptr<RDisk> peek(std::shared_ptr<BlockDevice> blkdev);
    static std::unique_ptr<RDisk> open(std::shared_ptr<BlockDevice> blkdev);
    static std::unique_ptr<RDisk> create(std::shared_ptr<BlockDevice> blkdev, uint32_t sectors = 63, uint32_t heads = 0, uint32_t rdb_cyls = 0);

    RDisk(std::shared_ptr<BlockDevice> blkdev, std::unique_ptr<RDBlock> rdsk);

    const RDBlock* rdsk() const { return rdsk_.get(); }
    const std::vector<std::unique_ptr<Partition>>& partitions() const { return partitions_; }
    const std::vector<std::unique_ptr<FileSystem>>& filesystems() const { return filesystems_; }

    Partition* add_partition(const std::string& drv_name, uint64_t size_bytes = 0, uint32_t dos_type = 0x444f5303, uint32_t sec_per_blk = 1, bool bootable = false, int32_t boot_pri = 0);
    Partition* add_partition_exact(const std::string& drv_name, uint32_t low_cyl, uint32_t high_cyl, uint32_t dos_type = 0x444f5303, uint32_t sec_per_blk = 1, bool bootable = false, int32_t boot_pri = 0);
    void delete_partition(const std::string& drv_name);
    
    void add_filesystem(const std::string& path, uint32_t dos_type, uint16_t version, uint16_t revision);
    void extract_filesystem(uint32_t dos_type, const std::string& out_path) const;
    
    // Internal access to the block for testing/validation
    const RDBlock& rdsk_blk() const { return *rdsk_; }

private:
    std::shared_ptr<BlockDevice> blkdev_;
    std::unique_ptr<RDBlock> rdsk_;
    std::vector<std::unique_ptr<Partition>> partitions_;
    std::vector<std::unique_ptr<FileSystem>> filesystems_;

    void scan();
    uint64_t allocate_block();
};

} // namespace amidisk
