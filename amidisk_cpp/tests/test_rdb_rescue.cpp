#include <gtest/gtest.h>
#include "rdb/rescue.h"
#include "rdb/rdisk.h"
#include "blkdev/blkdev.h"
#include "image.h"
#include <fstream>
#include <filesystem>

using namespace amidisk;

TEST(RDBRescueTest, RebuildsRDB) {
    std::string tmp_img = "test_rescue.img";
    std::ofstream f(tmp_img, std::ios::binary);
    f.seekp(4 * 1024 * 1024 - 1);
    f.write("", 1);
    f.close();
    
    auto img = DiskImage::open(tmp_img, false);
    auto dev = img->blkdev();
    
    // Create an RDB
    auto rd = RDisk::create(dev, 32, 2, 2);
    rd->add_partition_exact("DH0", 2, 20, 0x444f5303, 1, true, 0); // DOS\3
    rd->add_partition_exact("DH1", 21, 50, 0x50465303, 1, false, 0); // PFS\3
    rd->add_partition_exact("DH2", 51, 80, 0x53465300, 1, false, 0); // SFS\0
    
    // Write fake SFS and PFS3 headers at their expected offsets
    // DH1 is PFS3 at cyl 21 (sector 21*2*32 = 1344)
    std::vector<uint8_t> pfs3_header(512, 0);
    pfs3_header[4] = 0; pfs3_header[5] = 0; pfs3_header[6] = 0; pfs3_header[7] = 1; // options
    pfs3_header[84] = 0; pfs3_header[85] = 0; pfs3_header[86] = 0; pfs3_header[87] = 50-21+1; // disksize
    pfs3_header[20] = 3; pfs3_header[21] = 'P'; pfs3_header[22] = 'F'; pfs3_header[23] = 'S'; // label
    dev->write((1344 + 2) * 512, pfs3_header);
    
    // Wipe out the RDSK block (sector 0)
    std::vector<uint8_t> empty(512, 0);
    dev->write(0, empty);
    
    // Scan
    auto scan_res = Rescue::scan(dev);
    ASSERT_EQ(scan_res.first.size(), 3); // The three PART blocks should still be intact
    
    // Rebuild
    auto new_rd = Rescue::rebuild(dev, scan_res.first);
    ASSERT_NE(new_rd, nullptr);
    
    // Verify it was rebuilt correctly
    ASSERT_EQ(new_rd->partitions().size(), 3);
    EXPECT_EQ(new_rd->partitions()[0]->drv_name(), "DH0");
    EXPECT_EQ(new_rd->partitions()[0]->get_dos_type_str(), "DOS\\3 (FFS-Intl)");
    EXPECT_EQ(new_rd->partitions()[1]->drv_name(), "DH1");
    EXPECT_EQ(new_rd->partitions()[1]->get_dos_type_str(), "PFS\\3 (PFS3)");
    EXPECT_EQ(new_rd->partitions()[2]->drv_name(), "DH2");
    EXPECT_EQ(new_rd->partitions()[2]->get_dos_type_str(), "SFS\\0 (Smart File System v1)");
}
