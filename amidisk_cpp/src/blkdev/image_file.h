#pragma once

#include "blkdev.h"
#include <fstream>
#include <string>

namespace amidisk {

class ImageFileBlkDev : public BlockDevice {
public:
    ImageFileBlkDev(const std::string& path, bool read_only = true, uint32_t block_bytes = 512);
    ~ImageFileBlkDev() override;

    Geometry geometry() const override;
    uint32_t sector_size() const override;
    uint64_t size_bytes() const override;
    
    void read(uint64_t offset, std::span<uint8_t> buffer) const override;
    void write(uint64_t offset, std::span<const uint8_t> buffer) override;
    void flush() override;
    void close() override;

    DeviceKind kind() const override { return DeviceKind::Image; }
    bool is_read_only() const override { return read_only_; }

private:
    std::string path_;
    bool read_only_;
    uint32_t block_bytes_;
    uint64_t size_bytes_;
    Geometry geom_;
    mutable std::fstream file_;

    void infer_geometry();
};

} // namespace amidisk
