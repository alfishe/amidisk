#pragma once

#include "blkdev.h"
#include <memory>

namespace amidisk {

class PartBlockDevice : public BlockDevice {
public:
    PartBlockDevice(std::shared_ptr<BlockDevice> base, uint64_t start_block, uint64_t num_blocks, uint32_t block_bytes = 512);

    Geometry geometry() const override;
    uint32_t sector_size() const override;
    uint64_t size_bytes() const override;
    
    void read(uint64_t offset, std::span<uint8_t> buffer) const override;
    void write(uint64_t offset, std::span<const uint8_t> buffer) override;
    void flush() override;
    void close() override;

    DeviceKind kind() const override { return base_->kind(); }
    bool is_read_only() const override { return base_->is_read_only(); }

private:
    std::shared_ptr<BlockDevice> base_;
    uint64_t start_byte_;
    uint64_t size_bytes_;
    uint32_t block_bytes_;
};

} // namespace amidisk
