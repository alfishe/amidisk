#include "blocks.h"
#include <cstring>
#include <map>
#include <algorithm>

namespace amidisk {

bool checksum_ok(std::span<const uint8_t> data, uint32_t num_longs) {
    if (data.size() < num_longs * 4) return false;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < num_longs; ++i) {
        sum += read_be32(data, i * 4);
    }
    return sum == 0;
}

void fix_checksum(std::span<uint8_t> data, uint32_t num_longs, uint32_t chk_loc) {
    if (data.size() < num_longs * 4) return;
    write_be32(data, chk_loc * 4, 0);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < num_longs; ++i) {
        sum += read_be32(data, i * 4);
    }
    write_be32(data, chk_loc * 4, static_cast<uint32_t>(-static_cast<int32_t>(sum)));
}

std::string dos_type_to_str(uint32_t dt) {
    static const std::map<uint32_t, std::string> known = {
        {0x444f5300, "DOS\\0 (OFS Non-intl)"},
        {0x444f5301, "DOS\\1 (FFS Non-intl)"},
        {0x444f5302, "DOS\\2 (OFS-Intl)"},
        {0x444f5303, "DOS\\3 (FFS-Intl)"},
        {0x444f5304, "DOS\\4 (OFS-DirCache)"},
        {0x444f5305, "DOS\\5 (FFS-DirCache)"},
        {0x444f5306, "DOS\\6 (OFS-LNFS)"},
        {0x444f5307, "DOS\\7 (FFS-LNFS)"},
        {0x53465300, "SFS\\0 (Smart File System v1)"},
        {0x53465302, "SFS\\2 (Smart File System v2)"},
        {0x50465300, "PFS\\0 (PFS/PFS2 Floppy/Basic)"},
        {0x50465301, "PFS\\1 (PFS Hard disk)"},
        {0x50465302, "PFS\\2 (PFS3 Legacy)"},
        {0x50465303, "PFS\\3 (PFS3)"},
        {0x50465333, "PFS3 (PFS3 Modern Standard)"},
        {0x4a584604, "JXF\\4 (JXFS AmigaOS 4/MorphOS)"},
        {0x53574150, "SWAP (Amiga Virtual/Swap)"},
        {0x4d414300, "MAC\\0 (Mac OS)"},
        {0x4c4e5800, "LNX\\0 (Linux native)"},
        {0x53575000, "SWP\\0 (Linux swap)"},
        {0x46415400, "FAT\\0 (FAT12/16)"},
        {0x46415401, "FAT\\1 (FAT32)"},
        {0x4e544653, "NTFS (Windows NTFS)"},
        {0x65784654, "exFT (exFAT)"},
        {0x45585434, "EXT4 (Linux EXT4)"}
    };
    auto it = known.find(dt);
    if (it != known.end()) return it->second;

    std::string txt = "";
    uint8_t tag[4];
    write_be32(tag, 0, dt);
    for (int i = 0; i < 3; ++i) {
        if (tag[i] >= 32 && tag[i] < 127) {
            txt += static_cast<char>(tag[i]);
        } else {
            txt += '?';
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s\\%X", txt.c_str(), tag[3]);
    return buf;
}

// RDBBaseBlock
RDBBaseBlock::RDBBaseBlock(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num)
    : blkdev_(blkdev), blk_num_(blk_num), valid_(false), size_longs_(0) {}

void RDBBaseBlock::init_new(uint32_t size_longs) {
    data_.resize(blkdev_->sector_size(), 0);
    const char* magic = get_magic();
    std::memcpy(data_.data(), magic, 4);
    size_longs_ = size_longs;
    put_long(1, size_longs_);
    valid_ = true;
}

bool RDBBaseBlock::read() {
    data_.resize(blkdev_->sector_size());
    try {
        blkdev_->read(blk_num_ * blkdev_->sector_size(), data_);
    } catch (const BlockDeviceError&) {
        return false;
    }
    valid_ = false;
    
    if (std::memcmp(data_.data(), get_magic(), 4) != 0) return false;
    
    size_longs_ = get_long(1);
    if (size_longs_ < 3 || size_longs_ * 4 > data_.size()) return false;
    if (!checksum_ok(data_, size_longs_)) return false;
    
    valid_ = true;
    unpack();
    return true;
}

void RDBBaseBlock::write() {
    pack();
    fix_checksum(data_, size_longs_);
    blkdev_->write(blk_num_ * blkdev_->sector_size(), data_);
}

uint32_t RDBBaseBlock::get_long(uint32_t idx) const {
    return read_be32(data_, idx * 4);
}

int32_t RDBBaseBlock::get_slong(uint32_t idx) const {
    return static_cast<int32_t>(read_be32(data_, idx * 4));
}

void RDBBaseBlock::put_long(uint32_t idx, uint32_t val) {
    write_be32(data_, idx * 4, val);
}

void RDBBaseBlock::put_slong(uint32_t idx, int32_t val) {
    write_be32(data_, idx * 4, static_cast<uint32_t>(val));
}

std::string RDBBaseBlock::get_bytes(uint32_t off, uint32_t size) const {
    return std::string(reinterpret_cast<const char*>(&data_[off]), size);
}

std::string RDBBaseBlock::get_bstr(uint32_t off, uint32_t max_chars) const {
    uint8_t len = std::min<uint8_t>(data_[off], static_cast<uint8_t>(max_chars));
    return std::string(reinterpret_cast<const char*>(&data_[off + 1]), len);
}

void RDBBaseBlock::put_bstr(uint32_t off, uint32_t max_chars, const std::string& val) {
    uint32_t len = std::min<uint32_t>(val.length(), max_chars);
    data_[off] = static_cast<uint8_t>(len);
    std::memcpy(&data_[off + 1], val.data(), len);
    std::memset(&data_[off + 1 + len], 0, max_chars - len);
}

void RDBBaseBlock::put_padded(uint32_t off, uint32_t size, const std::string& text) {
    uint32_t len = std::min<uint32_t>(text.length(), size);
    std::memcpy(&data_[off], text.data(), len);
    std::memset(&data_[off + len], ' ', size - len);
}

// RDBlock
std::unique_ptr<RDBlock> RDBlock::create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) {
    auto b = std::make_unique<RDBlock>(blkdev, blk_num);
    b->init_new(64);
    return b;
}

void RDBlock::unpack() {
    host_id = get_long(3);
    block_bytes = get_long(4);
    flags = get_long(5);
    badblock_list = get_long(6);
    partition_list = get_long(7);
    fs_header_list = get_long(8);
    drive_init = get_long(9);
    
    cylinders = get_long(16);
    sectors = get_long(17);
    heads = get_long(18);
    interleave = get_long(19);
    park = get_long(20);
    write_precomp = get_long(24);
    reduced_write = get_long(25);
    step_rate = get_long(26);

    rdb_blocks_lo = get_long(32);
    rdb_blocks_hi = get_long(33);
    lo_cylinder = get_long(34);
    hi_cylinder = get_long(35);
    cyl_blocks = get_long(36);
    auto_park_seconds = get_long(37);
    high_rdsk_block = get_long(38);

    auto trim_spaces = [](std::string s) {
        s.erase(s.find_last_not_of(" \0") + 1);
        return s;
    };
    disk_vendor = trim_spaces(get_bytes(160, 8));
    disk_product = trim_spaces(get_bytes(168, 16));
    disk_revision = trim_spaces(get_bytes(184, 4));
    ctrl_vendor = trim_spaces(get_bytes(188, 8));
    ctrl_product = trim_spaces(get_bytes(196, 16));
    ctrl_revision = trim_spaces(get_bytes(212, 4));
}

void RDBlock::pack() {
    put_long(3, host_id);
    put_long(4, block_bytes);
    put_long(5, flags);
    put_long(6, badblock_list);
    put_long(7, partition_list);
    put_long(8, fs_header_list);
    put_long(9, drive_init);
    for (int i = 10; i < 16; ++i) put_long(i, 0xFFFFFFFF);
    put_long(16, cylinders);
    put_long(17, sectors);
    put_long(18, heads);
    put_long(19, interleave);
    put_long(20, park);
    put_long(24, write_precomp);
    put_long(25, reduced_write);
    put_long(26, step_rate);
    put_long(32, rdb_blocks_lo);
    put_long(33, rdb_blocks_hi);
    put_long(34, lo_cylinder);
    put_long(35, hi_cylinder);
    put_long(36, cyl_blocks);
    put_long(37, auto_park_seconds);
    put_long(38, high_rdsk_block);
    put_padded(160, 8, disk_vendor);
    put_padded(168, 16, disk_product);
    put_padded(184, 4, disk_revision);
    put_padded(188, 8, ctrl_vendor);
    put_padded(196, 16, ctrl_product);
    put_padded(212, 4, ctrl_revision);
}

// DosEnvec
void DosEnvec::parse(const RDBBaseBlock* blk, uint32_t base_long) {
    table_size = blk->get_long(base_long);
    if (table_size >= 1) size_block = blk->get_long(base_long + 1);
    if (table_size >= 2) sec_org = blk->get_long(base_long + 2);
    if (table_size >= 3) surfaces = blk->get_long(base_long + 3);
    if (table_size >= 4) sec_per_blk = blk->get_long(base_long + 4);
    if (table_size >= 5) blk_per_trk = blk->get_long(base_long + 5);
    if (table_size >= 6) reserved = blk->get_long(base_long + 6);
    if (table_size >= 7) pre_alloc = blk->get_long(base_long + 7);
    if (table_size >= 8) interleave = blk->get_long(base_long + 8);
    if (table_size >= 9) low_cyl = blk->get_long(base_long + 9);
    if (table_size >= 10) high_cyl = blk->get_long(base_long + 10);
    if (table_size >= 11) num_buffer = blk->get_long(base_long + 11);
    if (table_size >= 12) buf_mem_type = blk->get_long(base_long + 12);
    if (table_size >= 13) max_transfer = blk->get_long(base_long + 13);
    if (table_size >= 14) mask = blk->get_long(base_long + 14);
    if (table_size >= 15) boot_pri = blk->get_slong(base_long + 15);
    if (table_size >= 16) dos_type = blk->get_long(base_long + 16);
    if (table_size >= 17) baud = blk->get_long(base_long + 17);
    if (table_size >= 18) control = blk->get_long(base_long + 18);
    if (table_size >= 19) boot_blocks = blk->get_long(base_long + 19);
}

void DosEnvec::pack(RDBBaseBlock* blk, uint32_t base_long) const {
    blk->put_long(base_long, table_size);
    if (table_size >= 1) blk->put_long(base_long + 1, size_block);
    if (table_size >= 2) blk->put_long(base_long + 2, sec_org);
    if (table_size >= 3) blk->put_long(base_long + 3, surfaces);
    if (table_size >= 4) blk->put_long(base_long + 4, sec_per_blk);
    if (table_size >= 5) blk->put_long(base_long + 5, blk_per_trk);
    if (table_size >= 6) blk->put_long(base_long + 6, reserved);
    if (table_size >= 7) blk->put_long(base_long + 7, pre_alloc);
    if (table_size >= 8) blk->put_long(base_long + 8, interleave);
    if (table_size >= 9) blk->put_long(base_long + 9, low_cyl);
    if (table_size >= 10) blk->put_long(base_long + 10, high_cyl);
    if (table_size >= 11) blk->put_long(base_long + 11, num_buffer);
    if (table_size >= 12) blk->put_long(base_long + 12, buf_mem_type);
    if (table_size >= 13) blk->put_long(base_long + 13, max_transfer);
    if (table_size >= 14) blk->put_long(base_long + 14, mask);
    if (table_size >= 15) blk->put_slong(base_long + 15, boot_pri);
    if (table_size >= 16) blk->put_long(base_long + 16, dos_type);
    if (table_size >= 17) blk->put_long(base_long + 17, baud);
    if (table_size >= 18) blk->put_long(base_long + 18, control);
    if (table_size >= 19) blk->put_long(base_long + 19, boot_blocks);
}

// PartitionBlock
std::unique_ptr<PartitionBlock> PartitionBlock::create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) {
    auto b = std::make_unique<PartitionBlock>(blkdev, blk_num);
    b->init_new(64);
    return b;
}

void PartitionBlock::unpack() {
    host_id = get_long(3);
    next = get_long(4);
    flags = get_long(5);
    
    // Read up to 31 chars
    drive_name = get_bstr(36, 31);
    
    // Parse DosEnvec at offset 32 longs
    envec.parse(this, 32);
}

void PartitionBlock::pack() {
    put_long(3, host_id);
    put_long(4, next);
    put_long(5, flags);
    
    // Initialize reserved fields
    for (int i = 6; i < 9; ++i) put_long(i, 0); // reserved
    for (int i = 10; i < 32; ++i) put_long(i, 0); // padding around drive_name
    
    put_bstr(36, 31, drive_name);
    
    envec.pack(this, 32);
}

// FSHeaderBlock
std::unique_ptr<FSHeaderBlock> FSHeaderBlock::create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) {
    auto b = std::make_unique<FSHeaderBlock>(blkdev, blk_num);
    b->init_new(64);
    return b;
}

void FSHeaderBlock::unpack() {
    host_id = get_long(3);
    next = get_long(4);
    flags = get_long(5);
    version = (get_long(8) >> 16) & 0xFFFF;
    revision = get_long(8) & 0xFFFF;
    patch_flags = get_long(9);
    type = get_long(10);
    task = get_long(11);
    lock = get_long(12);
    handler = get_long(13);
    stack_size = get_long(14);
    priority = get_slong(15);
    startup = get_slong(16);
    seg_list_blk = get_slong(17);
    global_vec = get_slong(18);
}

void FSHeaderBlock::pack() {
    put_long(3, host_id);
    put_long(4, next);
    put_long(5, flags);
    
    put_long(6, 0); put_long(7, 0); // reserved
    
    put_long(8, (static_cast<uint32_t>(version) << 16) | revision);
    put_long(9, patch_flags);
    put_long(10, type);
    put_long(11, task);
    put_long(12, lock);
    put_long(13, handler);
    put_long(14, stack_size);
    put_slong(15, priority);
    put_slong(16, startup);
    put_slong(17, seg_list_blk);
    put_slong(18, global_vec);
    
    for (int i = 19; i < 64; ++i) put_long(i, 0);
}

// LoadSegBlock
std::unique_ptr<LoadSegBlock> LoadSegBlock::create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) {
    auto b = std::make_unique<LoadSegBlock>(blkdev, blk_num);
    b->init_new(128); // Standard size is often larger
    return b;
}

void LoadSegBlock::unpack() {
    host_id = get_long(3);
    next = get_long(4);
    
    // load_data starts at longword 5, length is size_longs - 5
    uint32_t len_longs = size_longs_ - 5;
    load_data.resize(len_longs * 4);
    
    // Access data_ directly
    std::memcpy(load_data.data(), &data_[20], len_longs * 4);
}

void LoadSegBlock::pack() {
    put_long(3, host_id);
    put_long(4, next);
    
    uint32_t len_longs = (load_data.size() + 3) / 4;
    size_longs_ = 5 + len_longs;
    data_.resize(size_longs_ * 4, 0);
    put_long(1, size_longs_);
    
    std::memcpy(&data_[20], load_data.data(), load_data.size());
}

// BadBlocksBlock
std::unique_ptr<BadBlocksBlock> BadBlocksBlock::create(std::shared_ptr<BlockDevice> blkdev, uint64_t blk_num) {
    auto b = std::make_unique<BadBlocksBlock>(blkdev, blk_num);
    b->init_new(64);
    return b;
}

void BadBlocksBlock::unpack() {
    host_id = get_long(3);
    next = get_long(4);
}

void BadBlocksBlock::pack() {
    put_long(3, host_id);
    put_long(4, next);
    put_long(5, 0); // reserved
}

} // namespace amidisk
