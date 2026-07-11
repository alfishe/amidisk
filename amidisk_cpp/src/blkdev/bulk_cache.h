#pragma once

#include "blkdev.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <tuple>

namespace amidisk {

/// Write-back cache for bulk imports: absorbs small metadata and file writes
/// into a per-block dict, coalescing them into large sorted sequential flushes.
/// This is the C++ equivalent of Python's BulkWriteCache.
class BulkWriteCache : public BlockDevice {
public:
    BulkWriteCache(std::shared_ptr<BlockDevice> base, uint64_t flush_threshold = 512ULL << 20);
    ~BulkWriteCache() override;

    Geometry geometry() const override;
    uint32_t sector_size() const override;
    uint64_t size_bytes() const override;

    void read(uint64_t offset, std::span<uint8_t> buffer) const override;
    void write(uint64_t offset, std::span<const uint8_t> buffer) override;
    void flush() override;
    void close() override;

    DeviceKind kind() const override { return DeviceKind::Overlay; }
    bool is_read_only() const override { return base_->is_read_only(); }

    void flush_cache();
    uint64_t pending_bytes() const { return pending_; }

    std::shared_ptr<BlockDevice> base() const { return base_; }

private:
    std::shared_ptr<BlockDevice> base_;
    uint32_t block_bytes_;

    // Small writes: block index -> block data
    mutable std::unordered_map<uint64_t, std::vector<uint8_t>> small_;

    // Large writes: sorted (start_block, num_blocks, data)
    mutable std::vector<std::tuple<uint64_t, uint64_t, std::vector<uint8_t>>> big_;
    mutable std::vector<uint64_t> big_starts_;

    uint64_t pending_ = 0;
    uint64_t flush_threshold_;

    // PERF: Reusable buffer for flush_cache to avoid repeated allocations
    mutable std::vector<uint8_t> flush_buffer_;

    // Look up a single block in the big-write cache
    const uint8_t* big_lookup(uint64_t blk) const;
};

/// RAII guard that swaps a block device reference with a BulkWriteCache,
/// flushes on destruction, and restores the original device.
class BulkGuard {
public:
    explicit BulkGuard(std::shared_ptr<BlockDevice>& target);
    ~BulkGuard();

    BulkGuard(const BulkGuard&) = delete;
    BulkGuard& operator=(const BulkGuard&) = delete;

private:
    std::shared_ptr<BlockDevice>& target_;
    std::shared_ptr<BlockDevice> original_;
};

} // namespace amidisk
