#pragma once

#include "blkdev.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <set>

namespace amidisk {

class OverlayBlockDevice : public BlockDevice {
public:
    OverlayBlockDevice(std::shared_ptr<BlockDevice> base, uint64_t max_ram_bytes = 256ULL * 1024 * 1024);
    ~OverlayBlockDevice() override;

    Geometry geometry() const override;
    uint32_t sector_size() const override;
    uint64_t size_bytes() const override;
    
    void read(uint64_t offset, std::span<uint8_t> buffer) const override;
    void write(uint64_t offset, std::span<const uint8_t> buffer) override;
    void flush() override;
    void close() override;

    DeviceKind kind() const override { return DeviceKind::Overlay; }
    bool is_read_only() const override { return false; }

    std::vector<uint64_t> dirty_blocks() const;
    uint64_t dirty_bytes() const;
    
    void commit(BlockDevice& target);
    std::vector<uint64_t> verify_committed(const BlockDevice& target) const;

private:
    std::shared_ptr<BlockDevice> base_;
    uint64_t max_ram_blocks_;
    
    // RAM cache: block index -> sector data
    mutable std::unordered_map<uint64_t, std::vector<uint8_t>> ram_;
    
    // Spill file handling
    mutable std::FILE* spill_file_;
    mutable std::unordered_map<uint64_t, uint64_t> spill_idx_;

    bool get_dirty(uint64_t lba, std::span<uint8_t> out) const;
    void put_dirty(uint64_t lba, std::span<const uint8_t> block);
};

} // namespace amidisk
