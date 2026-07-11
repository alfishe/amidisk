#include <gtest/gtest.h>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>
#include "fs/sfs.h"
#include "blkdev/image_file.h"

using namespace amidisk;

class SFSWriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::system("rm -rf data_tmp");
        std::system("mkdir -p data_tmp");
    }

    void TearDown() override {
        // std::system("rm -rf data_tmp");
    }

    std::vector<uint8_t> readFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    }

    void create_empty_file(const std::string& path, size_t size) {
        std::ofstream f(path, std::ios::binary);
        f.seekp(size - 1);
        f.write("", 1);
    }
};

TEST_F(SFSWriteTest, FormatBinaryParity) {
    size_t sz = 1024 * 1024;
    create_empty_file("data_tmp/sfs_py.img", sz);
    create_empty_file("data_tmp/sfs_cpp.img", sz);

    std::string py_cmd = "PYTHONPATH=../../../amidisk_python/src:../../amidisk_python/src:../amidisk_python/src:amidisk_python/src python3 -m amidisk format --dostype 0x53465300 data_tmp/sfs_py.img Empty";
    int ret = std::system(py_cmd.c_str());
    EXPECT_EQ(ret, 0);

    auto dev = std::make_shared<ImageFileBlkDev>("data_tmp/sfs_cpp.img", false);
    SFSVolume vol(dev, 0x53465300);
    vol.format("Empty", 0x53465300);

    auto py_data = readFile("data_tmp/sfs_py.img");
    auto cpp_data = readFile("data_tmp/sfs_cpp.img");

    EXPECT_EQ(py_data.size(), cpp_data.size());

    for (size_t i = 0; i < py_data.size(); ++i) {
        if (py_data[i] != cpp_data[i]) {
            std::cout << "Mismatch at byte " << i << " (Block " << i / 512 << ", offset " << i % 512 << ")\n";
            std::cout << "Py: " << std::hex << (int)py_data[i] << " Cpp: " << (int)cpp_data[i] << std::dec << "\n";
            EXPECT_EQ(py_data[i], cpp_data[i]);
            break;
        }
    }
}

TEST_F(SFSWriteTest, MkdirAndWriteFile) {
    size_t sz = 1024 * 1024;
    create_empty_file("data_tmp/sfs_cpp_write.img", sz);

    auto dev = std::make_shared<ImageFileBlkDev>("data_tmp/sfs_cpp_write.img", false);
    SFSVolume vol(dev, 0x53465300);
    vol.format("Empty", 0x53465300);
    
    vol.makedirs("Dir1/SubDir");
    std::string test_data = "Hello from AmigaFSTool SFS C++ Port!";
    vol.write_file("Dir1/SubDir/hello.txt", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()), {.comment = "A Comment"});

    auto entries = vol.list_dir("Dir1/SubDir");
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), "hello.txt");
    EXPECT_EQ(entries[0].comment_str(), "A Comment");
    EXPECT_TRUE(entries[0].is_file());

    std::string read_back;
    vol.read_file("Dir1/SubDir/hello.txt", [&](std::span<const uint8_t> chunk) {
        read_back.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
    });
    EXPECT_EQ(read_back, test_data);
}

TEST_F(SFSWriteTest, DeleteFile) {
    size_t sz = 1024 * 1024;
    create_empty_file("data_tmp/sfs_cpp_del_file.img", sz);

    auto dev = std::make_shared<ImageFileBlkDev>("data_tmp/sfs_cpp_del_file.img", false);
    SFSVolume vol(dev, 0x53465300);
    vol.format("Empty", 0x53465300);
    
    std::string test_data = "To be deleted";
    vol.write_file("delete_me.txt", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()));
    
    auto entries = vol.list_dir("");
    ASSERT_EQ(entries.size(), 2);
    
    vol.delete_path("delete_me.txt", false);
    
    entries = vol.list_dir("");
    EXPECT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), ".recycled");
}

TEST_F(SFSWriteTest, DeleteDirectoryRecursive) {
    size_t sz = 1024 * 1024;
    create_empty_file("data_tmp/sfs_cpp_del_dir.img", sz);

    auto dev = std::make_shared<ImageFileBlkDev>("data_tmp/sfs_cpp_del_dir.img", false);
    SFSVolume vol(dev, 0x53465300);
    vol.format("Empty", 0x53465300);
    
    vol.makedirs("Dir1/SubDir1");
    vol.makedirs("Dir1/SubDir2");
    std::string test_data = "Data";
    vol.write_file("Dir1/SubDir1/file1.txt", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()));
    vol.write_file("Dir1/SubDir1/file2.txt", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size()));
    
    auto entries = vol.list_dir("Dir1/SubDir1");
    ASSERT_EQ(entries.size(), 2);
    
    vol.delete_path("Dir1", true);
    
    entries = vol.list_dir("");
    EXPECT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].name_str(), ".recycled");
}
