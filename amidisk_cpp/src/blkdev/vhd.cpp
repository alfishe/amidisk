#include "vhd.h"
#include <cstring>
#include <filesystem>

namespace amidisk {

constexpr uint32_t VHD_TYPE_FIXED = 2;
constexpr uint32_t VHD_TYPE_DYNAMIC = 3;

VHDBlkDev::VHDBlkDev(const std::string& path, bool read_only)
    : path_(path), read_only_(read_only), disk_type_(0), current_size_(0),
      num_blocks_(0), dyn_block_size_(0), bitmap_secs_(0) {
    
    std::ios_base::openmode mode = std::ios::binary | std::ios::in;
    if (!read_only_) {
        mode |= std::ios::out;
    }

    file_.open(path_, mode);
    if (!file_.is_open()) {
        throw BlockDeviceError("Failed to open VHD file: " + path_);
    }

    file_.seekg(0, std::ios::end);
    uint64_t file_size = file_.tellg();
    if (file_size < 512) {
        throw BlockDeviceError("File too small for VHD");
    }

    file_.seekg(file_size - 512, std::ios::beg);
    std::vector<uint8_t> footer(512);
    if (!file_.read(reinterpret_cast<char*>(footer.data()), 512)) {
        throw BlockDeviceError("Failed to read VHD footer");
    }

    if (std::memcmp(footer.data(), "conectix", 8) != 0) {
        throw BlockDeviceError("No VHD footer cookie");
    }

    disk_type_ = read_be32(footer, 0x3C);
    
    // Size is at 0x30 in the footer (8 bytes big-endian)
    current_size_ = (static_cast<uint64_t>(read_be32(footer, 0x30)) << 32) |
                    read_be32(footer, 0x34);

    if (disk_type_ == VHD_TYPE_FIXED) {
        read_only_ = read_only;
        num_blocks_ = current_size_ / 512;
        if (num_blocks_ * 512 > file_size - 512) {
            num_blocks_ = (file_size - 512) / 512;
        }
    } else if (disk_type_ == VHD_TYPE_DYNAMIC) {
        read_only_ = true;
        if (!read_only) {
            throw BlockDeviceError("Dynamic VHD write not supported; convert to fixed VHD or HDF first");
        }
        parse_dynamic(footer);
    } else {
        throw BlockDeviceError("Unsupported VHD type");
    }
}

VHDBlkDev::~VHDBlkDev() {
    close();
}

void VHDBlkDev::parse_dynamic(std::span<const uint8_t> footer) {
    uint64_t data_offset = (static_cast<uint64_t>(read_be32(footer, 0x10)) << 32) |
                           read_be32(footer, 0x14);

    file_.seekg(data_offset, std::ios::beg);
    std::vector<uint8_t> dyn(1024);
    if (!file_.read(reinterpret_cast<char*>(dyn.data()), 1024)) {
        throw BlockDeviceError("Failed to read VHD dynamic header");
    }

    if (std::memcmp(dyn.data(), "cxsparse", 8) != 0) {
        throw BlockDeviceError("No VHD dynamic header cookie");
    }

    uint64_t table_offset = (static_cast<uint64_t>(read_be32(dyn, 16)) << 32) |
                            read_be32(dyn, 20);
    uint32_t max_entries = read_be32(dyn, 28);
    dyn_block_size_ = read_be32(dyn, 32);

    file_.seekg(table_offset, std::ios::beg);
    std::vector<uint8_t> raw_bat(max_entries * 4);
    if (!file_.read(reinterpret_cast<char*>(raw_bat.data()), max_entries * 4)) {
        throw BlockDeviceError("Failed to read VHD BAT");
    }

    bat_.resize(max_entries);
    for (uint32_t i = 0; i < max_entries; ++i) {
        bat_[i] = read_be32(raw_bat, i * 4);
    }

    uint32_t bitmap_bytes = (dyn_block_size_ / 512 + 7) / 8;
    bitmap_secs_ = (bitmap_bytes + 511) / 512;
    num_blocks_ = current_size_ / 512;
}

Geometry VHDBlkDev::geometry() const {
    Geometry geom{0, 16, 63};
    geom.cylinders = num_blocks_ / (geom.heads * geom.sectors);
    if (geom.cylinders == 0 && num_blocks_ > 0) {
        geom.cylinders = 1;
    }
    return geom;
}

uint32_t VHDBlkDev::sector_size() const {
    return 512;
}

uint64_t VHDBlkDev::size_bytes() const {
    return current_size_;
}

void VHDBlkDev::read(uint64_t offset, std::span<uint8_t> buffer) const {
    if (offset + buffer.size() > current_size_) {
        throw BlockDeviceError("Read past end of VHD");
    }
    
    if (offset % 512 != 0 || buffer.size() % 512 != 0) {
        throw BlockDeviceError("VHD reads must be sector aligned. offset=" + std::to_string(offset) + " size=" + std::to_string(buffer.size()));
    }
    
    uint64_t lba = offset / 512;
    uint64_t count = buffer.size() / 512;

    if (disk_type_ == VHD_TYPE_FIXED) {
        file_.seekg(lba * 512, std::ios::beg);
        if (!file_.read(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
            throw BlockDeviceError("Short read on fixed VHD");
        }
        return;
    }

    // Dynamic
    uint32_t secs_per_block = dyn_block_size_ / 512;
    uint64_t remaining = count;
    uint64_t cur = lba;
    uint8_t* out_ptr = buffer.data();

    while (remaining > 0) {
        uint64_t bat_idx = cur / secs_per_block;
        uint64_t in_blk = cur % secs_per_block;
        uint64_t run = std::min(remaining, (uint64_t)secs_per_block - in_blk);
        
        // Optimization: Zero-Copy Block Loading
        // Instead of allocating a transient buffer (e.g. `std::vector<uint8_t>`) for each read loop
        // and manually copying via memcpy, we decode the VHD BAT to find the physical offset
        // and read directly into the caller's target `out_ptr` array. This saves millions
        // of heap allocations/deallocations during a partition scan.
        if (bat_idx >= bat_.size() || bat_[bat_idx] == 0xFFFFFFFF) {
            std::memset(out_ptr, 0, run * 512);
        } else {
            uint64_t disk_off = (static_cast<uint64_t>(bat_[bat_idx]) + bitmap_secs_ + in_blk) * 512;
            file_.seekg(disk_off, std::ios::beg);
            if (!file_.read(reinterpret_cast<char*>(out_ptr), run * 512)) {
                throw BlockDeviceError("Short read in dynamic VHD block");
            }
        }
        
        cur += run;
        remaining -= run;
        out_ptr += run * 512;
    }
}

void VHDBlkDev::write(uint64_t offset, std::span<const uint8_t> buffer) {
    if (read_only_) {
        throw BlockDeviceError("VHD device is read-only");
    }
    if (offset % 512 != 0 || buffer.size() % 512 != 0) {
        throw BlockDeviceError("VHD writes must be sector aligned");
    }
    if (offset + buffer.size() > current_size_) {
        throw BlockDeviceError("Write past end of VHD");
    }

    file_.seekp(offset, std::ios::beg);
    if (!file_.write(reinterpret_cast<const char*>(buffer.data()), buffer.size())) {
        throw BlockDeviceError("Write error on fixed VHD");
    }
    file_.flush();
}

void VHDBlkDev::flush() {
    if (file_.is_open()) {
        file_.flush();
    }
}

void VHDBlkDev::close() {
    if (file_.is_open()) {
        file_.close();
    }
}

} // namespace amidisk
