#include "part_blkdev.h"

namespace amidisk {

PartBlockDevice::PartBlockDevice(std::shared_ptr<BlockDevice> base, uint64_t start_block, uint64_t num_blocks, uint32_t block_bytes)
    : base_(base), block_bytes_(block_bytes) {
    
    // Convert base block units to bytes
    start_byte_ = start_block * base_->sector_size();
    
    // The partition logical block size might differ from the base device physical block size
    // For now, we assume num_blocks is in terms of the new block_bytes
    size_bytes_ = num_blocks * block_bytes_;
}

Geometry PartBlockDevice::geometry() const {
    Geometry geom{0, 16, 63};
    geom.cylinders = (size_bytes_ / block_bytes_) / (geom.heads * geom.sectors);
    if (geom.cylinders == 0 && size_bytes_ > 0) {
        geom.cylinders = 1;
    }
    return geom;
}

uint32_t PartBlockDevice::sector_size() const {
    return block_bytes_;
}

uint64_t PartBlockDevice::size_bytes() const {
    return size_bytes_;
}

void PartBlockDevice::read(uint64_t offset, std::span<uint8_t> buffer) const {
    if (offset + buffer.size() > size_bytes_) {
        throw BlockDeviceError("Read past end of partition");
    }
    base_->read(start_byte_ + offset, buffer);
}

void PartBlockDevice::write(uint64_t offset, std::span<const uint8_t> buffer) {
    if (offset + buffer.size() > size_bytes_) {
        throw BlockDeviceError("Write past end of partition");
    }
    base_->write(start_byte_ + offset, buffer);
}

void PartBlockDevice::flush() {
    base_->flush();
}

void PartBlockDevice::close() {
    // We don't necessarily close the base device since it might be shared
}

} // namespace amidisk
