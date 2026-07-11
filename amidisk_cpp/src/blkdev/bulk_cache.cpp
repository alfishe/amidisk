#include "bulk_cache.h"
#include <algorithm>
#include <cstring>

namespace amidisk {

// ---------------------------------------------------------------------------
//  BulkWriteCache
// ---------------------------------------------------------------------------

BulkWriteCache::BulkWriteCache(std::shared_ptr<BlockDevice> base, uint64_t flush_threshold)
    : base_(std::move(base))
    , block_bytes_(base_->sector_size())
    , flush_threshold_(flush_threshold)
{
}

BulkWriteCache::~BulkWriteCache() {
    try { flush_cache(); } catch (...) {}
}

Geometry BulkWriteCache::geometry() const {
    return base_->geometry();
}

uint32_t BulkWriteCache::sector_size() const {
    return base_->sector_size();
}

uint64_t BulkWriteCache::size_bytes() const {
    return base_->size_bytes();
}

const uint8_t* BulkWriteCache::big_lookup(uint64_t blk) const {
    // Binary search in big_starts_ for the segment containing blk
    auto it = std::upper_bound(big_starts_.begin(), big_starts_.end(), blk);
    if (it == big_starts_.begin()) return nullptr;
    size_t idx = static_cast<size_t>(it - big_starts_.begin()) - 1;
    if (idx >= big_.size()) return nullptr;
    const auto& [s, n, data] = big_[idx];
    if (blk >= s && blk < s + n) {
        uint64_t off = (blk - s) * block_bytes_;
        if (off < data.size()) return data.data() + off;
    }
    return nullptr;
}

void BulkWriteCache::write(uint64_t offset, std::span<const uint8_t> buffer) {
    uint64_t bb = block_bytes_;
    uint64_t n = buffer.size() / bb;
    uint64_t start_blk = offset / bb;

    if (n <= 8) {
        // Small write: store per-block
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t blk = start_blk + i;
            std::vector<uint8_t> block_data(bb);
            std::memcpy(block_data.data(), buffer.data() + i * bb, bb);
            small_[blk] = std::move(block_data);
        }
    } else {
        // Large write: store whole segment
        // Remove any small entries that overlap
        for (uint64_t b = start_blk; b < start_blk + n; ++b) {
            small_.erase(b);
        }

        std::vector<uint8_t> seg_data(buffer.begin(), buffer.end());
        // Insert sorted by start_blk
        auto it = std::upper_bound(big_starts_.begin(), big_starts_.end(), start_blk);
        size_t idx = static_cast<size_t>(it - big_starts_.begin());
        big_.insert(big_.begin() + idx, {start_blk, n, std::move(seg_data)});
        big_starts_.insert(big_starts_.begin() + idx, start_blk);
    }

    pending_ += buffer.size();
    if (pending_ >= flush_threshold_) {
        flush_cache();
    }
}

void BulkWriteCache::read(uint64_t offset, std::span<uint8_t> buffer) const {
    uint64_t bb = block_bytes_;
    uint64_t n = buffer.size() / bb;
    uint64_t start_blk = offset / bb;

    if (small_.empty() && big_.empty()) {
        base_->read(offset, buffer);
        return;
    }

    if (n <= 256) {
        // Per-block check: serve from cache where available
        bool any_cached = false;
        bool any_uncached = false;

        // Build output
        std::vector<uint8_t> result(buffer.size());
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t blk = start_blk + i;
            auto sit = small_.find(blk);
            const uint8_t* big_data = nullptr;

            if (sit != small_.end()) {
                std::memcpy(result.data() + i * bb, sit->second.data(), bb);
                any_cached = true;
            } else if ((big_data = big_lookup(blk)) != nullptr) {
                std::memcpy(result.data() + i * bb, big_data, bb);
                any_cached = true;
            } else {
                any_uncached = true;
            }
        }

        if (any_cached && !any_uncached) {
            // All from cache
            std::memcpy(buffer.data(), result.data(), buffer.size());
            return;
        }
        if (!any_cached) {
            // All from base
            base_->read(offset, buffer);
            return;
        }
        // Mixed: read from base, then overlay cached blocks
        base_->read(offset, buffer);
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t blk = start_blk + i;
            auto sit = small_.find(blk);
            const uint8_t* big_data = nullptr;
            if (sit != small_.end()) {
                std::memcpy(buffer.data() + i * bb, sit->second.data(), bb);
            } else if ((big_data = big_lookup(blk)) != nullptr) {
                std::memcpy(buffer.data() + i * bb, big_data, bb);
            }
        }
        return;
    }

    // Large read: cheapest correct path is flush-then-read-through
    const_cast<BulkWriteCache*>(this)->flush_cache();
    base_->read(offset, buffer);
}

void BulkWriteCache::flush_cache() {
    if (small_.empty() && big_.empty()) return;

    uint64_t bb = block_bytes_;

    // Merge small and big entries, sort by start block
    struct Entry {
        uint64_t start;
        uint64_t nblocks;
        std::vector<uint8_t> data;
    };

    std::vector<Entry> items;
    items.reserve(small_.size() + big_.size());

    for (auto& [blk, data] : small_) {
        items.push_back({blk, 1, std::move(data)});
    }
    for (auto& [s, n, data] : big_) {
        items.push_back({s, n, std::move(data)});
    }

    std::sort(items.begin(), items.end(),
              [](const Entry& a, const Entry& b) { return a.start < b.start; });

    // Coalesce consecutive entries into large writes
    constexpr uint64_t MAXRUN = 32ULL << 20;  // 32 MB
    size_t i = 0;
    while (i < items.size()) {
        uint64_t start = items[i].start;
        std::vector<uint8_t> combined;
        combined.reserve(items[i].data.size());
        combined.insert(combined.end(), items[i].data.begin(), items[i].data.end());
        uint64_t end = items[i].start + items[i].nblocks;

        size_t j = i + 1;
        while (j < items.size() &&
               items[j].start == end &&
               combined.size() + items[j].data.size() <= MAXRUN) {
            combined.insert(combined.end(), items[j].data.begin(), items[j].data.end());
            end += items[j].nblocks;
            j++;
        }

        base_->write(start * bb, combined);
        i = j;
    }

    small_.clear();
    big_.clear();
    big_starts_.clear();
    pending_ = 0;
}

void BulkWriteCache::flush() {
    flush_cache();
    base_->flush();
}

void BulkWriteCache::close() {
    flush_cache();
    base_->close();
}

// ---------------------------------------------------------------------------
//  BulkGuard
// ---------------------------------------------------------------------------

BulkGuard::BulkGuard(std::shared_ptr<BlockDevice>& target)
    : target_(target)
    , original_(target)
{
    target_ = std::make_shared<BulkWriteCache>(original_);
}

BulkGuard::~BulkGuard() {
    auto* cache = dynamic_cast<BulkWriteCache*>(target_.get());
    if (cache) {
        cache->flush_cache();
    }
    target_ = original_;
}

} // namespace amidisk
