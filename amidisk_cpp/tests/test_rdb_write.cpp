#include <gtest/gtest.h>
#include "../src/rdb/rdisk.h"
#include "../src/image.h"
#include <fstream>
#include <filesystem>

using namespace amidisk;
namespace fs = std::filesystem;

class RDBWriteTest : public ::testing::Test {
protected:
    std::string tmp_img = "test_rdb_write.img";
    std::string tmp_fs = "test_fs.bin";

    void SetUp() override {
        // Create 10MB empty image
        std::ofstream f(tmp_img, std::ios::binary);
        f.seekp(10 * 1024 * 1024 - 1);
        f.write("", 1);
        f.close();
        
        // Create dummy FS file
        std::ofstream fs_f(tmp_fs, std::ios::binary);
        std::string dummy(2048, 'A');
        fs_f.write(dummy.data(), dummy.size());
        fs_f.close();
    }

    void TearDown() override {
        fs::remove(tmp_img);
        fs::remove(tmp_fs);
    }
};

TEST_F(RDBWriteTest, InitAndAddPartition) {
    auto bdev = DiskImage::open(tmp_img, false)->blkdev();
    
    // Init RDB
    auto rdisk = RDisk::create(bdev, 63, 0, 0);
    EXPECT_EQ(rdisk->partitions().size(), 0);
    
    // Add partition
    auto part = rdisk->add_partition("DH0", 2 * 1024 * 1024, 0x444f5303, 1, true, 0);
    EXPECT_EQ(part->drv_name(), "DH0");
    EXPECT_EQ(rdisk->partitions().size(), 1);
    
    // Add another partition
    auto part2 = rdisk->add_partition("DH1", 0, 0x444f5303, 1, false, 0);
    EXPECT_EQ(part2->drv_name(), "DH1");
    EXPECT_EQ(rdisk->partitions().size(), 2);
    
    // Test delete
    rdisk->delete_partition("DH0");
    EXPECT_EQ(rdisk->partitions().size(), 1);
    EXPECT_EQ(rdisk->partitions()[0]->drv_name(), "DH1");
}

TEST_F(RDBWriteTest, AddFilesystem) {
    auto bdev = DiskImage::open(tmp_img, false)->blkdev();
    auto rdisk = RDisk::create(bdev, 63, 0, 0);
    
    rdisk->add_filesystem(tmp_fs, 0x50465303, 19, 2);
    EXPECT_EQ(rdisk->filesystems().size(), 1);
    EXPECT_EQ(rdisk->filesystems()[0]->fshd_blk().type, 0x50465303);
    EXPECT_EQ(rdisk->filesystems()[0]->fshd_blk().version, 19);
    
    std::string out_fs = "extracted_fs.bin";
    rdisk->extract_filesystem(0x50465303, out_fs);
    
    std::ifstream ext(out_fs, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ext)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content.size(), 2048);
    EXPECT_EQ(content[0], 'A');
    
    fs::remove(out_fs);
}
