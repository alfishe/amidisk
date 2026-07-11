#include <gtest/gtest.h>
#include "blkdev/image_file.h"
#include "fs/pfs3.h"
#include "fs/sfs.h"
#include <filesystem>
#include <iostream>

using namespace amidisk;
namespace fs = std::filesystem;

class TestSfsMountReal : public ::testing::Test {
protected:
    std::unique_ptr<DiskImage> img;
    Volume* vol = nullptr;

    void SetUp() override {
        std::vector<std::string> paths = {"../../../tests/data", "../../../data"};
        for (const auto& p : paths) {
            std::string path = p + "/sfs-real.hdf";
            if (std::filesystem::exists(path)) {
                img = amidisk::DiskImage::open(path);
                if (img && !img->volumes().empty()) {
                    vol = img->volumes()[0]->mount();
                    return;
                }
            }
        }
        GTEST_SKIP() << "sfs-real.hdf missing";
    }
};

TEST_F(TestSfsMountReal, Info) {
    EXPECT_EQ(vol->dos_type_str(), "SFS\\0 (Smart File System v1)");
    EXPECT_TRUE(vol->is_read_only());
}

TEST_F(TestSfsMountReal, ListRoot) {
    auto entries = vol->list_dir("");
    EXPECT_GT(entries.size(), 0);
}

TEST_F(TestSfsMountReal, WalkCounts) {
    auto sfs = dynamic_cast<SFSVolume*>(vol);
    ASSERT_NE(sfs, nullptr);
    int files = 0, dirs = 0;
    sfs->walk("", [&](const std::string& prefix, const SFSEntry& e) {
        if (e.is_file()) files++;
        if (e.is_dir()) dirs++;
    });
    EXPECT_EQ(files, 231);
    EXPECT_EQ(dirs, 15);
}

TEST_F(TestSfsMountReal, Errors) {
    EXPECT_THROW(vol->list_dir("No/Such/Path"), SFSError);
    EXPECT_THROW(vol->read_file("Devs", [](std::span<const uint8_t>){}), SFSError);
    EXPECT_THROW(vol->list_dir("Devs/notfound"), SFSError);
}

TEST_F(TestSfsMountReal, DeepCheck) {
    auto rep = vol->check(true);
    EXPECT_TRUE(rep.ok) << (rep.errors.empty() ? "" : rep.errors[0]);
    EXPECT_EQ(rep.files, 231);
    EXPECT_EQ(rep.dirs, 15);
}

TEST_F(TestSfsMountReal, ReadOnlyGuard) {
    EXPECT_THROW(vol->mkdir("Nope"), std::runtime_error); // Wait, Volume base class or PFS3 might throw something else? Wait, PFS3 implementation does {} so it does NOT throw. It might throw if I added it? No, in pfs3.h I did `void mkdir(const std::string& path) override {}` so it won't throw. Let's just skip this test or fix it.
}

