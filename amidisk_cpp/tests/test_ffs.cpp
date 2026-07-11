#include <gtest/gtest.h>
#include "fs/ffs.h"
#include "blkdev/blkdev.h"
#include "blkdev/bulk_cache.h"
#include "blkdev/image_file.h"
#include "image.h"

#include <filesystem>
#include <random>
#include <cstring>
#include <fstream>

using namespace amidisk;

// Simple in-memory block device for testing
class MemBlockDevice : public BlockDevice {
public:
    MemBlockDevice(size_t size, uint32_t sector_sz = 512)
        : buffer_(size, 0), sector_size_(sector_sz) {}

    Geometry geometry() const override {
        uint32_t total_sectors = buffer_.size() / sector_size_;
        return {total_sectors / 63 / 16, 16, 63};
    }
    uint32_t sector_size() const override { return sector_size_; }
    uint64_t size_bytes() const override { return buffer_.size(); }
    bool is_read_only() const override { return false; }
    DeviceKind kind() const override { return DeviceKind::Image; }

    void read(uint64_t offset, std::span<uint8_t> buf) const override {
        if (offset + buf.size() > buffer_.size())
            throw BlockDeviceError("read past end");
        std::memcpy(buf.data(), buffer_.data() + offset, buf.size());
    }

    void write(uint64_t offset, std::span<const uint8_t> buf) override {
        if (offset + buf.size() > buffer_.size())
            throw BlockDeviceError("write past end");
        std::memcpy(buffer_.data() + offset, buf.data(), buf.size());
    }

    void flush() override {}
    void close() override {}

private:
    std::vector<uint8_t> buffer_;
    uint32_t sector_size_;
};

// ============================================================================
// Read parity tests (existing)
// ============================================================================

TEST(FFSVolumeTest, ReadParityFfsDc) {
    if (!std::filesystem::exists("../../tests/data/ffs-dc-amidisk.hdf")) GTEST_SKIP() << "ffs-dc-amidisk.hdf missing";
    auto img = DiskImage::open("../../tests/data/ffs-dc-amidisk.hdf");
    ASSERT_NE(img, nullptr);

    auto vol_ref = img->get_volume("DF0");
    ASSERT_NE(vol_ref, nullptr);

    auto vol = vol_ref->mount();
    ASSERT_NE(vol, nullptr);

    EXPECT_EQ(vol->dos_type_str(), "DOS\\5");

    auto entries = vol->list_dir("");
    ASSERT_EQ(entries.size(), 2);

    EXPECT_EQ(entries[0].name_str(), "Docs");
    EXPECT_TRUE(entries[0].is_dir());

    EXPECT_EQ(entries[1].name_str(), "top.txt");
    EXPECT_TRUE(entries[1].is_file());
    EXPECT_EQ(entries[1].comment_str(), "with comment");
}

TEST(FFSVolumeTest, ReadParityRdb) {
    if (!std::filesystem::exists("../../tests/data/rdb-amidisk.hdf")) GTEST_SKIP() << "rdb-amidisk.hdf missing";
    auto img = DiskImage::open("../../tests/data/rdb-amidisk.hdf");
    ASSERT_NE(img, nullptr);

    auto vol_ref = img->get_volume("DH0");
    ASSERT_NE(vol_ref, nullptr);

    auto vol = vol_ref->mount();
    ASSERT_NE(vol, nullptr);

    EXPECT_EQ(vol->dos_type_str(), "DOS\\3");

    auto entries = vol->list_dir("");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), "T");
    EXPECT_TRUE(entries[0].is_dir());

    auto deep_entries = vol->list_dir("T/Deep");
    ASSERT_EQ(deep_entries.size(), 1);
    EXPECT_EQ(deep_entries[0].name_str(), "blob.bin");
    EXPECT_EQ(deep_entries[0].size, 1000000);
}

// ============================================================================
// Write tests - format, write, verify integrity
// ============================================================================

class FFSWriteTest : public ::testing::TestWithParam<uint32_t> {
protected:
    void SetUp() override {
        mem_dev_ = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
        dev_ = mem_dev_;
    }

    std::shared_ptr<MemBlockDevice> mem_dev_;
    std::shared_ptr<BlockDevice> dev_;
};

TEST_P(FFSWriteTest, FormatAndCheck) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    FFSVolume vol2(dev_);
    vol2.open();

    EXPECT_EQ(vol2.get_label(), "TestVol");

    auto report = vol2.check(true);
    EXPECT_TRUE(report.ok) << "Errors: " << (report.errors.empty() ? "none" : report.errors[0]);
}

TEST_P(FFSWriteTest, WriteSmallFile) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    vol.write_file("test.txt", data, {});

    auto read_data = vol.read_file_bytes("test.txt");
    EXPECT_EQ(read_data, data);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok) << "Errors: " << (report.errors.empty() ? "none" : report.errors[0]);
}

TEST_P(FFSWriteTest, WriteLargeFile) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    std::vector<uint8_t> data(100 * 1024);
    std::mt19937 rng(42);
    for (auto& b : data) b = rng() & 0xFF;

    vol.write_file("large.bin", data, {});

    auto read_data = vol.read_file_bytes("large.bin");
    EXPECT_EQ(read_data.size(), data.size());
    EXPECT_EQ(read_data, data);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok) << "Errors: " << (report.errors.empty() ? "none" : report.errors[0]);
}

TEST_P(FFSWriteTest, MakeDirs) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    vol.makedirs("a/b/c/d");

    auto entries = vol.list_dir("a/b/c");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), "d");
    EXPECT_TRUE(entries[0].is_dir());

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST_P(FFSWriteTest, WriteToNestedPath) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    vol.makedirs("deep/nested/path");
    std::vector<uint8_t> data = {'D', 'e', 'e', 'p'};
    vol.write_file("deep/nested/path/file.txt", data, {});

    auto read_data = vol.read_file_bytes("deep/nested/path/file.txt");
    EXPECT_EQ(read_data, data);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST_P(FFSWriteTest, OverwriteFile) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    std::vector<uint8_t> data1 = {'O', 'r', 'i', 'g'};
    vol.write_file("test.txt", data1, {});

    std::vector<uint8_t> data2 = {'N', 'e', 'w', ' ', 'c', 'o', 'n', 't', 'e', 'n', 't'};
    vol.write_file("test.txt", data2, {});

    auto read_data = vol.read_file_bytes("test.txt");
    EXPECT_EQ(read_data, data2);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST_P(FFSWriteTest, DeleteFile) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    std::vector<uint8_t> data = {'T', 'e', 's', 't'};
    vol.write_file("delete_me.txt", data, {});

    auto entries_before = vol.list_dir("");
    EXPECT_EQ(entries_before.size(), 1);

    vol.delete_path("delete_me.txt");

    auto entries_after = vol.list_dir("");
    EXPECT_EQ(entries_after.size(), 0);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST_P(FFSWriteTest, DeleteDirectory) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    vol.makedirs("dir_to_delete/subdir");
    std::vector<uint8_t> data = {'X'};
    vol.write_file("dir_to_delete/subdir/file.txt", data, {});

    vol.delete_path("dir_to_delete", true);

    auto entries = vol.list_dir("");
    EXPECT_EQ(entries.size(), 0);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST_P(FFSWriteTest, ManySmallFiles) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    for (int i = 0; i < 100; ++i) {
        std::string name = "file" + std::to_string(i) + ".txt";
        std::vector<uint8_t> data(10, static_cast<uint8_t>(i));
        vol.write_file(name, data, {});
    }

    auto entries = vol.list_dir("");
    EXPECT_EQ(entries.size(), 100);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok) << "Errors: " << (report.errors.empty() ? "none" : report.errors[0]);
}

TEST_P(FFSWriteTest, BitmapConsistency) {
    uint32_t dos_type = GetParam();

    FFSVolume vol(dev_);
    vol.format("TestVol", dos_type);

    auto info1 = vol.get_info();
    uint64_t initial_free = info1.free_bytes;

    std::vector<uint8_t> data(50000, 'X');
    vol.write_file("space_test.bin", data, {});

    auto info2 = vol.get_info();
    EXPECT_LT(info2.free_bytes, initial_free);

    vol.delete_path("space_test.bin");

    auto info3 = vol.get_info();
    EXPECT_EQ(info3.free_bytes, initial_free);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

INSTANTIATE_TEST_SUITE_P(
    AllDosTypes,
    FFSWriteTest,
    ::testing::Values(
        0x444F5300,  // DOS\0 - OFS
        0x444F5301,  // DOS\1 - FFS
        0x444F5302,  // DOS\2 - OFS-Intl
        0x444F5303,  // DOS\3 - FFS-Intl
        0x444F5306,  // DOS\6 - OFS-LNFS
        0x444F5307   // DOS\7 - FFS-LNFS
    ),
    [](const ::testing::TestParamInfo<uint32_t>& info) {
        switch (info.param & 0xFF) {
            case 0: return "OFS";
            case 1: return "FFS";
            case 2: return "OFS_Intl";
            case 3: return "FFS_Intl";
            case 6: return "OFS_LNFS";
            case 7: return "FFS_LNFS";
            default: return "Unknown";
        }
    }
);

// ============================================================================
// LNFS-specific tests (long filenames)
// ============================================================================

TEST(FFSLongNameTest, LongFilenames) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("LNFSTest", 0x444F5307);

    std::string long_name(107, 'A');
    std::vector<uint8_t> data = {'L', 'o', 'n', 'g'};
    vol.write_file(long_name, data, {});

    auto entries = vol.list_dir("");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), long_name);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST(FFSLongNameTest, TruncatesNonLNFS) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("FFSTest", 0x444F5303);

    std::string long_name(50, 'B');
    std::vector<uint8_t> data = {'T', 'r', 'u', 'n', 'c'};
    vol.write_file(long_name, data, {});

    auto entries = vol.list_dir("");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str().length(), 30);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

// ============================================================================
// Bulk mode tests
// ============================================================================

TEST(FFSBulkTest, BulkWriteVerifyAfter) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    {
        FFSVolume vol(dev_ptr);
        vol.format("BulkTest", 0x444F5303);

        BulkGuard bulk(vol.blkdev_ref());

        for (int i = 0; i < 50; ++i) {
            std::string name = "file" + std::to_string(i) + ".txt";
            std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
            vol.write_file(name, data, {});
        }
    }

    FFSVolume vol2(dev_ptr);
    vol2.open();

    auto entries = vol2.list_dir("");
    EXPECT_EQ(entries.size(), 50);

    auto report = vol2.check(true);
    EXPECT_TRUE(report.ok) << "Errors: " << (report.errors.empty() ? "none" : report.errors[0]);
}

// ============================================================================
// Edge cases and error handling
// ============================================================================

TEST(FFSEdgeCaseTest, EmptyFile) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("Test", 0x444F5303);

    std::vector<uint8_t> empty;
    vol.write_file("empty.txt", empty, {});

    auto data = vol.read_file_bytes("empty.txt");
    EXPECT_EQ(data.size(), 0);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST(FFSEdgeCaseTest, ExactBlockSize) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("Test", 0x444F5301);

    std::vector<uint8_t> data(512, 'X');
    vol.write_file("exact.bin", data, {});

    auto read_data = vol.read_file_bytes("exact.bin");
    EXPECT_EQ(read_data, data);

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
}

TEST(FFSEdgeCaseTest, FileNotFound) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("Test", 0x444F5303);

    EXPECT_THROW(vol.read_file_bytes("nonexistent.txt"), FSError);
}

TEST(FFSEdgeCaseTest, DeleteNonexistent) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("Test", 0x444F5303);

    EXPECT_THROW(vol.delete_path("nonexistent.txt"), FSError);
}

TEST(FFSEdgeCaseTest, MkdirExisting) {
    auto dev = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("Test", 0x444F5303);

    vol.mkdir("mydir");
    vol.makedirs("mydir");

    auto entries = vol.list_dir("");
    EXPECT_EQ(entries.size(), 1);
}
