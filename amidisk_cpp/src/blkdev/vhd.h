#pragma once

#include "blkdev.h"
#include <fstream>
#include <string>
#include <vector>

namespace amidisk {

class VHDBlkDev : public BlockDevice {
public:
    VHDBlkDev(const std::string& path, bool read_only = true);
    ~VHDBlkDev() override;

    Geometry geometry() const override;
    uint32_t sector_size() const override;
    uint64_t size_bytes() const override;
    
    void read(uint64_t offset, std::span<uint8_t> buffer) const override;
    void write(uint64_t offset, std::span<const uint8_t> buffer) override;
    void flush() override;
    void close() override;

    DeviceKind kind() const override { return DeviceKind::Vhd; }
    bool is_read_only() const override { return read_only_; }

private:
    std::string path_;
    bool read_only_;
    uint32_t disk_type_;
    uint64_t current_size_;
    uint64_t num_blocks_;
    mutable std::fstream file_;

    // For dynamic VHDs
    uint32_t dyn_block_size_;
    uint32_t bitmap_secs_;
    std::vector<uint32_t> bat_;

    void parse_dynamic(std::span<const uint8_t> footer);
};

} // namespace amidisk
