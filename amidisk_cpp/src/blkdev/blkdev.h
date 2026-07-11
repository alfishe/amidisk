#pragma once

#include <stdexcept>
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <cstdint>

namespace amidisk {

class BlockDeviceError : public std::runtime_error {
public:
    explicit BlockDeviceError(const std::string& message) : std::runtime_error(message) {}
};

enum class DeviceKind {
    Image,
    Vhd,
    Physical,
    Overlay
};

struct Geometry {
    uint32_t cylinders;
    uint32_t heads;
    uint32_t sectors; // sectors per track
};

class LockGuard {
public:
    virtual ~LockGuard() = default;
};

class BlockDevice {
public:
    virtual ~BlockDevice() = default;

    virtual Geometry geometry() const = 0;
    virtual uint32_t sector_size() const = 0;
    virtual uint64_t size_bytes() const = 0;
    
    virtual uint64_t num_blocks() const {
        return size_bytes() / sector_size();
    }

    // Read count bytes starting from lba * sector_size()
    virtual void read(uint64_t offset, std::span<uint8_t> buffer) const = 0;
    
    // Write count bytes starting from lba * sector_size()
    virtual void write(uint64_t offset, std::span<const uint8_t> buffer) = 0;

    virtual void flush() = 0;
    virtual void close() = 0;
    
    virtual std::unique_ptr<LockGuard> lock_exclusive() {
        return nullptr; // Default no-op
    }
    
    virtual DeviceKind kind() const = 0;
    virtual bool is_read_only() const = 0;
};

// Endianness helpers (Big Endian)
inline uint32_t read_be32(std::span<const uint8_t> data, size_t offset) {
    if (offset + 4 > data.size()) throw std::out_of_range("read_be32 out of bounds");
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           (static_cast<uint32_t>(data[offset + 3]));
}

inline void write_be32(std::span<uint8_t> data, size_t offset, uint32_t val) {
    if (offset + 4 > data.size()) throw std::out_of_range("write_be32 out of bounds");
    data[offset] = static_cast<uint8_t>((val >> 24) & 0xFF);
    data[offset + 1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    data[offset + 2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[offset + 3] = static_cast<uint8_t>(val & 0xFF);
}

inline uint16_t read_be16(std::span<const uint8_t> data, size_t offset) {
    if (offset + 2 > data.size()) throw std::out_of_range("read_be16 out of bounds");
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
           static_cast<uint16_t>(data[offset + 1]));
}

inline void write_be16(std::span<uint8_t> data, size_t offset, uint16_t val) {
    if (offset + 2 > data.size()) throw std::out_of_range("write_be16 out of bounds");
    data[offset] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[offset + 1] = static_cast<uint8_t>(val & 0xFF);
}

// Free functions
std::shared_ptr<BlockDevice> open_blkdev(const std::string& path, bool read_only = true);

} // namespace amidisk
