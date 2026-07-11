#include <gtest/gtest.h>
#include "../src/blkdev/blkdev.h"
#include "../src/blkdev/image_file.h"
#include "../src/blkdev/part_blkdev.h"
#include "../src/blkdev/overlay.h"
#include <cstdio>
#include <fstream>
#include <filesystem>

using namespace amidisk;

class BlkDevTest : public ::testing::Test {
protected:
    std::string test_file = "test_blkdev.img";

    void SetUp() override {
        std::ofstream out(test_file, std::ios::binary);
        std::vector<uint8_t> data(1024, 0);
        data[0] = 0xAA;
        data[1] = 0xBB;
        data[512] = 0xCC;
        data[513] = 0xDD;
        out.write(reinterpret_cast<char*>(data.data()), data.size());
    }

    void TearDown() override {
        std::filesystem::remove(test_file);
    }
};

TEST_F(BlkDevTest, ReadWrite) {
    auto dev = std::make_shared<ImageFileBlkDev>(test_file, false);
    EXPECT_EQ(dev->size_bytes(), 1024);
    EXPECT_EQ(dev->sector_size(), 512);

    std::vector<uint8_t> buf(512);
    dev->read(0, buf);
    EXPECT_EQ(buf[0], 0xAA);
    EXPECT_EQ(buf[1], 0xBB);

    buf[0] = 0xEE;
    dev->write(512, buf);
    
    std::vector<uint8_t> buf2(512);
    dev->read(512, buf2);
    EXPECT_EQ(buf2[0], 0xEE);
    EXPECT_EQ(buf2[1], 0xBB);
}

TEST_F(BlkDevTest, Overlay) {
    auto base = std::make_shared<ImageFileBlkDev>(test_file, true);
    OverlayBlockDevice overlay(base, 1024 * 1024);

    std::vector<uint8_t> buf(512);
    overlay.read(0, buf);
    EXPECT_EQ(buf[0], 0xAA);

    buf[0] = 0xFF;
    overlay.write(0, buf);

    // Read back from overlay
    std::vector<uint8_t> buf2(512);
    overlay.read(0, buf2);
    EXPECT_EQ(buf2[0], 0xFF);

    // Base should remain unchanged
    std::vector<uint8_t> buf3(512);
    base->read(0, buf3);
    EXPECT_EQ(buf3[0], 0xAA);
}

TEST_F(BlkDevTest, PartBlockDevice) {
    auto base = std::make_shared<ImageFileBlkDev>(test_file, false);
    // Partition of 1 sector starting at sector 1
    PartBlockDevice part(base, 1, 1);

    EXPECT_EQ(part.size_bytes(), 512);

    std::vector<uint8_t> buf(512);
    part.read(0, buf);
    EXPECT_EQ(buf[0], 0xCC);
    EXPECT_EQ(buf[1], 0xDD);
}
