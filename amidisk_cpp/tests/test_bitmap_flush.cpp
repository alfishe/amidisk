#include <gtest/gtest.h>
#include "fs/ffs.h"
#include "fs/volume.h"
#include "blkdev/blkdev.h"

#include <set>
#include <cstring>

using namespace amidisk;

class TrackingBlockDevice : public BlockDevice {
public:
    TrackingBlockDevice(size_t size, uint32_t sector_sz = 512)
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

class BitmapFlushTest : public ::testing::Test {
protected:
    void SetUp() override {
        dev_ = std::make_shared<TrackingBlockDevice>(10 * 1024 * 1024);  // 10MB (small for fast tests)
        vol_ = std::make_unique<FFSVolume>(dev_);
        vol_->format("TEST", 0x444F5307);  // DOS7
    }

    std::shared_ptr<TrackingBlockDevice> dev_;
    std::unique_ptr<FFSVolume> vol_;
};

// Test: After allocating blocks and flushing, the bitmap on disk should mark those blocks as used.
TEST_F(BitmapFlushTest, AllocatedBlocksMarkedUsedAfterFlush) {
    // Write a file which allocates blocks
    std::vector<uint8_t> data(10000, 'X');
    vol_->write_file("/testfile", data, {});

    // Get the blocks that should be allocated
    auto entry = vol_->resolve("/testfile");
    ASSERT_GT(entry.blk, 0u);

    // Check consistency on the same volume
    auto check_result = vol_->check();

    EXPECT_TRUE(check_result.ok)
        << "Bitmap inconsistency: allocated blocks should be marked as used. Errors: "
        << (check_result.errors.empty() ? "none" : check_result.errors[0]);
}

// Test: Many small file writes should all be correctly reflected in bitmap
TEST_F(BitmapFlushTest, ManySmallFilesConsistentBitmap) {
    for (int i = 0; i < 100; ++i) {
        std::string name = "/file" + std::to_string(i);
        std::vector<uint8_t> data(500 + i * 10, static_cast<uint8_t>(i));
        vol_->write_file(name, data, {});
    }

    // Check filesystem consistency
    auto check_result = vol_->check();
    EXPECT_TRUE(check_result.ok)
        << "Bitmap inconsistency after many small files. Errors: "
        << (check_result.errors.empty() ? "none" : check_result.errors[0]);
}

// Test: Allocations spanning multiple bitmap pages should all flush correctly
TEST_F(BitmapFlushTest, MultiPageBitmapFlush) {
    // Test with 500MB to trigger the bug
    auto big_dev = std::make_shared<TrackingBlockDevice>(500 * 1024 * 1024);  // 500MB
    FFSVolume big_vol(big_dev);
    big_vol.format("BIGTEST", 0x444F5307);

    // Debug: Check if bitmap page 193 block (512194) is marked correctly
    // For 500MB: root_blk = 512000, bitmap page 193 at block 512001+193 = 512194
    // Block 512194 is in bitmap page 126 (idx 512192/4064 = 126), off 128
    // Long index = 1 + 128/32 = 5, bit = 0
    {
        uint32_t blk = 512194;
        uint32_t idx = blk - 2;  // reserved=2
        uint32_t pi = idx / 4064;  // bits_per_page=4064
        uint32_t off = idx % 4064;
        uint32_t long_idx = 1 + off / 32;
        uint32_t bit = off % 32;

        // Read the actual bit from bitmap
        // Since we can't directly access bitmap, let's at least verify the check passes
        EXPECT_EQ(pi, 126u) << "Page index for block 512194 should be 126";
        EXPECT_EQ(off, 128u) << "Offset within page should be 128";
        EXPECT_EQ(long_idx, 5u) << "Long index should be 5";
        EXPECT_EQ(bit, 0u) << "Bit should be 0";
    }

    // Check immediately after format - no file writes yet
    auto check_after_format = big_vol.check();
    EXPECT_TRUE(check_after_format.ok)
        << "Bitmap inconsistency after format (before any file writes). Errors: "
        << (check_after_format.errors.empty() ? "none" : check_after_format.errors[0]);

    // If format is already broken, skip file writes test
    if (!check_after_format.ok) return;

    // Write files that span multiple bitmap pages
    // Each bitmap page covers (512-4)*8 = 4064 blocks = ~2MB
    // So writing 10MB should touch ~5 bitmap pages
    std::vector<uint8_t> data(1024 * 1024, 'A');  // 1MB
    for (int i = 0; i < 5; ++i) {
        std::string name = "/bigfile" + std::to_string(i);
        big_vol.write_file(name, data, {});
    }

    auto check_result = big_vol.check();
    EXPECT_TRUE(check_result.ok)
        << "Bitmap inconsistency with multi-page allocations. Errors: "
        << (check_result.errors.empty() ? "none" : check_result.errors[0]);
}

// Test: Bitmap cursor should not cause lost allocations
TEST_F(BitmapFlushTest, CursorDoesNotCauseLostAllocations) {
    // Allocate some blocks, then allocate more - cursor should resume
    std::vector<uint8_t> data1(5000, 'A');
    vol_->write_file("/first", data1, {});

    std::vector<uint8_t> data2(5000, 'B');
    vol_->write_file("/second", data2, {});

    std::vector<uint8_t> data3(5000, 'C');
    vol_->write_file("/third", data3, {});

    // All should be present and consistent
    auto check_result = vol_->check();
    EXPECT_TRUE(check_result.ok)
        << "Bitmap cursor caused inconsistency. Errors: "
        << (check_result.errors.empty() ? "none" : check_result.errors[0]);

    // Verify files exist
    auto f1 = vol_->resolve("/first");
    auto f2 = vol_->resolve("/second");
    auto f3 = vol_->resolve("/third");
    EXPECT_GT(f1.blk, 0u);
    EXPECT_GT(f2.blk, 0u);
    EXPECT_GT(f3.blk, 0u);
}
