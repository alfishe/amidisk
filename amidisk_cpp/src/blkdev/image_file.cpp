#include "image_file.h"
#include <filesystem>

namespace amidisk {

ImageFileBlkDev::ImageFileBlkDev(const std::string& path, bool read_only, uint32_t block_bytes)
    : path_(path), read_only_(read_only), block_bytes_(block_bytes), size_bytes_(0) {
    
    std::ios_base::openmode mode = std::ios::binary;
    if (read_only_) {
        mode |= std::ios::in;
    } else {
        mode |= std::ios::in | std::ios::out;
        if (!std::filesystem::exists(path_)) {
            // Create if it doesn't exist and not read_only
            std::ofstream create(path_, std::ios::binary);
            create.close();
        }
    }

    file_.open(path_, mode);
    if (!file_.is_open()) {
        throw BlockDeviceError("Failed to open image file: " + path_);
    }

    // Get size
    file_.seekg(0, std::ios::end);
    size_bytes_ = file_.tellg();
    file_.seekg(0, std::ios::beg);

    infer_geometry();
}

ImageFileBlkDev::~ImageFileBlkDev() {
    close();
}

void ImageFileBlkDev::infer_geometry() {
    uint64_t total_blocks = size_bytes_ / block_bytes_;
    geom_.cylinders = 0;
    geom_.heads = 1;
    geom_.sectors = 1;

    if (total_blocks == 0) return;

    if (total_blocks == 1760) {
        geom_.cylinders = 80;
        geom_.heads = 2;
        geom_.sectors = 11;
        return;
    }

    // Default amitools/WinUAE heuristics for HDD
    geom_.heads = 16;
    geom_.sectors = 63;
    geom_.cylinders = total_blocks / (geom_.heads * geom_.sectors);
    if (geom_.cylinders == 0 && total_blocks > 0) {
        geom_.cylinders = 1;
    }
}

Geometry ImageFileBlkDev::geometry() const {
    return geom_;
}

uint32_t ImageFileBlkDev::sector_size() const {
    return block_bytes_;
}

uint64_t ImageFileBlkDev::size_bytes() const {
    return size_bytes_;
}

void ImageFileBlkDev::read(uint64_t offset, std::span<uint8_t> buffer) const {
    if (offset + buffer.size() > size_bytes_) {
        throw BlockDeviceError("Read past end of image file");
    }
    file_.seekg(offset, std::ios::beg);
    if (!file_.read(reinterpret_cast<char*>(buffer.data()), buffer.size())) {
        throw BlockDeviceError("Read error on image file");
    }
}

void ImageFileBlkDev::write(uint64_t offset, std::span<const uint8_t> buffer) {
    if (read_only_) {
        throw BlockDeviceError("Image file is read-only");
    }
    
    // Auto-extend file if needed
    if (offset + buffer.size() > size_bytes_) {
        size_bytes_ = offset + buffer.size();
        infer_geometry();
    }
    
    file_.seekp(offset, std::ios::beg);
    if (!file_.write(reinterpret_cast<const char*>(buffer.data()), buffer.size())) {
        throw BlockDeviceError("Write error on image file");
    }
}

void ImageFileBlkDev::flush() {
    if (file_.is_open()) {
        file_.flush();
    }
}

void ImageFileBlkDev::close() {
    if (file_.is_open()) {
        file_.close();
    }
}

} // namespace amidisk
