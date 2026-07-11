#include "overlay.h"
#include <algorithm>
#include <cstring>

namespace amidisk {

OverlayBlockDevice::OverlayBlockDevice(std::shared_ptr<BlockDevice> base, uint64_t max_ram_bytes)
    : base_(base), spill_file_(nullptr) {
    max_ram_blocks_ = max_ram_bytes / base_->sector_size();
}

OverlayBlockDevice::~OverlayBlockDevice() {
    close();
}

Geometry OverlayBlockDevice::geometry() const {
    return base_->geometry();
}

uint32_t OverlayBlockDevice::sector_size() const {
    return base_->sector_size();
}

uint64_t OverlayBlockDevice::size_bytes() const {
    return base_->size_bytes();
}

bool OverlayBlockDevice::get_dirty(uint64_t lba, std::span<uint8_t> out) const {
    auto it = ram_.find(lba);
    if (it != ram_.end()) {
        std::memcpy(out.data(), it->second.data(), out.size());
        return true;
    }
    
    auto spill_it = spill_idx_.find(lba);
    if (spill_it != spill_idx_.end() && spill_file_) {
        std::fseek(spill_file_, spill_it->second, SEEK_SET);
        if (std::fread(out.data(), 1, out.size(), spill_file_) == out.size()) {
            return true;
        }
    }
    
    return false;
}

void OverlayBlockDevice::put_dirty(uint64_t lba, std::span<const uint8_t> block) {
    if (ram_.find(lba) != ram_.end() || ram_.size() < max_ram_blocks_) {
        ram_[lba] = std::vector<uint8_t>(block.begin(), block.end());
        return;
    }
    
    if (!spill_file_) {
        spill_file_ = std::tmpfile();
        if (!spill_file_) {
            throw BlockDeviceError("Failed to create temporary spill file for overlay");
        }
    }
    
    uint64_t off;
    auto it = spill_idx_.find(lba);
    if (it == spill_idx_.end()) {
        std::fseek(spill_file_, 0, SEEK_END);
        off = std::ftell(spill_file_);
        spill_idx_[lba] = off;
    } else {
        off = it->second;
    }
    
    std::fseek(spill_file_, off, SEEK_SET);
    if (std::fwrite(block.data(), 1, block.size(), spill_file_) != block.size()) {
        throw BlockDeviceError("Failed to write to overlay spill file");
    }
}

void OverlayBlockDevice::read(uint64_t offset, std::span<uint8_t> buffer) const {
    uint32_t bb = sector_size();
    if (offset % bb != 0 || buffer.size() % bb != 0) {
        throw BlockDeviceError("Overlay reads must be sector aligned");
    }
    
    if (ram_.empty() && spill_idx_.empty()) {
        return base_->read(offset, buffer);
    }
    
    uint64_t start_lba = offset / bb;
    uint64_t count = buffer.size() / bb;
    
    bool any_dirty = false;
    for (uint64_t i = 0; i < count; ++i) {
        if (ram_.count(start_lba + i) > 0 || spill_idx_.count(start_lba + i) > 0) {
            any_dirty = true;
            break;
        }
    }
    
    if (!any_dirty) {
        return base_->read(offset, buffer);
    }
    
    // Read from base first, then overlay dirty blocks
    base_->read(offset, buffer);
    
    for (uint64_t i = 0; i < count; ++i) {
        std::span<uint8_t> block_span = buffer.subspan(i * bb, bb);
        get_dirty(start_lba + i, block_span);
    }
}

void OverlayBlockDevice::write(uint64_t offset, std::span<const uint8_t> buffer) {
    uint32_t bb = sector_size();
    if (offset % bb != 0 || buffer.size() % bb != 0) {
        throw BlockDeviceError("Overlay writes must be sector aligned");
    }
    
    uint64_t start_lba = offset / bb;
    uint64_t count = buffer.size() / bb;
    
    for (uint64_t i = 0; i < count; ++i) {
        put_dirty(start_lba + i, buffer.subspan(i * bb, bb));
    }
}

std::vector<uint64_t> OverlayBlockDevice::dirty_blocks() const {
    std::set<uint64_t> blocks;
    for (const auto& [lba, _] : ram_) blocks.insert(lba);
    for (const auto& [lba, _] : spill_idx_) blocks.insert(lba);
    return std::vector<uint64_t>(blocks.begin(), blocks.end());
}

uint64_t OverlayBlockDevice::dirty_bytes() const {
    return dirty_blocks().size() * sector_size();
}

void OverlayBlockDevice::commit(BlockDevice& target) {
    uint32_t bb = sector_size();
    std::vector<uint8_t> block(bb);
    
    for (uint64_t lba : dirty_blocks()) {
        if (get_dirty(lba, block)) {
            target.write(lba * bb, block);
        }
    }
    target.flush();
}

std::vector<uint64_t> OverlayBlockDevice::verify_committed(const BlockDevice& target) const {
    std::vector<uint64_t> bad;
    uint32_t bb = sector_size();
    std::vector<uint8_t> expected(bb);
    std::vector<uint8_t> actual(bb);
    
    for (uint64_t lba : dirty_blocks()) {
        if (get_dirty(lba, expected)) {
            target.read(lba * bb, actual);
            if (std::memcmp(expected.data(), actual.data(), bb) != 0) {
                bad.push_back(lba);
            }
        }
    }
    return bad;
}

void OverlayBlockDevice::flush() {
    if (spill_file_) {
        std::fflush(spill_file_);
    }
}

void OverlayBlockDevice::close() {
    if (spill_file_) {
        std::fclose(spill_file_);
        spill_file_ = nullptr;
    }
    ram_.clear();
    spill_idx_.clear();
}

} // namespace amidisk
