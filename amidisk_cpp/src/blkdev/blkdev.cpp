#include "blkdev.h"
#include "image_file.h"
#include "vhd.h"
#include <fstream>
#include <cstring>
#include <filesystem>

namespace amidisk {

std::shared_ptr<BlockDevice> open_blkdev(const std::string& path, bool read_only) {
    // Check if it's a VHD by reading the footer
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) >= 512) {
        std::ifstream file(path, std::ios::binary);
        if (file) {
            file.seekg(-512, std::ios::end);
            char footer[8];
            if (file.read(footer, 8)) {
                if (std::memcmp(footer, "conectix", 8) == 0) {
                    return std::make_shared<VHDBlkDev>(path, read_only);
                }
            }
        }
    }
    
    // Default to plain image file
    return std::make_shared<ImageFileBlkDev>(path, read_only);
}

} // namespace amidisk
