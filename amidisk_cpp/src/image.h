#pragma once

#include "blkdev/blkdev.h"
#include "rdb/rdisk.h"
#include "fs/volume.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace amidisk {

// ADF floppy sizes: DD (880KB), HD (1.76MB)
static constexpr uint64_t ADF_SIZE_DD  = 901120;
static constexpr uint64_t ADF_SIZE_HD  = 1802240;

class ImageError : public std::runtime_error {
public:
    explicit ImageError(const std::string& msg) : std::runtime_error(msg) {}
};

struct IdentifyInfo {
    std::string declared_dos_type;  // empty if none
    uint64_t size_bytes = 0;
    std::string boot_magic;         // printable form of first 4 bytes
    std::vector<std::string> guesses;

    nlohmann::json to_json() const;
};

class VolumeRef;

class DiskImage {
public:
    static std::unique_ptr<DiskImage> open(const std::string& path, bool read_only = true);

    DiskImage(std::shared_ptr<BlockDevice> blkdev, const std::string& path = "", bool writable = false);

    std::shared_ptr<BlockDevice> blkdev() const { return blkdev_; }
    RDisk* rdisk() const { return rdisk_.get(); }
    const std::vector<std::unique_ptr<VolumeRef>>& volumes() const { return volumes_; }
    const std::string& kind() const { return kind_; }
    const std::string& path() const { return path_; }
    bool writable() const { return writable_; }

    VolumeRef* get_volume(const std::string& name = "") const;
    std::pair<VolumeRef*, std::string> parse_path(const std::string& spec) const;
    nlohmann::json get_info() const;
    void close();

private:
    std::shared_ptr<BlockDevice> blkdev_;
    std::unique_ptr<RDisk> rdisk_;
    std::vector<std::unique_ptr<VolumeRef>> volumes_;
    std::string kind_;
    std::string path_;
    bool writable_ = false;

    void detect();
    std::unique_ptr<RDisk> probe_mbr_rdb();
};

// Engine selection helper — returns the right concrete engine for a dostype.
std::unique_ptr<Volume> create_engine_for_dostype(
    std::shared_ptr<BlockDevice> bdev, uint32_t dos_type,
    uint32_t sec_per_blk = 1, uint32_t reserved = 2);

class FFSVolume;

class VolumeRef {
public:
    VolumeRef(DiskImage* image, const std::string& name, const Partition* partition = nullptr);
    ~VolumeRef();

    const std::string& name() const { return name_; }
    const Partition* partition() const { return partition_; }

    Volume* mount();
    std::unique_ptr<Volume> raw_volume(uint32_t override_dos_type = 0) const;
    std::shared_ptr<BlockDevice> create_blkdev() const;
    std::string dos_type_str() const;

    // A3 enhancements
    std::string label() const;
    IdentifyInfo identify() const;

private:
    DiskImage* image_;
    std::string name_;
    const Partition* partition_;
    std::unique_ptr<Volume> vol_;
};

// Free function: best-effort identification of an unmountable volume
IdentifyInfo identify_volume(std::shared_ptr<BlockDevice> dev, uint32_t dos_type = 0);

} // namespace amidisk
