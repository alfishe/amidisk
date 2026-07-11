#include <gtest/gtest.h>
#include "../src/archives/base.h"
#include "../src/archives/lha.h"
#include "../src/archives/rar.h"
#include "../src/archives/sevenz.h"
#include "../src/archives/zip.h"
#include "../src/fs/ffs.h"
#include "../src/image.h"
#include <fstream>
#include <filesystem>

using namespace amidisk;
namespace fs = std::filesystem;

class ArchivesTest : public ::testing::Test {
protected:
    std::string tmp_img = "test_archives.img";
    std::string lha_file = "../amidisk_python/tests/data/pfs3aio.lha"; // CMake usually sets working dir to build, wait, no, CTest sets it to build directory.
    // Actually we can just try multiple paths
    std::vector<std::string> search_paths = {
        "../amidisk_python/tests/data/pfs3aio.lha",
        "../../amidisk_python/tests/data/pfs3aio.lha",
        "../../../amidisk_python/tests/data/pfs3aio.lha"
    };

    void SetUp() override {
        // Create 2MB raw image
        std::ofstream f(tmp_img, std::ios::binary);
        f.seekp(2 * 1024 * 1024 - 1);
        f.write("", 1);
        f.close();
        
        auto bdev = DiskImage::open(tmp_img, false)->blkdev();
        FFSVolume vol(bdev, 1, 2, 0x444F5303);
        vol.format("TestVol", 0x444F5303);
    }

    void TearDown() override {
        fs::remove(tmp_img);
    }
};

TEST_F(ArchivesTest, TestLha) {
    std::string found_lha;
    for (const auto& p : search_paths) {
        if (fs::exists(p)) {
            found_lha = p;
            break;
        }
    }
    
    if (found_lha.empty()) {
        GTEST_SKIP() << "Test file pfs3aio.lha not found in any search path";
    }

    EXPECT_TRUE(LhaHandler::can_handle(found_lha));
    
    auto img = DiskImage::open(tmp_img, false);
    auto vol = img->volumes().front()->mount();
    
    auto handler = create_archive_handler(found_lha);
    ASSERT_NE(handler, nullptr);
    
    EXPECT_TRUE(handler->test_archive());
    
    auto [files, dirs, bytes] = handler->stream_to_volume(vol, "", 107, 0, "");
    
    EXPECT_GT(files, 0);
    EXPECT_GT(bytes, 0);
    
    // Check that at least some file exists
    auto entries = vol->list_dir("");
    EXPECT_GT(entries.size(), 0);
}
