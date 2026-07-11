#include <gtest/gtest.h>
#include "fs/ffs.h"
#include "image.h"
#include <filesystem>
#include <iostream>

using namespace amidisk;
namespace fs = std::filesystem;

TEST(FFSVolumeTest, SmokeCheckAllFixtures) {
    std::vector<std::string> paths = {"../../../tests/data", "../../../data"};
    for (const auto& base_path : paths) {
        if (!fs::exists(base_path)) continue;
        for (const auto& entry : fs::directory_iterator(base_path)) {
            std::string ext = entry.path().extension().string();
            if (ext != ".hdf" && ext != ".adf" && ext != ".vhd") continue;
            
            std::string path = entry.path().string();
            std::cout << "Testing fixture: " << path << std::endl;
            
            auto img = DiskImage::open(path);
            ASSERT_NE(img, nullptr) << "Failed to open image: " << path;
            
            for (const auto& vol_ref : img->volumes()) {
                Volume* vol = nullptr;
                try {
                    vol = vol_ref->mount();
                } catch (const std::exception& e) {
                    continue; // Skip volumes that are not AmigaDOS/FFS
                }
                
                if (vol) {
                    auto rep = vol->check();
                    EXPECT_TRUE(rep.ok) << "Check failed on " << path << ":" << vol_ref->name() << "\nFirst error: " << (rep.errors.empty() ? "None" : rep.errors[0]);
                }
            }
        }
    }
}

TEST(FFSVolumeTest, SmokeReadStartupSequence) {
    if (!fs::exists("../../data/OS-3.2.3.vhd")) {
        GTEST_SKIP() << "Fixture OS-3.2.3.vhd not found";
    }
    auto img = DiskImage::open("../../data/OS-3.2.3.vhd");
    
    auto vol_ref = img->get_volume("GDH0");
    ASSERT_NE(vol_ref, nullptr) << "Volume GDH0 not found";
    
    Volume* vol = vol_ref->mount();
    ASSERT_NE(vol, nullptr) << "Failed to mount GDH0";
    
    bool read_success = false;
    std::string first_char;
    
    vol->read_file("S/startup-sequence", [&](std::span<const uint8_t> chunk) {
        if (!read_success && !chunk.empty()) {
            first_char = std::string(1, static_cast<char>(chunk[0]));
            read_success = true;
        }
    });
    
    EXPECT_TRUE(read_success);
    EXPECT_EQ(first_char, ";") << "startup-sequence should start with a semicolon comment";
}
