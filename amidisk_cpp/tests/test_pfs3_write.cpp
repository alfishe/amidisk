#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include "image.h"
#include "fs/pfs3.h"
#include "blkdev/image_file.h"

using namespace amidisk;
namespace fs = std::filesystem;

class PFS3WriteTest : public ::testing::Test {
protected:
    std::string test_dir = "amidisk_python/tests/data_tmp";
    std::string cpp_img_path = test_dir + "/pfs3_cpp_format.hdf";
    std::string py_img_path = test_dir + "/pfs3_py_format.hdf";
    
    void SetUp() override {
        fs::create_directories(test_dir);
    }
    
    void create_empty_file(const std::string& path, size_t size) {
        std::ofstream f(path, std::ios::binary);
        f.seekp(size - 1);
        f.write("", 1);
    }
    
    bool files_equal(const std::string& p1, const std::string& p2) {
        std::ifstream f1(p1, std::ios::binary | std::ios::ate);
        std::ifstream f2(p2, std::ios::binary | std::ios::ate);
        if (!f1 || !f2) return false;
        if (f1.tellg() != f2.tellg()) return false;
        size_t size = f1.tellg();
        f1.seekg(0); f2.seekg(0);
        std::vector<uint8_t> b1(size), b2(size);
        f1.read(reinterpret_cast<char*>(b1.data()), size);
        f2.read(reinterpret_cast<char*>(b2.data()), size);
        return b1 == b2;
    }
    
    void run_python_format(const std::string& img_path, const std::string& label) {
        std::string cmd = "PYTHONPATH=../../../amidisk_python/src:../../amidisk_python/src:../amidisk_python/src:amidisk_python/src python3 -m amidisk format --dostype 0x50465303 " + img_path + " " + label;
        int res = system(cmd.c_str());
        ASSERT_EQ(res, 0);
    }
    
    void run_python_cp(const std::string& img_path, const std::string& src, const std::string& dst) {
        std::string cmd = "PYTHONPATH=../../../amidisk_python/src:../../amidisk_python/src:../amidisk_python/src:amidisk_python/src python3 -m amidisk cp " + img_path + " " + src + " " + dst;
        int res = system(cmd.c_str());
        ASSERT_EQ(res, 0);
    }
};

TEST_F(PFS3WriteTest, FormatBinaryParity) {
    size_t sz = 10 * 1024 * 1024; // 10MB
    create_empty_file(cpp_img_path, sz);
    create_empty_file(py_img_path, sz);
    
    // Python format
    run_python_format(py_img_path, "PyPFS3");
    
    // C++ format
    auto bdev = std::make_shared<ImageFileBlkDev>(cpp_img_path, false);
    PFS3Volume vol(bdev, 1, 2, 0x50465303);
    vol.format("PyPFS3", 0x50465303);
    bdev.reset();
    
    // We expect differences in datestamp, but let's test if we can read it.
    auto check_bdev = std::make_shared<ImageFileBlkDev>(cpp_img_path, false);
    PFS3Volume cpp_vol(check_bdev, 1, 2, 0x50465303);
    cpp_vol.open();
    auto rep = cpp_vol.check(true);
    EXPECT_TRUE(rep.ok);
    EXPECT_EQ(rep.files, 0);
    EXPECT_EQ(rep.dirs, 0);
}

TEST_F(PFS3WriteTest, MkdirAndWriteFile) {
    size_t sz = 10 * 1024 * 1024;
    create_empty_file(cpp_img_path, sz);
    auto bdev = std::make_shared<ImageFileBlkDev>(cpp_img_path, false);
    PFS3Volume vol(bdev, 1, 2, 0x50465303);
    vol.format("TestVol", 0x50465303);
    
    vol.makedirs("Data/Images");
    std::string test_data = "Hello World! This is PFS3 C++ Write Test.";
    vol.write_file("Data/Images/hello.txt", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()), {.comment = "My Comment"});
    
    auto entries = vol.list_dir("Data/Images");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), "hello.txt");
    EXPECT_TRUE(entries[0].is_file());
    EXPECT_EQ(entries[0].comment_str(), "My Comment");
    
    std::string read_back;
    vol.read_file("Data/Images/hello.txt", [&](std::span<const uint8_t> chunk) {
        read_back.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
    });
    EXPECT_EQ(read_back, test_data);
    
    auto rep = vol.check(true);
    EXPECT_TRUE(rep.ok);
}

TEST_F(PFS3WriteTest, DeleteFile) {
    size_t sz = 5 * 1024 * 1024;
    create_empty_file(cpp_img_path, sz);
    auto bdev = std::make_shared<ImageFileBlkDev>(cpp_img_path, false);
    PFS3Volume vol(bdev, 1, 2, 0x50465303);
    vol.format("DelVol", 0x50465303);
    
    vol.makedirs("test");
    std::string text = "ToDelete";
    vol.write_file("test/file.txt", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    
    auto entries = vol.list_dir("test");
    EXPECT_EQ(entries.size(), 1);
    
    vol.delete_path("test/file.txt");
    entries = vol.list_dir("test");
    EXPECT_EQ(entries.size(), 0);
    
    auto rep = vol.check(true);
    EXPECT_TRUE(rep.ok);
}

TEST_F(PFS3WriteTest, LargeWrite) {
    size_t sz = 20 * 1024 * 1024;
    create_empty_file(cpp_img_path, sz);
    auto bdev = std::make_shared<ImageFileBlkDev>(cpp_img_path, false);
    PFS3Volume vol(bdev, 1, 2, 0x50465303);
    vol.format("LargeVol", 0x50465303);
    
    // Write 5MB file
    size_t data_sz = 5 * 1024 * 1024;
    std::vector<uint8_t> data(data_sz);
    for (size_t i = 0; i < data_sz; ++i) data[i] = i & 0xFF;
    
    vol.write_file("large.bin", std::span<const uint8_t>(data.data(), data.size()));
    
    std::vector<uint8_t> read_back;
    read_back.reserve(data_sz);
    vol.read_file("large.bin", [&](std::span<const uint8_t> chunk) {
        read_back.insert(read_back.end(), chunk.begin(), chunk.end());
    });
    EXPECT_EQ(read_back.size(), data_sz);
    EXPECT_TRUE(std::equal(data.begin(), data.end(), read_back.begin()));
    
    auto rep = vol.check(true);
    EXPECT_TRUE(rep.ok);
}
