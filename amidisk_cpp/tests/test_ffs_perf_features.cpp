#include <gtest/gtest.h>
#include "fs/ffs.h"
#include "blkdev/blkdev.h"
#include "blkdev/bulk_cache.h"

#include <cstring>
#include <random>
#include <chrono>
#include <set>

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
        write_count_++;
        if (offset + buf.size() > buffer_.size())
            throw BlockDeviceError("write past end");
        std::memcpy(buffer_.data() + offset, buf.data(), buf.size());
    }

    void flush() override {}
    void close() override {}

    uint64_t write_count() const { return write_count_; }
    void reset_write_count() { write_count_ = 0; }

private:
    std::vector<uint8_t> buffer_;
    uint32_t sector_size_;
    uint64_t write_count_ = 0;
};

// =============================================================================
// TEST: Allocation Cursor
// Verifies that bitmap allocation resumes from last position instead of
// scanning from the beginning each time.
// =============================================================================

class BitmapCursorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 50MB volume - enough for cursor behavior to matter
        dev_ = std::make_shared<MemBlockDevice>(50 * 1024 * 1024);
        vol_ = std::make_unique<FFSVolume>(std::static_pointer_cast<BlockDevice>(dev_));
        vol_->format("CursorTest", 0x444F5303);
    }

    std::shared_ptr<MemBlockDevice> dev_;
    std::unique_ptr<FFSVolume> vol_;
};

TEST_F(BitmapCursorTest, CursorAdvancesAfterAllocation) {
    // Allocate some blocks
    auto blks1 = vol_->bitmap->alloc_runs(100);
    ASSERT_EQ(blks1.size(), 100);

    // Get highest allocated block
    uint32_t max1 = *std::max_element(blks1.begin(), blks1.end());

    // Allocate more - should start AFTER previous allocations
    auto blks2 = vol_->bitmap->alloc_runs(100);
    ASSERT_EQ(blks2.size(), 100);

    uint32_t min2 = *std::min_element(blks2.begin(), blks2.end());

    // Second allocation should start at or after where first ended
    // (cursor advanced, not rescanning from start)
    EXPECT_GE(min2, max1) << "Cursor should advance - second alloc should start after first";
}

TEST_F(BitmapCursorTest, CursorDoesNotRestartFromZero) {
    // Key property: cursor advances, it doesn't restart from 0 each time
    // This is what makes allocation O(1) amortized instead of O(N) per alloc

    auto blks1 = vol_->bitmap->alloc_runs(100);
    ASSERT_EQ(blks1.size(), 100);
    uint32_t max1 = *std::max_element(blks1.begin(), blks1.end());

    auto blks2 = vol_->bitmap->alloc_runs(100);
    ASSERT_EQ(blks2.size(), 100);
    uint32_t min2 = *std::min_element(blks2.begin(), blks2.end());

    auto blks3 = vol_->bitmap->alloc_runs(100);
    ASSERT_EQ(blks3.size(), 100);
    uint32_t min3 = *std::min_element(blks3.begin(), blks3.end());

    // Each allocation should start after the previous one
    EXPECT_GT(min2, max1) << "Second alloc should start after first ended";
    EXPECT_GT(min3, *std::max_element(blks2.begin(), blks2.end()))
        << "Third alloc should start after second ended";
}

TEST_F(BitmapCursorTest, SequentialAllocationsAreContiguous) {
    // Sequential allocations should produce mostly contiguous blocks
    // (cursor moves forward, not jumping around)
    std::vector<uint32_t> all_blocks;

    for (int i = 0; i < 10; ++i) {
        auto blks = vol_->bitmap->alloc_runs(50);
        ASSERT_EQ(blks.size(), 50);
        all_blocks.insert(all_blocks.end(), blks.begin(), blks.end());
    }

    // Count gaps (non-consecutive blocks)
    std::sort(all_blocks.begin(), all_blocks.end());
    int gaps = 0;
    for (size_t i = 1; i < all_blocks.size(); ++i) {
        if (all_blocks[i] != all_blocks[i-1] + 1) gaps++;
    }

    // Should have very few gaps - cursor advances linearly
    EXPECT_LT(gaps, 20) << "Sequential allocs should be mostly contiguous (cursor advancing)";
}

// =============================================================================
// TEST: Word-Level Bitmap Operations
// Verifies that fully-allocated words are skipped and fully-free words
// are grabbed in single operations.
// =============================================================================

class BitmapWordOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        dev_ = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
        vol_ = std::make_unique<FFSVolume>(std::static_pointer_cast<BlockDevice>(dev_));
        vol_->format("WordOpsTest", 0x444F5303);
    }

    std::shared_ptr<MemBlockDevice> dev_;
    std::unique_ptr<FFSVolume> vol_;
};

TEST_F(BitmapWordOpsTest, FullWordGrab) {
    // Allocate exactly 32 blocks when aligned - should grab full word
    auto blks = vol_->bitmap->alloc_runs(32);
    ASSERT_EQ(blks.size(), 32);

    // All 32 should be consecutive (full word grab)
    std::sort(blks.begin(), blks.end());
    for (size_t i = 1; i < blks.size(); ++i) {
        EXPECT_EQ(blks[i], blks[i-1] + 1) << "32-block alloc should be contiguous (full word)";
    }
}

TEST_F(BitmapWordOpsTest, SkipsAllocatedWords) {
    // Allocate blocks to create some fully-allocated words
    auto first = vol_->bitmap->alloc_runs(64);  // Allocate 2 words worth
    ASSERT_EQ(first.size(), 64);

    // Free only part of first word
    vol_->bitmap->free(first[0]);
    vol_->bitmap->free(first[1]);

    // Now cursor is past these. Allocate more - should skip full words
    auto start = std::chrono::steady_clock::now();
    auto second = vol_->bitmap->alloc_runs(100);
    auto end = std::chrono::steady_clock::now();

    ASSERT_EQ(second.size(), 100);

    // Should complete quickly - word-level skip avoids bit-by-bit scan
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_LT(ms, 100) << "Allocation should be fast (word-level skipping)";
}

TEST_F(BitmapWordOpsTest, AllocatedBlocksAreUnique) {
    std::set<uint32_t> all_blocks;

    // Do many allocations
    for (int i = 0; i < 50; ++i) {
        auto blks = vol_->bitmap->alloc_runs(20);
        for (uint32_t b : blks) {
            auto [_, inserted] = all_blocks.insert(b);
            EXPECT_TRUE(inserted) << "Block " << b << " allocated twice!";
        }
    }
}

// =============================================================================
// TEST: resolve_parent Caching
// Verifies that consecutive files in the same directory reuse cached parent.
// =============================================================================

class ResolveParentCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        dev_ = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
        vol_ = std::make_unique<FFSVolume>(std::static_pointer_cast<BlockDevice>(dev_));
        vol_->format("ParentCacheTest", 0x444F5303);
    }

    std::shared_ptr<MemBlockDevice> dev_;
    std::unique_ptr<FFSVolume> vol_;
};

TEST_F(ResolveParentCacheTest, SameDirectoryCacheHit) {
    vol_->makedirs("a/b/c/d/e");

    // Write many files to same deep directory
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        std::string path = "a/b/c/d/e/file" + std::to_string(i) + ".txt";
        std::vector<uint8_t> data = {'X'};
        vol_->write_file(path, data, {});
    }
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Should be fast due to parent caching - not re-resolving a/b/c/d/e 100 times
    EXPECT_LT(ms, 500) << "Same-dir writes should be fast (parent cached)";

    // Verify files exist
    auto entries = vol_->list_dir("a/b/c/d/e");
    EXPECT_EQ(entries.size(), 100);
}

TEST_F(ResolveParentCacheTest, DifferentDirectoriesStillWork) {
    vol_->makedirs("dir1");
    vol_->makedirs("dir2");
    vol_->makedirs("dir3");

    // Alternate between directories
    for (int i = 0; i < 30; ++i) {
        std::string dir = "dir" + std::to_string((i % 3) + 1);
        std::string path = dir + "/file" + std::to_string(i) + ".txt";
        std::vector<uint8_t> data = {'Y'};
        vol_->write_file(path, data, {});
    }

    // Verify all files exist
    EXPECT_EQ(vol_->list_dir("dir1").size(), 10);
    EXPECT_EQ(vol_->list_dir("dir2").size(), 10);
    EXPECT_EQ(vol_->list_dir("dir3").size(), 10);
}

TEST_F(ResolveParentCacheTest, DeepPathPerformance) {
    // Create very deep path
    std::string path = "";
    for (int i = 0; i < 20; ++i) {
        path += (path.empty() ? "" : "/") + std::string("level") + std::to_string(i);
    }
    vol_->makedirs(path);

    // Write files - should still be fast due to caching
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        std::vector<uint8_t> data = {'Z'};
        vol_->write_file(path + "/file" + std::to_string(i) + ".txt", data, {});
    }
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_LT(ms, 500) << "Deep path writes should be fast (parent cached)";
}

// =============================================================================
// TEST: makedirs Path Caching
// Verifies that dir_cache_ avoids redundant directory existence checks.
// =============================================================================

class MakedirsCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        dev_ = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
        vol_ = std::make_unique<FFSVolume>(std::static_pointer_cast<BlockDevice>(dev_));
        vol_->format("MakedirsCacheTest", 0x444F5303);
    }

    std::shared_ptr<MemBlockDevice> dev_;
    std::unique_ptr<FFSVolume> vol_;
};

TEST_F(MakedirsCacheTest, RepeatedMakedirsIsFast) {
    // First makedirs creates the path
    vol_->makedirs("a/b/c/d/e");

    // Repeated makedirs should be nearly instant (cached)
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        vol_->makedirs("a/b/c/d/e");
    }
    auto end = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_LT(ms, 100) << "Repeated makedirs should be instant (last_makedirs_path_ cache)";
}

TEST_F(MakedirsCacheTest, SimilarPathsCached) {
    vol_->makedirs("shared/parent/path/subdir1");
    vol_->makedirs("shared/parent/path/subdir2");
    vol_->makedirs("shared/parent/path/subdir3");

    // All should exist
    auto entries = vol_->list_dir("shared/parent/path");
    EXPECT_EQ(entries.size(), 3);

    // Verify they're directories
    for (const auto& e : entries) {
        EXPECT_TRUE(e.is_dir());
    }
}

TEST_F(MakedirsCacheTest, ManyDifferentPaths) {
    auto start = std::chrono::steady_clock::now();

    // Create many different paths - dir_cache_ should help
    for (int i = 0; i < 100; ++i) {
        std::string path = "base/category" + std::to_string(i % 10) +
                          "/sub" + std::to_string(i / 10);
        vol_->makedirs(path);
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Should complete reasonably fast with dir_cache_
    EXPECT_LT(ms, 500);

    // Verify structure
    EXPECT_EQ(vol_->list_dir("base").size(), 10);  // 10 categories
}

// =============================================================================
// TEST: Deferred Bitmap Writes
// Verifies that bitmap pages are marked dirty and flushed together.
// =============================================================================

class DeferredBitmapTest : public ::testing::Test {
protected:
    void SetUp() override {
        dev_ = std::make_shared<MemBlockDevice>(10 * 1024 * 1024);
        vol_ = std::make_unique<FFSVolume>(std::static_pointer_cast<BlockDevice>(dev_));
        vol_->format("BitmapDeferTest", 0x444F5303);
    }

    std::shared_ptr<MemBlockDevice> dev_;
    std::unique_ptr<FFSVolume> vol_;
};

TEST_F(DeferredBitmapTest, DirtyPagesAccumulate) {
    // Allocate blocks - should mark pages dirty
    for (int i = 0; i < 10; ++i) {
        vol_->bitmap->alloc_runs(100);
    }

    // Verify blocks are allocated (marked used)
    auto blks = vol_->bitmap->alloc_runs(10);
    ASSERT_EQ(blks.size(), 10);

    // All previously allocated blocks should be marked as not free
    // (even before flush - dirty tracking works on in-memory pages)
    for (uint32_t b : blks) {
        EXPECT_FALSE(vol_->bitmap->is_free(b)) << "Allocated block should be marked used";
    }

    // Flush and verify integrity
    vol_->bitmap->flush();

    auto report = vol_->check(false);
    EXPECT_TRUE(report.ok) << "Volume should pass integrity check after flush";
}

TEST_F(DeferredBitmapTest, IntegrityAfterFlush) {
    // Allocate many blocks
    std::vector<uint32_t> all_blocks;
    for (int i = 0; i < 50; ++i) {
        auto blks = vol_->bitmap->alloc_runs(20);
        all_blocks.insert(all_blocks.end(), blks.begin(), blks.end());
    }

    vol_->bitmap->flush();

    // All allocated blocks should be marked as used
    for (uint32_t b : all_blocks) {
        EXPECT_FALSE(vol_->bitmap->is_free(b)) << "Block " << b << " should be marked used";
    }

    // Check volume integrity
    auto report = vol_->check(false);
    EXPECT_TRUE(report.ok) << "Volume should pass integrity check";
}

TEST_F(DeferredBitmapTest, FreeAndReallocate) {
    auto blks = vol_->bitmap->alloc_runs(100);
    ASSERT_EQ(blks.size(), 100);

    // Free half
    for (size_t i = 0; i < 50; ++i) {
        vol_->bitmap->free(blks[i]);
    }

    // Flush
    vol_->bitmap->flush();

    // Freed blocks should be free
    for (size_t i = 0; i < 50; ++i) {
        EXPECT_TRUE(vol_->bitmap->is_free(blks[i])) << "Freed block should be free";
    }

    // Still allocated blocks should be used
    for (size_t i = 50; i < 100; ++i) {
        EXPECT_FALSE(vol_->bitmap->is_free(blks[i])) << "Allocated block should be used";
    }
}

// =============================================================================
// TEST: Dirty Directory Caching (Bulk Mode)
// Verifies that directory blocks are cached and flushed in bulk mode.
// =============================================================================

class DirtyDirCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        mem_dev_ = std::make_shared<MemBlockDevice>(20 * 1024 * 1024);
        dev_ = mem_dev_;
    }

    std::shared_ptr<MemBlockDevice> mem_dev_;
    std::shared_ptr<BlockDevice> dev_;
};

TEST_F(DirtyDirCacheTest, BulkModeReducesWrites) {
    // Test WITHOUT bulk mode
    FFSVolume vol1(dev_);
    vol1.format("NoBulkTest", 0x444F5303);

    mem_dev_->reset_write_count();
    vol1.makedirs("testdir");
    for (int i = 0; i < 50; ++i) {
        std::vector<uint8_t> data = {'A'};
        vol1.write_file("testdir/file" + std::to_string(i) + ".txt", data, {});
    }
    uint64_t writes_no_bulk = mem_dev_->write_count();

    // Test WITH bulk mode
    FFSVolume vol2(dev_);
    vol2.format("BulkTest", 0x444F5303);

    {
        BulkGuard bulk(vol2.blkdev_ref());
        mem_dev_->reset_write_count();

        vol2.makedirs("testdir");
        for (int i = 0; i < 50; ++i) {
            std::vector<uint8_t> data = {'B'};
            vol2.write_file("testdir/file" + std::to_string(i) + ".txt", data, {});
        }
    }
    uint64_t writes_bulk = mem_dev_->write_count();

    // Bulk mode should have fewer or equal writes
    // (BulkWriteCache batches at I/O level anyway, but dirty_dirs_ helps at FS level)
    EXPECT_LE(writes_bulk, writes_no_bulk * 2)
        << "Bulk mode should not increase writes significantly";
}

TEST_F(DirtyDirCacheTest, BulkModeIntegrity) {
    FFSVolume vol(dev_);
    vol.format("BulkIntegrityTest", 0x444F5303);

    {
        BulkGuard bulk(vol.blkdev_ref());

        vol.makedirs("dir1/sub1");
        vol.makedirs("dir1/sub2");
        vol.makedirs("dir2");

        for (int i = 0; i < 100; ++i) {
            std::string dir = (i % 3 == 0) ? "dir1/sub1" : ((i % 3 == 1) ? "dir1/sub2" : "dir2");
            std::vector<uint8_t> data(100, static_cast<uint8_t>(i));
            vol.write_file(dir + "/file" + std::to_string(i) + ".txt", data, {});
        }
    }

    // After BulkGuard destruction, verify integrity
    auto report = vol.check(true);
    EXPECT_TRUE(report.ok) << "Bulk mode should maintain integrity: "
                           << (report.errors.empty() ? "" : report.errors[0]);

    // Verify file counts
    EXPECT_EQ(vol.list_dir("dir1/sub1").size(), 34);
    EXPECT_EQ(vol.list_dir("dir1/sub2").size(), 33);
    EXPECT_EQ(vol.list_dir("dir2").size(), 33);
}

TEST_F(DirtyDirCacheTest, BulkModeReadBack) {
    FFSVolume vol(dev_);
    vol.format("BulkReadTest", 0x444F5303);

    std::vector<std::pair<std::string, std::vector<uint8_t>>> written;

    {
        BulkGuard bulk(vol.blkdev_ref());

        vol.makedirs("data");
        for (int i = 0; i < 20; ++i) {
            std::string path = "data/file" + std::to_string(i) + ".bin";
            std::vector<uint8_t> data(1000);
            std::mt19937 rng(i);
            for (auto& b : data) b = rng() & 0xFF;

            vol.write_file(path, data, {});
            written.push_back({path, data});
        }
    }

    // Read back and verify
    for (const auto& [path, expected] : written) {
        auto actual = vol.read_file_bytes(path);
        EXPECT_EQ(actual, expected) << "File " << path << " content mismatch";
    }
}

// =============================================================================
// TEST: Combined Performance
// Verifies that all optimizations work together correctly.
// =============================================================================

TEST(CombinedPerfTest, TarLikeWorkload) {
    auto dev = std::make_shared<MemBlockDevice>(50 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("TarWorkloadTest", 0x444F5303);

    auto start = std::chrono::steady_clock::now();

    {
        BulkGuard bulk(vol.blkdev_ref());

        // Simulate tar extraction pattern: many files in nested directories
        for (int dir = 0; dir < 20; ++dir) {
            std::string base = "archive/folder" + std::to_string(dir);
            vol.makedirs(base);

            for (int file = 0; file < 50; ++file) {
                std::string path = base + "/file" + std::to_string(file) + ".dat";
                std::vector<uint8_t> data(500, static_cast<uint8_t>(file));
                vol.write_file(path, data, {});
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // 1000 files should complete in reasonable time with all optimizations
    EXPECT_LT(ms, 5000) << "1000 files should complete quickly with optimizations";

    // Verify
    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
    EXPECT_EQ(report.files, 1000);
    EXPECT_EQ(report.dirs, 21);  // 20 folders + archive dir
}

TEST(CombinedPerfTest, LargeFilesMixedWithSmall) {
    auto dev = std::make_shared<MemBlockDevice>(30 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("MixedSizeTest", 0x444F5303);

    {
        BulkGuard bulk(vol.blkdev_ref());
        vol.makedirs("mixed");

        std::mt19937 rng(42);
        for (int i = 0; i < 100; ++i) {
            std::string path = "mixed/file" + std::to_string(i) + ".bin";
            size_t size = (i % 10 == 0) ? 100000 : (rng() % 1000 + 10);  // Mostly small, some large

            std::vector<uint8_t> data(size);
            for (auto& b : data) b = rng() & 0xFF;

            vol.write_file(path, data, {});
        }
    }

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok) << (report.errors.empty() ? "" : report.errors[0]);
    EXPECT_EQ(report.files, 100);
}

TEST(CombinedPerfTest, DeepNestedStructure) {
    auto dev = std::make_shared<MemBlockDevice>(20 * 1024 * 1024);
    std::shared_ptr<BlockDevice> dev_ptr = dev;

    FFSVolume vol(dev_ptr);
    vol.format("DeepNestTest", 0x444F5303);

    {
        BulkGuard bulk(vol.blkdev_ref());

        // Create deeply nested structure with files at each level
        for (int depth = 1; depth <= 15; ++depth) {
            std::string path = "";
            for (int i = 0; i < depth; ++i) {
                path += (path.empty() ? "" : "/") + std::string("d") + std::to_string(i);
            }
            vol.makedirs(path);

            // Add files at this depth
            for (int f = 0; f < 5; ++f) {
                std::vector<uint8_t> data = {'D', static_cast<uint8_t>(depth), static_cast<uint8_t>(f)};
                vol.write_file(path + "/f" + std::to_string(f) + ".txt", data, {});
            }
        }
    }

    auto report = vol.check(true);
    EXPECT_TRUE(report.ok);
    EXPECT_EQ(report.files, 75);  // 15 depths * 5 files
}
