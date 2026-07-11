#include <gtest/gtest.h>
#include "../src/fs/ffs.h"
#include "../src/image.h"
#include <fstream>
#include <filesystem>

using namespace amidisk;
namespace fs = std::filesystem;

class FFSModifyTest : public ::testing::Test {
protected:
    std::string tmp_img = "test_ffs_modify.img";

    void SetUp() override {
        // Create 2MB raw image
        std::ofstream f(tmp_img, std::ios::binary);
        f.seekp(2 * 1024 * 1024 - 1);
        f.write("", 1);
        f.close();
        
        // Format it
        auto bdev = DiskImage::open(tmp_img, false)->blkdev();
        FFSVolume vol(bdev, 1, 2, 0x444F5303);
        vol.format("TestVol", 0x444F5303);
    }

    void TearDown() override {
        fs::remove(tmp_img);
    }
};

TEST_F(FFSModifyTest, MkdirAndRm) {
    auto img = DiskImage::open(tmp_img, false);
    auto vol = dynamic_cast<FFSVolume*>(img->volumes().front()->mount());
    
    // Test mkdir
    vol->mkdir("dir1");
    auto entries = vol->list_dir("");
    EXPECT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), "dir1");
    
    // Test makedirs
    vol->makedirs("dir1/dir2/dir3");
    entries = vol->list_dir("dir1/dir2");
    EXPECT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), "dir3");
    
    // Test rm (non-recursive should fail)
    EXPECT_THROW(vol->delete_path("dir1"), FSError);
    
    // Test rm (recursive)
    vol->delete_path("dir1", true);
    entries = vol->list_dir("");
    EXPECT_EQ(entries.size(), 0);
}

TEST_F(FFSModifyTest, Rename) {
    auto img = DiskImage::open(tmp_img, false);
    auto vol = dynamic_cast<FFSVolume*>(img->volumes().front()->mount());
    
    vol->mkdir("src_dir");
    vol->rename("src_dir", "dst_dir");
    
    auto entries = vol->list_dir("");
    EXPECT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), "dst_dir");
}

TEST_F(FFSModifyTest, Repair) {
    auto img = DiskImage::open(tmp_img, false);
    auto vol = dynamic_cast<FFSVolume*>(img->volumes().front()->mount());
    
    vol->mkdir("test");
    
    // Corrupt the bitmap (mark block 2 as free even though it's used by "test")
    vol->bitmap->free(2); // block 2 is the dir block
    
    auto rep = vol->check();
    EXPECT_FALSE(rep.ok);
    EXPECT_EQ(rep.errors.size(), 1);
    
    // Repair without apply
    uint32_t fixes = vol->repair(false);
    EXPECT_EQ(fixes, 1);
    
    // Repair with apply
    fixes = vol->repair(true);
    EXPECT_EQ(fixes, 1);
    
    // Now it should be OK
    rep = vol->check();
    EXPECT_TRUE(rep.ok);
}
