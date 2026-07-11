#include "sfs.h"
#include "../rdb/blocks.h"
#include "util.h"
#include "../blkdev/blkdev.h"
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <unordered_set>

namespace amidisk {

constexpr uint32_t ROOT_ID = 0x53465300;         // 'SFS\0'
constexpr uint32_t OBJC_ID = 0x4F424A43;         // 'OBJC'
constexpr uint32_t BNDC_ID = 0x424E4443;         // 'BNDC'

constexpr uint8_t OTYPE_DIR = 128;
constexpr uint8_t OTYPE_LINK = 64;
constexpr uint8_t OTYPE_HARDLINK = 32;

constexpr int MAX_CHAIN = 1000000;



static uint16_t sfs_hash(const std::string& name, bool case_sensitive) {
    std::string base = name;
    size_t pos = base.find('/');
    if (pos != std::string::npos) base = base.substr(0, pos);
    
    uint32_t h = base.length();
    for (char c : base) {
        uint8_t uc = static_cast<uint8_t>(c);
        if (!case_sensitive) {
            if ((uc >= 0x61 && uc <= 0x7A) || (uc >= 0xE0 && uc <= 0xFE && uc != 0xF7)) {
                uc -= 32;
            }
        }
        h = (h * 13 + uc) & 0xFFFF;
    }
    return h;
}

static std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.length()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.length();
        if (end > start) parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::chrono::system_clock::time_point SFSEntry::mtime() const {
    auto epoch = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(252460800) // Jan 1 1978
        )
    );
    return epoch + std::chrono::seconds(secs);
}

std::string SFSEntry::type_str() const {
    if (is_dir()) return "dir";
    if (is_link()) return is_hardlink() ? "hardlink" : "softlink";
    return "file";
}

std::string SFSEntry::protect_str() const {
    return amidisk::protect_to_str(protect ^ 0x0F);
}

SFSVolume::SFSVolume(std::shared_ptr<BlockDevice> blkdev, uint32_t dos_type)
    : blkdev_(blkdev), dos_type_(dos_type), read_only_(true) {
    bs_ = blkdev->sector_size();
}

std::vector<uint8_t> SFSVolume::read_block(uint32_t blocknr, uint32_t count) {
    if (total_ > 0 && blocknr + count > total_) throw SFSError("SFS block out of volume");
    std::vector<uint8_t> data(count * spb_ * blkdev_->sector_size());
    blkdev_->read((uint64_t)blocknr * spb_ * blkdev_->sector_size(), data);
    return data;
}

bool SFSVolume::checksum_ok(const std::vector<uint8_t>& raw) {
    uint32_t s = 0;
    for (size_t i = 0; i < raw.size(); i += 4) {
        s += (raw[i] << 24) | (raw[i+1] << 16) | (raw[i+2] << 8) | raw[i+3];
    }
    return s == 0xFFFFFFFF;
}

std::vector<uint8_t> SFSVolume::read_checked(uint32_t blocknr, uint32_t want_id) {
    auto raw = read_block(blocknr);
    uint32_t bid = (raw[0] << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
    if (bid != want_id) throw SFSError("SFS block ID mismatch");
    if (!checksum_ok(raw)) throw SFSError("SFS block checksum invalid");
    uint32_t own = (raw[8] << 24) | (raw[9] << 16) | (raw[10] << 8) | raw[11];
    if (own != blocknr) throw SFSError("SFS block ownblock mismatch");
    return raw;
}

void SFSVolume::open() {
    std::vector<uint8_t> raw(blkdev_->sector_size());
    blkdev_->read(0, raw);
    uint32_t bid = (raw[0] << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
    if (bid != ROOT_ID) throw SFSError("no SFS root block");
    
    uint16_t version = (raw[12] << 8) | raw[13];
    if (version != 3) throw SFSError("unsupported SFS structure version");
    
    datecreated_ = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
    root_bits_ = raw[20];
    
    totalblocks_ = (raw[48] << 24) | (raw[49] << 16) | (raw[50] << 8) | raw[51];
    blocksize_ = (raw[52] << 24) | (raw[53] << 16) | (raw[54] << 8) | raw[55];
    
    bitmapbase_ = (raw[96] << 24) | (raw[97] << 16) | (raw[98] << 8) | raw[99];
    adminspacecontainer_ = (raw[100] << 24) | (raw[101] << 16) | (raw[102] << 8) | raw[103];
    rootobjectcontainer_ = (raw[104] << 24) | (raw[105] << 16) | (raw[106] << 8) | raw[107];
    extentbnoderoot_ = (raw[108] << 24) | (raw[109] << 16) | (raw[110] << 8) | raw[111];
    objectnoderoot_ = (raw[112] << 24) | (raw[113] << 16) | (raw[114] << 8) | raw[115];
    
    if (blocksize_ % blkdev_->sector_size() != 0) throw SFSError("SFS blocksize not multiple of sector");
    spb_ = blocksize_ / blkdev_->sector_size();
    bs_ = blocksize_;
    total_ = totalblocks_;
    if (total_ * spb_ > blkdev_->num_blocks()) {
        total_ = blkdev_->num_blocks() / spb_;
    }
    
    raw = read_checked(0, ROOT_ID);
    case_sensitive_ = (root_bits_ & 128) != 0;
    
    auto cont = read_checked(rootobjectcontainer_, OBJC_ID);
    root_obj_ = parse_object(cont, 24, rootobjectcontainer_);
    label_ = root_obj_.name_str();
    
    uint32_t free_off = bs_ - 36 + 8;
    freeblocks_ = (cont[free_off] << 24) | (cont[free_off+1] << 16) | (cont[free_off+2] << 8) | cont[free_off+3];
}

std::string SFSVolume::dos_type_str() const {
    return dos_type_to_str(dos_type_ ? dos_type_ : 0x53465300);
}


void SFSVolume::require_writable() const {
    if (read_only_) throw SFSError("SFS volume is read-only");
}

void SFSVolume::write_block(uint32_t blocknr, std::vector<uint8_t>& raw) {
    if (raw.size() != bs_) throw SFSError("Invalid block size in write_block");
    if (blocknr >= total_) throw SFSError("Write block out of bounds");
    
    // Check if the first 4 bytes indicate a known metadata block type (id_val is not 0)
    uint32_t id_val = (raw[0] << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
    if (id_val != 0) {
        // Set checksum to 0
        raw[4] = 0; raw[5] = 0; raw[6] = 0; raw[7] = 0;
        uint32_t c = 0;
        for (size_t i = 0; i < raw.size() / 4; ++i) {
            uint32_t v = (raw[i*4] << 24) | (raw[i*4+1] << 16) | (raw[i*4+2] << 8) | raw[i*4+3];
            c += v;
        }
        uint32_t chk = 0xFFFFFFFF - c;
        raw[4] = (chk >> 24) & 0xFF;
        raw[5] = (chk >> 16) & 0xFF;
        raw[6] = (chk >> 8) & 0xFF;
        raw[7] = chk & 0xFF;
    }
    blkdev_->write(blocknr * spb_ * blkdev_->sector_size(), raw);
}

void SFSVolume::fix_checksum(std::vector<uint8_t>& raw, uint32_t id_val, uint32_t blocknr) {
    if (raw.size() < 12) throw SFSError("Block too small for checksum");
    raw[0] = (id_val >> 24) & 0xFF;
    raw[1] = (id_val >> 16) & 0xFF;
    raw[2] = (id_val >> 8) & 0xFF;
    raw[3] = id_val & 0xFF;
    
    raw[4] = 0; raw[5] = 0; raw[6] = 0; raw[7] = 0;
    
    raw[8] = (blocknr >> 24) & 0xFF;
    raw[9] = (blocknr >> 16) & 0xFF;
    raw[10] = (blocknr >> 8) & 0xFF;
    raw[11] = blocknr & 0xFF;
    
    uint32_t s = 0;
    for (size_t i = 0; i < raw.size() / 4; ++i) {
        uint32_t v = (raw[i*4] << 24) | (raw[i*4+1] << 16) | (raw[i*4+2] << 8) | raw[i*4+3];
        s += v;
    }
    uint32_t chk = 0xFFFFFFFF - s;
    raw[4] = (chk >> 24) & 0xFF;
    raw[5] = (chk >> 16) & 0xFF;
    raw[6] = (chk >> 8) & 0xFF;
    raw[7] = chk & 0xFF;
}

void SFSVolume::touch_volume() {
    uint32_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count() - 252460800;
    
    auto raw = read_block(0);
    raw[16] = (timestamp >> 24) & 0xFF;
    raw[17] = (timestamp >> 16) & 0xFF;
    raw[18] = (timestamp >> 8) & 0xFF;
    raw[19] = timestamp & 0xFF;
    write_block(0, raw);
    
    auto raw2 = read_block(totalblocks_ - 1);
    raw2[16] = (timestamp >> 24) & 0xFF;
    raw2[17] = (timestamp >> 16) & 0xFF;
    raw2[18] = (timestamp >> 8) & 0xFF;
    raw2[19] = timestamp & 0xFF;
    write_block(totalblocks_ - 1, raw2);
}

void SFSVolume::mark_space(uint32_t start, uint32_t count, bool free) {
    uint32_t bits_per_block = (bs_ - 12) * 8;
    
    uint32_t blk = start;
    uint32_t remaining = count;
    
    while (remaining > 0) {
        uint32_t bitmap_idx = blk / bits_per_block;
        uint32_t bit_offset = blk % bits_per_block;
        
        uint32_t bblock = bitmapbase_ + bitmap_idx;
        auto raw = read_block(bblock);
        
        while (remaining > 0 && bit_offset < bits_per_block) {
            if (bit_offset % 8 == 0 && remaining >= 8) {
                uint32_t nbytes = std::min<uint32_t>(remaining, bits_per_block - bit_offset) / 8;
                if (nbytes > 0) {
                    uint32_t o = 12 + bit_offset / 8;
                    for (uint32_t i = 0; i < nbytes; ++i) {
                        raw[o + i] = free ? 0xFF : 0x00;
                    }
                    blk += nbytes * 8;
                    remaining -= nbytes * 8;
                    bit_offset += nbytes * 8;
                    continue;
                }
            }
            
            uint32_t word_idx = bit_offset / 32;
            uint32_t bit_in_word = bit_offset % 32;
            
            uint32_t offset = 12 + word_idx * 4;
            uint32_t w = (raw[offset] << 24) | (raw[offset+1] << 16) | (raw[offset+2] << 8) | raw[offset+3];
            
            if (free) {
                w |= (1 << (31 - bit_in_word));
            } else {
                w &= ~(1 << (31 - bit_in_word));
            }
            
            raw[offset] = (w >> 24) & 0xFF;
            raw[offset+1] = (w >> 16) & 0xFF;
            raw[offset+2] = (w >> 8) & 0xFF;
            raw[offset+3] = w & 0xFF;
            
            blk++;
            remaining--;
            bit_offset++;
        }
        write_block(bblock, raw);
    }
}

std::vector<std::pair<uint32_t, uint32_t>> SFSVolume::alloc_contiguous_blocks(uint32_t count) {
    if (count > freeblocks_) throw SFSError("disk full");
    
    uint32_t bits_per_block = (bs_ - 12) * 8;
    uint32_t words_per_block = (bs_ - 12) / 4;
    uint32_t blocks_bitmap = (total_ + bits_per_block - 1) / bits_per_block;
    
    int64_t cur_start = -1;
    uint32_t cur_count = 0;
    
    for (uint32_t bitmap_idx = 0; bitmap_idx < blocks_bitmap; ++bitmap_idx) {
        auto raw = read_block(bitmapbase_ + bitmap_idx);
        for (uint32_t word_idx = 0; word_idx < words_per_block; ++word_idx) {
            uint32_t offset = 12 + word_idx * 4;
            uint32_t w = (raw[offset] << 24) | (raw[offset+1] << 16) | (raw[offset+2] << 8) | raw[offset+3];
            if (w == 0) {
                cur_count = 0;
                cur_start = -1;
                continue;
            }
            
            for (int bit_in_word = 0; bit_in_word < 32; ++bit_in_word) {
                uint32_t blk = bitmap_idx * bits_per_block + word_idx * 32 + bit_in_word;
                if (blk >= total_) break;
                
                bool is_free = (w & (1 << (31 - bit_in_word))) != 0;
                if (is_free) {
                    if (cur_count == 0) cur_start = blk;
                    cur_count++;
                    if (cur_count == count) {
                        mark_space(cur_start, count, false);
                        freeblocks_ -= count;
                        return {{cur_start, count}};
                    }
                } else {
                    cur_count = 0;
                    cur_start = -1;
                }
            }
        }
    }
    throw SFSError("disk full or heavily fragmented");
}

std::vector<std::pair<uint32_t, uint32_t>> SFSVolume::alloc_blocks(uint32_t count) {
    if (count > freeblocks_) throw SFSError("disk full");
    
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    uint32_t needed = count;
    uint32_t bits_per_block = (bs_ - 12) * 8;
    uint32_t words_per_block = (bs_ - 12) / 4;
    uint32_t blocks_bitmap = (total_ + bits_per_block - 1) / bits_per_block;
    
    int64_t cur_start = -1;
    uint32_t cur_count = 0;
    
    for (uint32_t bitmap_idx = 0; bitmap_idx < blocks_bitmap; ++bitmap_idx) {
        if (needed == 0) break;
        if (bitmap_idx == 0 && cur_count > 0) {
            chunks.push_back({cur_start, cur_count});
            needed -= cur_count;
            if (needed == 0) break;
            cur_count = 0;
            cur_start = -1;
        }
        auto raw = read_block(bitmapbase_ + bitmap_idx);
        for (uint32_t word_idx = 0; word_idx < words_per_block; ++word_idx) {
            if (needed == 0) break;
            uint32_t offset = 12 + word_idx * 4;
            uint32_t w = (raw[offset] << 24) | (raw[offset+1] << 16) | (raw[offset+2] << 8) | raw[offset+3];
            
            for (int bit_in_word = 0; bit_in_word < 32; ++bit_in_word) {
                uint32_t blk = bitmap_idx * bits_per_block + word_idx * 32 + bit_in_word;
                if (blk >= total_) break;
                
                bool is_free = (w & (1 << (31 - bit_in_word))) != 0;
                if (is_free) {
                    if (cur_count == 0) cur_start = blk;
                    cur_count++;
                    if (cur_count == needed) {
                        chunks.push_back({cur_start, cur_count});
                        needed -= cur_count;
                        break;
                    }
                } else {
                    if (cur_count > 0) {
                        chunks.push_back({cur_start, cur_count});
                        needed -= cur_count;
                        cur_count = 0;
                        cur_start = -1;
                        if (needed == 0) break;
                    }
                }
            }
        }
    }
    
    if (cur_count > 0 && needed > 0) {
        chunks.push_back({cur_start, cur_count});
        needed -= cur_count;
    }
    
    if (needed > 0) throw SFSError("allocation failed internally");
    
    for (const auto& chunk : chunks) {
        mark_space(chunk.first, chunk.second, false);
    }
    freeblocks_ -= count;
    return chunks;
}

uint32_t SFSVolume::alloc_adminspace() {
    uint32_t admin_block = adminspacecontainer_;
    uint32_t adminspaces = (bs_ - 24) / 8;
    
    while (admin_block != 0) {
        auto raw = read_block(admin_block);
        for (uint32_t i = 0; i < adminspaces; ++i) {
            uint32_t offset = 24 + i * 8;
            uint32_t space = (raw[offset] << 24) | (raw[offset+1] << 16) | (raw[offset+2] << 8) | raw[offset+3];
            uint32_t bits = (raw[offset+4] << 24) | (raw[offset+5] << 16) | (raw[offset+6] << 8) | raw[offset+7];
            
            if (space != 0 && bits != 0xFFFFFFFF) {
                for (int bit_idx = 0; bit_idx < 32; ++bit_idx) {
                    if ((bits & (1 << (31 - bit_idx))) == 0) {
                        bits |= (1 << (31 - bit_idx));
                        raw[offset+4] = (bits >> 24) & 0xFF;
                        raw[offset+5] = (bits >> 16) & 0xFF;
                        raw[offset+6] = (bits >> 8) & 0xFF;
                        raw[offset+7] = bits & 0xFF;
                        write_block(admin_block, raw);
                        
                        uint32_t new_blk = space + bit_idx;
                        std::vector<uint8_t> empty(bs_, 0);
                        write_block(new_blk, empty);
                        return new_blk;
                    }
                }
            }
        }
        admin_block = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
    }
    
    auto chunks = alloc_contiguous_blocks(32);
    uint32_t startblock = chunks[0].first;
    
    admin_block = adminspacecontainer_;
    while (admin_block != 0) {
        auto raw = read_block(admin_block);
        for (uint32_t i = 0; i < adminspaces; ++i) {
            uint32_t offset = 24 + i * 8;
            uint32_t space = (raw[offset] << 24) | (raw[offset+1] << 16) | (raw[offset+2] << 8) | raw[offset+3];
            if (space == 0) {
                raw[offset] = (startblock >> 24) & 0xFF;
                raw[offset+1] = (startblock >> 16) & 0xFF;
                raw[offset+2] = (startblock >> 8) & 0xFF;
                raw[offset+3] = startblock & 0xFF;
                raw[offset+4] = 0; raw[offset+5] = 0; raw[offset+6] = 0; raw[offset+7] = 0;
                write_block(admin_block, raw);
                return alloc_adminspace();
            }
        }
        uint32_t nxt = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
        if (nxt == 0) {
            raw[16] = (startblock >> 24) & 0xFF;
            raw[17] = (startblock >> 16) & 0xFF;
            raw[18] = (startblock >> 8) & 0xFF;
            raw[19] = startblock & 0xFF;
            write_block(admin_block, raw);
            
            std::vector<uint8_t> new_container(bs_, 0);
            fix_checksum(new_container, 0x41444D43, startblock); // ADMC
            new_container[12] = (admin_block >> 24) & 0xFF;
            new_container[13] = (admin_block >> 16) & 0xFF;
            new_container[14] = (admin_block >> 8) & 0xFF;
            new_container[15] = admin_block & 0xFF;
            new_container[20] = 32;
            
            uint32_t offset = 24;
            new_container[offset] = (startblock >> 24) & 0xFF;
            new_container[offset+1] = (startblock >> 16) & 0xFF;
            new_container[offset+2] = (startblock >> 8) & 0xFF;
            new_container[offset+3] = startblock & 0xFF;
            new_container[offset+4] = 0x80;
            write_block(startblock, new_container);
            
            return alloc_adminspace();
        }
        admin_block = nxt;
    }
    throw SFSError("could not allocate admin space");
}

void SFSVolume::free_adminspace(uint32_t block) {
    uint32_t admin_block = adminspacecontainer_;
    uint32_t adminspaces = (bs_ - 24) / 8;
    int guard = 0;
    while (admin_block != 0) {
        if (++guard > MAX_CHAIN) throw SFSError("cyclic admin space container chain");
        auto raw = read_block(admin_block);
        for (uint32_t i = 0; i < adminspaces; ++i) {
            uint32_t space = (raw[24 + i * 8] << 24) | (raw[24 + i * 8 + 1] << 16) | (raw[24 + i * 8 + 2] << 8) | raw[24 + i * 8 + 3];
            uint32_t bits = (raw[24 + i * 8 + 4] << 24) | (raw[24 + i * 8 + 5] << 16) | (raw[24 + i * 8 + 6] << 8) | raw[24 + i * 8 + 7];
            
            if (space != 0 && block >= space && block < space + 32) {
                bits &= ~(1U << (31 - (block - space)));
                raw[24 + i * 8 + 4] = (bits >> 24) & 0xFF;
                raw[24 + i * 8 + 5] = (bits >> 16) & 0xFF;
                raw[24 + i * 8 + 6] = (bits >> 8) & 0xFF;
                raw[24 + i * 8 + 7] = bits & 0xFF;
                write_block(admin_block, raw);
                return;
            }
        }
        admin_block = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
    }
}

uint32_t SFSVolume::alloc_objectnode() {
    uint32_t nodesize = 10;
    uint32_t nodecount_leaf = (bs_ - 20) / nodesize;
    
    std::function<uint32_t(uint32_t)> search_container = [&](uint32_t blk) -> uint32_t {
        auto raw = read_block(blk);
        uint32_t nodenumber = (raw[12] << 24) | (raw[13] << 16) | (raw[14] << 8) | raw[15];
        uint32_t nodes = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
        
        if (nodes == 1) { // leaf container
            uint32_t base = 20;
            for (uint32_t n = 0; n < nodecount_leaf; ++n) {
                uint32_t data = (raw[base + n * nodesize] << 24) | 
                                (raw[base + n * nodesize + 1] << 16) | 
                                (raw[base + n * nodesize + 2] << 8) | 
                                raw[base + n * nodesize + 3];
                if (data == 0) {
                    uint32_t nodeno = nodenumber + n;
                    if (nodeno > 0) return nodeno;
                }
            }
        } else {
            uint32_t node_containers = (bs_ - 20) / 4;
            uint32_t base = 20;
            for (uint32_t n = 0; n < node_containers; ++n) {
                uint32_t child_ptr = (raw[base + n * 4] << 24) | 
                                     (raw[base + n * 4 + 1] << 16) | 
                                     (raw[base + n * 4 + 2] << 8) | 
                                     raw[base + n * 4 + 3];
                if (child_ptr == 0) break;
                bool is_full = (child_ptr & 1) != 0;
                uint32_t child_blk = child_ptr & ~1;
                
                if (!is_full) {
                    uint32_t res = search_container(child_blk);
                    if (res != 0) return res;
                }
            }
        }
        return 0;
    };
    
    uint32_t nodeno = search_container(objectnoderoot_);
    if (nodeno != 0) return nodeno;
    throw SFSError("objectnode tree full, expansion not yet fully ported");
}

void SFSVolume::free_objectnode(uint32_t nodeno) {
    auto [path, blk, raw, nodenumber] = node_path(nodeno);
    uint32_t off = 20 + (nodeno - nodenumber) * 10;
    
    raw[off] = 0; raw[off+1] = 0; raw[off+2] = 0; raw[off+3] = 0;
    write_block(blk, raw);
    
    update_full_bits(path, false);
}

std::pair<SFSEntry, std::string> SFSVolume::split_parent(const std::string& path) {
    auto parts = split_path(path);
    if (parts.empty()) throw SFSError("empty path");
    std::string key;
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        if (i > 0) key += "/";
        key += parts[i];
    }
    auto parent = resolve(key);
    return {parent, parts.back()};
}

std::vector<uint8_t> SFSVolume::create_fsobject(const std::string& name, uint32_t size, uint32_t data, uint32_t objectnode, bool is_dir, const std::string& comment) {
    uint8_t bits = is_dir ? 0x80 : 0x00;
    
    std::string name_bytes = name;
    std::string comment_bytes = comment;
    
    uint32_t total_len = 25 + name_bytes.length() + 1 + comment_bytes.length() + 1;
    if (total_len % 2 != 0) total_len += 1; // Pad to even
    
    std::vector<uint8_t> raw(total_len, 0);
    uint32_t protection = 0x0000000F; // R, W, E, D
    uint32_t datemodified = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count() - 252460800;
    
    raw[4] = (objectnode >> 24) & 0xFF; raw[5] = (objectnode >> 16) & 0xFF; raw[6] = (objectnode >> 8) & 0xFF; raw[7] = objectnode & 0xFF;
    raw[8] = (protection >> 24) & 0xFF; raw[9] = (protection >> 16) & 0xFF; raw[10] = (protection >> 8) & 0xFF; raw[11] = protection & 0xFF;
    raw[12] = (data >> 24) & 0xFF; raw[13] = (data >> 16) & 0xFF; raw[14] = (data >> 8) & 0xFF; raw[15] = data & 0xFF;
    raw[16] = (size >> 24) & 0xFF; raw[17] = (size >> 16) & 0xFF; raw[18] = (size >> 8) & 0xFF; raw[19] = size & 0xFF;
    raw[20] = (datemodified >> 24) & 0xFF; raw[21] = (datemodified >> 16) & 0xFF; raw[22] = (datemodified >> 8) & 0xFF; raw[23] = datemodified & 0xFF;
    raw[24] = bits;
    
    uint32_t p = 25;
    for (char c : name_bytes) raw[p++] = c;
    raw[p++] = 0;
    for (char c : comment_bytes) raw[p++] = c;
    raw[p++] = 0;
    
    return raw;
}

void SFSVolume::update_object_firstdirblock(const SFSEntry& entry) {
    if (entry.objc_block == 0) return;
    auto raw = read_block(entry.objc_block);
    uint32_t offset = entry.objc_offset + 16;
    raw[offset] = (entry.firstdirblock >> 24) & 0xFF;
    raw[offset+1] = (entry.firstdirblock >> 16) & 0xFF;
    raw[offset+2] = (entry.firstdirblock >> 8) & 0xFF;
    raw[offset+3] = entry.firstdirblock & 0xFF;
    
    raw[4] = 0; raw[5] = 0; raw[6] = 0; raw[7] = 0;
    uint32_t c = 0;
    for (size_t i = 0; i < raw.size() / 4; ++i) {
        c += (raw[i*4] << 24) | (raw[i*4+1] << 16) | (raw[i*4+2] << 8) | raw[i*4+3];
    }
    uint32_t chk = 0xFFFFFFFF - c;
    raw[4] = (chk >> 24) & 0xFF;
    raw[5] = (chk >> 16) & 0xFF;
    raw[6] = (chk >> 8) & 0xFF;
    raw[7] = chk & 0xFF;
    write_block(entry.objc_block, raw);
}

void SFSVolume::insert_object(const SFSEntry& parent_entry, const std::vector<uint8_t>& obj_data) {
    uint32_t block = parent_entry.firstdirblock;
    
    if (block == 0) {
        block = alloc_adminspace();
        std::vector<uint8_t> raw(bs_, 0);
        fix_checksum(raw, 0x4F424A43, block); // OBJC
        raw[12] = (parent_entry.objectnode >> 24) & 0xFF;
        raw[13] = (parent_entry.objectnode >> 16) & 0xFF;
        raw[14] = (parent_entry.objectnode >> 8) & 0xFF;
        raw[15] = parent_entry.objectnode & 0xFF;
        
        for (size_t i = 0; i < obj_data.size(); ++i) {
            raw[24 + i] = obj_data[i];
        }
        write_block(block, raw);
        
        SFSEntry mut_parent = parent_entry;
        mut_parent.firstdirblock = block;
        if (mut_parent.objc_block != 0) {
            update_object_firstdirblock(mut_parent);
        }
        return;
    }
    
    int guard = 0;
    while (block != 0) {
        if (++guard > MAX_CHAIN) throw SFSError("cyclic SFS dir container chain");
        auto raw = read_block(block);
        uint32_t base = 24;
        while (base + 25 < bs_) {
            uint32_t p = base + 25;
            while (p < bs_ && raw[p] != 0) p++;
            int32_t name_len = p - (base + 25);
            
            if (name_len < 0 || raw[base + 25] == 0) {
                // Empty slot
                if (base + obj_data.size() + 2 <= bs_) {
                    for (size_t i = 0; i < obj_data.size(); ++i) {
                        raw[base + i] = obj_data[i];
                    }
                    if (base + obj_data.size() + 25 < bs_) {
                        raw[base + obj_data.size() + 25] = 0;
                    }
                    fix_checksum(raw, 0x4F424A43, block);
                    write_block(block, raw);
                    return;
                } else {
                    break;
                }
            }
            
            uint32_t comment_start = base + 25 + name_len + 1;
            uint32_t cp = comment_start;
            while (cp < bs_ && raw[cp] != 0) cp++;
            uint32_t comment_len = cp - comment_start;
            
            uint32_t total_len = 25 + name_len + 1 + comment_len + 1;
            if (total_len % 2 != 0) total_len += 1;
            base += total_len;
        }
        
        uint32_t nxt = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
        if (nxt == 0) {
            uint32_t new_blk = alloc_adminspace();
            std::vector<uint8_t> new_raw(bs_, 0);
            fix_checksum(new_raw, 0x4F424A43, new_blk); // OBJC
            new_raw[12] = raw[12]; new_raw[13] = raw[13]; new_raw[14] = raw[14]; new_raw[15] = raw[15]; // parent
            new_raw[20] = (block >> 24) & 0xFF; new_raw[21] = (block >> 16) & 0xFF; new_raw[22] = (block >> 8) & 0xFF; new_raw[23] = block & 0xFF; // prev
            
            for (size_t i = 0; i < obj_data.size(); ++i) {
                new_raw[24 + i] = obj_data[i];
            }
            write_block(new_blk, new_raw);
            
            raw[16] = (new_blk >> 24) & 0xFF; raw[17] = (new_blk >> 16) & 0xFF; raw[18] = (new_blk >> 8) & 0xFF; raw[19] = new_blk & 0xFF;
            fix_checksum(raw, 0x4F424A43, block);
            write_block(block, raw);
            return;
        }
        block = nxt;
    }
}


struct BNodePath {
    uint32_t blk;
    std::vector<uint8_t> raw;
    uint8_t isleaf;
    uint8_t nodesize;
    uint16_t nodecount;
};

void SFSVolume::format(const std::string& name, uint32_t dos_type) {
    if (name.length() > 30) throw SFSError("Name too long");
    std::string label = name;
    
    dos_type_ = dos_type ? dos_type : 0x53465300;
    label_ = label;
    
    uint32_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count() - 252460800;
    
    uint32_t blocks_admin = 32;
    uint32_t blocks_reserved_start = 2;
    uint32_t blocks_reserved_end = 1;
    uint32_t blocks_total = blkdev_->num_blocks() / spb_;
    total_ = blocks_total;
    
    uint32_t bits_per_block = (bs_ - 12) * 8;
    uint32_t blocks_bitmap = (blocks_total + bits_per_block - 1) / bits_per_block;
    
    uint32_t block_adminspace = blocks_reserved_start;
    uint32_t block_root = blocks_reserved_start + 1;
    uint32_t block_extentbnoderoot = block_root + 3;
    uint32_t block_bitmapbase = block_adminspace + blocks_admin;
    uint32_t block_objectnoderoot = block_root + 4;
    uint32_t block_recycled = block_root + 5;
    
    auto write_b = [&](uint32_t blk, std::vector<uint8_t>& r) {
        blkdev_->write(blk * spb_ * blkdev_->sector_size(), r);
    };
    
    // 1. AdminSpaceContainer
    std::vector<uint8_t> raw_admc(bs_, 0);
    raw_admc[20] = 2; // bits=2
    raw_admc[24] = (block_adminspace >> 24) & 0xFF; raw_admc[25] = (block_adminspace >> 16) & 0xFF; raw_admc[26] = (block_adminspace >> 8) & 0xFF; raw_admc[27] = block_adminspace & 0xFF;
    raw_admc[28] = 0xFE; raw_admc[29] = 0; raw_admc[30] = 0; raw_admc[31] = 0;
    fix_checksum(raw_admc, 0x41444D43, block_adminspace); // ADMC
    write_b(block_adminspace, raw_admc);
    
    // 2. Root ObjectContainer
    std::vector<uint8_t> raw_objc(bs_, 0);
    raw_objc[24] = 0; raw_objc[25] = 0; raw_objc[26] = 0; raw_objc[27] = 0; // uid, gid
    raw_objc[28] = 0; raw_objc[29] = 0; raw_objc[30] = 0; raw_objc[31] = 1; // ROOTNODE=1
    raw_objc[32] = 0; raw_objc[33] = 0; raw_objc[34] = 0; raw_objc[35] = 15; // protect
    
    raw_objc[36] = ((block_root+1) >> 24) & 0xFF; raw_objc[37] = ((block_root+1) >> 16) & 0xFF; raw_objc[38] = ((block_root+1) >> 8) & 0xFF; raw_objc[39] = (block_root+1) & 0xFF;
    raw_objc[40] = (block_recycled >> 24) & 0xFF; raw_objc[41] = (block_recycled >> 16) & 0xFF; raw_objc[42] = (block_recycled >> 8) & 0xFF; raw_objc[43] = block_recycled & 0xFF;
    raw_objc[44] = (timestamp >> 24) & 0xFF; raw_objc[45] = (timestamp >> 16) & 0xFF; raw_objc[46] = (timestamp >> 8) & 0xFF; raw_objc[47] = timestamp & 0xFF;
    raw_objc[48] = 0x80; // OTYPE_DIR
    for (size_t i = 0; i < label.length(); ++i) raw_objc[49+i] = label[i];
    
    uint32_t freeblks = blocks_total - blocks_admin - blocks_reserved_start - blocks_reserved_end - blocks_bitmap;
    uint32_t o = bs_ - 36;
    raw_objc[o+8] = (freeblks >> 24) & 0xFF; raw_objc[o+9] = (freeblks >> 16) & 0xFF; raw_objc[o+10] = (freeblks >> 8) & 0xFF; raw_objc[o+11] = freeblks & 0xFF;
    raw_objc[o+12] = (timestamp >> 24) & 0xFF; raw_objc[o+13] = (timestamp >> 16) & 0xFF; raw_objc[o+14] = (timestamp >> 8) & 0xFF; raw_objc[o+15] = timestamp & 0xFF;
    fix_checksum(raw_objc, 0x4F424A43, block_root); // OBJC
    write_b(block_root, raw_objc);
    
    // 3. Root HashTable
    std::vector<uint8_t> raw_htab(bs_, 0);
    raw_htab[12] = 0; raw_htab[13] = 0; raw_htab[14] = 0; raw_htab[15] = 1; // ROOTNODE
    
    uint16_t h = sfs_hash(".recycled", case_sensitive_);
    uint32_t h_idx = h % ((bs_ - 16) / 4);
    raw_htab[16 + h_idx * 4 + 3] = 2; // RECYCLEDNODE=2
    fix_checksum(raw_htab, 0x48544142, block_root + 1); // HTAB
    write_b(block_root + 1, raw_htab);
    
    // 4. Transaction Block
    std::vector<uint8_t> raw_trok(bs_, 0);
    fix_checksum(raw_trok, 0x54524F4B, block_root + 2); // TROK
    write_b(block_root + 2, raw_trok);
    
    // 5. ExtentNode B-Tree root
    std::vector<uint8_t> raw_bndc(bs_, 0);
    raw_bndc[12] = 0; raw_bndc[13] = 0; raw_bndc[14] = 1; raw_bndc[15] = 14;
    fix_checksum(raw_bndc, 0x424E4443, block_extentbnoderoot);
    write_b(block_extentbnoderoot, raw_bndc);
    
    // 6. ObjectNode root
    std::vector<uint8_t> raw_nodc(bs_, 0);
    raw_nodc[12] = 0; raw_nodc[13] = 0; raw_nodc[14] = 0; raw_nodc[15] = 1; // nodenumber
    raw_nodc[16] = 0; raw_nodc[17] = 0; raw_nodc[18] = 0; raw_nodc[19] = 1; // leaf
    
    raw_nodc[20] = (block_root >> 24) & 0xFF; raw_nodc[21] = (block_root >> 16) & 0xFF; raw_nodc[22] = (block_root >> 8) & 0xFF; raw_nodc[23] = block_root & 0xFF;
    raw_nodc[30] = (block_recycled >> 24) & 0xFF; raw_nodc[31] = (block_recycled >> 16) & 0xFF; raw_nodc[32] = (block_recycled >> 8) & 0xFF; raw_nodc[33] = block_recycled & 0xFF;
    raw_nodc[38] = (h >> 8) & 0xFF; raw_nodc[39] = h & 0xFF;
    fix_checksum(raw_nodc, 0x4E444320, block_objectnoderoot); // NODC
    write_b(block_objectnoderoot, raw_nodc);
    
    // 7. Recycled ObjectContainer
    std::vector<uint8_t> raw_recy(bs_, 0);
    raw_recy[12] = 0; raw_recy[13] = 0; raw_recy[14] = 0; raw_recy[15] = 1; // ROOTNODE
    raw_recy[28] = 0; raw_recy[29] = 0; raw_recy[30] = 0; raw_recy[31] = 2; // RECYCLEDNODE
    raw_recy[32] = 0; raw_recy[33] = 0; raw_recy[34] = 0; raw_recy[35] = 3; // protect
    raw_recy[44] = (timestamp >> 24) & 0xFF; raw_recy[45] = (timestamp >> 16) & 0xFF; raw_recy[46] = (timestamp >> 8) & 0xFF; raw_recy[47] = timestamp & 0xFF;
    raw_recy[48] = 0x80 | 2 | 4; // dir | undeletable | quickdir
    std::string recy_name = ".recycled";
    for (size_t i = 0; i < recy_name.length(); ++i) raw_recy[49+i] = recy_name[i];
    fix_checksum(raw_recy, 0x4F424A43, block_recycled);
    write_b(block_recycled, raw_recy);
    
    // 8. Bitmap
    int64_t startfree = blocks_admin + blocks_bitmap + blocks_reserved_start;
    int64_t sizefree = blocks_total - startfree - blocks_reserved_end;
    
    uint32_t block = block_bitmapbase;
    for (uint32_t i = 0; i < blocks_bitmap; ++i) {
        std::vector<uint8_t> raw_btmp(bs_, 0);
        for (uint32_t cnt2 = 0; cnt2 < (bs_ - 12) / 4; ++cnt2) {
            uint32_t val = 0;
            if (startfree > 0) {
                startfree -= 32;
                if (startfree < 0) {
                    val = (1U << (-startfree)) - 1;
                    sizefree += startfree;
                }
            } else if (sizefree > 0) {
                sizefree -= 32;
                if (sizefree < 0) {
                    val = ~((1U << (-sizefree)) - 1);
                } else {
                    val = 0xFFFFFFFF;
                }
            } else {
                break;
            }
            raw_btmp[12 + cnt2 * 4] = (val >> 24) & 0xFF;
            raw_btmp[12 + cnt2 * 4 + 1] = (val >> 16) & 0xFF;
            raw_btmp[12 + cnt2 * 4 + 2] = (val >> 8) & 0xFF;
            raw_btmp[12 + cnt2 * 4 + 3] = val & 0xFF;
        }
        fix_checksum(raw_btmp, 0x42544D50, block); // BTMP
        write_b(block, raw_btmp);
        block++;
    }
    
    // 9. Root blocks
    std::vector<uint8_t> raw_root(bs_, 0);
    raw_root[12] = 0; raw_root[13] = 3; // version
    raw_root[16] = (timestamp >> 24) & 0xFF; raw_root[17] = (timestamp >> 16) & 0xFF; raw_root[18] = (timestamp >> 8) & 0xFF; raw_root[19] = timestamp & 0xFF;
    raw_root[20] = 64; // ROOTBITS_RECYCLED
    uint64_t lastbyte = (uint64_t)blocks_total * bs_;
    raw_root[32] = 0; raw_root[33] = 0; raw_root[34] = 0; raw_root[35] = 0;
    raw_root[36] = 0; raw_root[37] = 0; raw_root[38] = 0; raw_root[39] = 0;
    uint32_t lb_h = lastbyte >> 32;
    uint32_t lb_l = lastbyte & 0xFFFFFFFF;
    raw_root[40] = (lb_h >> 24) & 0xFF; raw_root[41] = (lb_h >> 16) & 0xFF; raw_root[42] = (lb_h >> 8) & 0xFF; raw_root[43] = lb_h & 0xFF;
    raw_root[44] = (lb_l >> 24) & 0xFF; raw_root[45] = (lb_l >> 16) & 0xFF; raw_root[46] = (lb_l >> 8) & 0xFF; raw_root[47] = lb_l & 0xFF;
    
    raw_root[48] = (blocks_total >> 24) & 0xFF; raw_root[49] = (blocks_total >> 16) & 0xFF; raw_root[50] = (blocks_total >> 8) & 0xFF; raw_root[51] = blocks_total & 0xFF;
    raw_root[52] = (bs_ >> 24) & 0xFF; raw_root[53] = (bs_ >> 16) & 0xFF; raw_root[54] = (bs_ >> 8) & 0xFF; raw_root[55] = bs_ & 0xFF;
    
    raw_root[96] = (block_bitmapbase >> 24) & 0xFF; raw_root[97] = (block_bitmapbase >> 16) & 0xFF; raw_root[98] = (block_bitmapbase >> 8) & 0xFF; raw_root[99] = block_bitmapbase & 0xFF;
    raw_root[100] = (block_adminspace >> 24) & 0xFF; raw_root[101] = (block_adminspace >> 16) & 0xFF; raw_root[102] = (block_adminspace >> 8) & 0xFF; raw_root[103] = block_adminspace & 0xFF;
    raw_root[104] = (block_root >> 24) & 0xFF; raw_root[105] = (block_root >> 16) & 0xFF; raw_root[106] = (block_root >> 8) & 0xFF; raw_root[107] = block_root & 0xFF;
    raw_root[108] = (block_extentbnoderoot >> 24) & 0xFF; raw_root[109] = (block_extentbnoderoot >> 16) & 0xFF; raw_root[110] = (block_extentbnoderoot >> 8) & 0xFF; raw_root[111] = block_extentbnoderoot & 0xFF;
    raw_root[112] = (block_objectnoderoot >> 24) & 0xFF; raw_root[113] = (block_objectnoderoot >> 16) & 0xFF; raw_root[114] = (block_objectnoderoot >> 8) & 0xFF; raw_root[115] = block_objectnoderoot & 0xFF;
    
    fix_checksum(raw_root, 0x53465300, 0); // ROOT
    write_b(0, raw_root);
    
    fix_checksum(raw_root, 0x53465300, blocks_total - 1);
    write_b(blocks_total - 1, raw_root);
    
    read_only_ = false;
    open();
}

void SFSVolume::add_extent(uint32_t start, uint32_t nxt, uint32_t prev, uint32_t blocks) {
    std::vector<BNodePath> path;
    uint32_t cur = extentbnoderoot_;
    
    while (true) {
        auto raw = read_block(cur);
        uint16_t nodecount = (raw[12] << 8) | raw[13];
        uint8_t isleaf = raw[14];
        uint8_t nodesize = raw[15];
        
        path.push_back({cur, raw, isleaf, nodesize, nodecount});
        
        if (isleaf) break;
        
        uint32_t chosen_child = 0;
        uint32_t base = 16;
        for (int n = nodecount - 1; n >= 0; --n) {
            uint32_t nkey = (raw[base + n * nodesize] << 24) | (raw[base + n * nodesize + 1] << 16) | (raw[base + n * nodesize + 2] << 8) | raw[base + n * nodesize + 3];
            if (n == 0 || start >= nkey) {
                chosen_child = (raw[base + n * nodesize + 4] << 24) | (raw[base + n * nodesize + 5] << 16) | (raw[base + n * nodesize + 6] << 8) | raw[base + n * nodesize + 7];
                break;
            }
        }
        cur = chosen_child;
    }
    
    std::vector<uint8_t> node_data(14, 0);
    node_data[0] = (start >> 24) & 0xFF; node_data[1] = (start >> 16) & 0xFF; node_data[2] = (start >> 8) & 0xFF; node_data[3] = start & 0xFF;
    node_data[4] = (nxt >> 24) & 0xFF; node_data[5] = (nxt >> 16) & 0xFF; node_data[6] = (nxt >> 8) & 0xFF; node_data[7] = nxt & 0xFF;
    node_data[8] = (prev >> 24) & 0xFF; node_data[9] = (prev >> 16) & 0xFF; node_data[10] = (prev >> 8) & 0xFF; node_data[11] = prev & 0xFF;
    node_data[12] = (blocks >> 8) & 0xFF; node_data[13] = blocks & 0xFF;
    
    uint32_t current_key = start;
    std::vector<uint8_t> current_data = node_data;
    
    while (!path.empty()) {
        auto p = path.back();
        path.pop_back();
        
        uint32_t max_nodes = (bs_ - 16) / p.nodesize;
        
        if (p.nodecount < max_nodes) {
            uint32_t base = 16;
            uint32_t insert_idx = 0;
            for (int n = p.nodecount - 1; n >= 0; --n) {
                uint32_t nkey = (p.raw[base + n * p.nodesize] << 24) | (p.raw[base + n * p.nodesize + 1] << 16) | (p.raw[base + n * p.nodesize + 2] << 8) | p.raw[base + n * p.nodesize + 3];
                if (current_key > nkey) {
                    insert_idx = n + 1;
                    break;
                }
            }
            
            if (insert_idx < p.nodecount) {
                uint32_t src_start = base + insert_idx * p.nodesize;
                uint32_t src_end = base + p.nodecount * p.nodesize;
                for (int i = src_end - 1; i >= (int)src_start; --i) {
                    p.raw[i + p.nodesize] = p.raw[i];
                }
            }
            
            for (size_t i = 0; i < current_data.size(); ++i) {
                p.raw[base + insert_idx * p.nodesize + i] = current_data[i];
            }
            
            p.nodecount++;
            p.raw[12] = (p.nodecount >> 8) & 0xFF;
            p.raw[13] = p.nodecount & 0xFF;
            write_block(p.blk, p.raw);
            return;
        } else {
            uint32_t new_blk = alloc_adminspace();
            std::vector<uint8_t> new_raw(bs_, 0);
            fix_checksum(new_raw, 0x424E4443, new_blk); // BNDC
            
            uint16_t half = p.nodecount / 2;
            uint16_t remain = p.nodecount - half;
            
            uint32_t base = 16;
            uint32_t src_start = base + remain * p.nodesize;
            uint32_t src_end = base + p.nodecount * p.nodesize;
            
            for (uint32_t i = 0; i < src_end - src_start; ++i) {
                new_raw[base + i] = p.raw[src_start + i];
            }
            
            new_raw[12] = (half >> 8) & 0xFF; new_raw[13] = half & 0xFF;
            new_raw[14] = p.isleaf;
            new_raw[15] = p.nodesize;
            
            p.raw[12] = (remain >> 8) & 0xFF; p.raw[13] = remain & 0xFF;
            
            uint32_t new_key = (new_raw[base] << 24) | (new_raw[base+1] << 16) | (new_raw[base+2] << 8) | new_raw[base+3];
            
            if (current_key < new_key) {
                uint32_t insert_idx = 0;
                for (int n = remain - 1; n >= 0; --n) {
                    uint32_t nkey = (p.raw[base + n * p.nodesize] << 24) | (p.raw[base + n * p.nodesize + 1] << 16) | (p.raw[base + n * p.nodesize + 2] << 8) | p.raw[base + n * p.nodesize + 3];
                    if (current_key > nkey) {
                        insert_idx = n + 1;
                        break;
                    }
                }
                if (insert_idx < remain) {
                    uint32_t s_start = base + insert_idx * p.nodesize;
                    uint32_t s_end = base + remain * p.nodesize;
                    for (int i = s_end - 1; i >= (int)s_start; --i) {
                        p.raw[i + p.nodesize] = p.raw[i];
                    }
                }
                for (size_t i = 0; i < current_data.size(); ++i) {
                    p.raw[base + insert_idx * p.nodesize + i] = current_data[i];
                }
                remain++;
                p.raw[12] = (remain >> 8) & 0xFF; p.raw[13] = remain & 0xFF;
            } else {
                uint32_t insert_idx = 0;
                for (int n = half - 1; n >= 0; --n) {
                    uint32_t nkey = (new_raw[base + n * p.nodesize] << 24) | (new_raw[base + n * p.nodesize + 1] << 16) | (new_raw[base + n * p.nodesize + 2] << 8) | new_raw[base + n * p.nodesize + 3];
                    if (current_key > nkey) {
                        insert_idx = n + 1;
                        break;
                    }
                }
                if (insert_idx < half) {
                    uint32_t s_start = base + insert_idx * p.nodesize;
                    uint32_t s_end = base + half * p.nodesize;
                    for (int i = s_end - 1; i >= (int)s_start; --i) {
                        new_raw[i + p.nodesize] = new_raw[i];
                    }
                }
                for (size_t i = 0; i < current_data.size(); ++i) {
                    new_raw[base + insert_idx * p.nodesize + i] = current_data[i];
                }
                half++;
                new_raw[12] = (half >> 8) & 0xFF; new_raw[13] = half & 0xFF;
                new_key = (new_raw[base] << 24) | (new_raw[base+1] << 16) | (new_raw[base+2] << 8) | new_raw[base+3];
            }
            
            write_block(p.blk, p.raw);
            write_block(new_blk, new_raw);
            
            current_key = new_key;
            current_data.assign(8, 0);
            current_data[0] = (new_key >> 24) & 0xFF; current_data[1] = (new_key >> 16) & 0xFF; current_data[2] = (new_key >> 8) & 0xFF; current_data[3] = new_key & 0xFF;
            current_data[4] = (new_blk >> 24) & 0xFF; current_data[5] = (new_blk >> 16) & 0xFF; current_data[6] = (new_blk >> 8) & 0xFF; current_data[7] = new_blk & 0xFF;
            
            if (path.empty()) {
                uint32_t old_root_new_blk = alloc_adminspace();
                std::vector<uint8_t> old_root_new_raw = p.raw;
                old_root_new_raw[8] = (old_root_new_blk >> 24) & 0xFF; old_root_new_raw[9] = (old_root_new_blk >> 16) & 0xFF; old_root_new_raw[10] = (old_root_new_blk >> 8) & 0xFF; old_root_new_raw[11] = old_root_new_blk & 0xFF;
                write_block(old_root_new_blk, old_root_new_raw);
                
                std::vector<uint8_t> new_root(bs_, 0);
                fix_checksum(new_root, 0x424E4443, p.blk);
                new_root[12] = 0; new_root[13] = 2; // count 2
                new_root[14] = 0; // isleaf
                new_root[15] = 8; // nodesize
                
                new_root[16] = 0; new_root[17] = 0; new_root[18] = 0; new_root[19] = 0;
                new_root[20] = (old_root_new_blk >> 24) & 0xFF; new_root[21] = (old_root_new_blk >> 16) & 0xFF; new_root[22] = (old_root_new_blk >> 8) & 0xFF; new_root[23] = old_root_new_blk & 0xFF;
                
                new_root[24] = (new_key >> 24) & 0xFF; new_root[25] = (new_key >> 16) & 0xFF; new_root[26] = (new_key >> 8) & 0xFF; new_root[27] = new_key & 0xFF;
                new_root[28] = (new_blk >> 24) & 0xFF; new_root[29] = (new_blk >> 16) & 0xFF; new_root[30] = (new_blk >> 8) & 0xFF; new_root[31] = new_blk & 0xFF;
                
                write_block(p.blk, new_root);
                return;
            }
        }
    }
}

SFSEntry SFSVolume::parse_object(const std::vector<uint8_t>& raw, uint32_t off, uint32_t block) {
    SFSEntry e;
    e.objc_block = block;
    e.objc_offset = off;
    e.uid = (raw[off] << 8) | raw[off+1];
    e.gid = (raw[off+2] << 8) | raw[off+3];
    e.objectnode = (raw[off+4] << 24) | (raw[off+5] << 16) | (raw[off+6] << 8) | raw[off+7];
    e.protect = (raw[off+8] << 24) | (raw[off+9] << 16) | (raw[off+10] << 8) | raw[off+11];
    
    uint32_t a = (raw[off+12] << 24) | (raw[off+13] << 16) | (raw[off+14] << 8) | raw[off+15];
    uint32_t b = (raw[off+16] << 24) | (raw[off+17] << 16) | (raw[off+18] << 8) | raw[off+19];
    
    e.secs = (raw[off+20] << 24) | (raw[off+21] << 16) | (raw[off+22] << 8) | raw[off+23];
    e.bits = raw[off+24];
    
    if (e.bits & OTYPE_DIR) {
        e.hashtable = a;
        e.firstdirblock = b;
    } else {
        e.data = a;
        e.size = b;
    }
    
    size_t p = off + 25;
    size_t end = p;
    while (end < raw.size() && raw[end] != 0) end++;
    e.name = std::string((char*)&raw[p], end - p);
    
    p = end + 1;
    end = p;
    while (end < raw.size() && raw[end] != 0) end++;
    e.comment = std::string((char*)&raw[p], end - p);
    
    return e;
}

void SFSVolume::iter_container_objects(const std::vector<uint8_t>& raw, uint32_t block, std::function<void(const SFSEntry&)> callback) {
    uint32_t off = 24;
    int guard = 0;
    while (off + 27 < bs_) {
        if (++guard > MAX_CHAIN) throw SFSError("runaway object container scan");
        uint32_t objectnode = (raw[off+4] << 24) | (raw[off+5] << 16) | (raw[off+6] << 8) | raw[off+7];
        if (objectnode == 0) break;
        
        SFSEntry e = parse_object(raw, off, block);
        callback(e);
        
        off = off + 25 + e.name.length() + 1 + e.comment.length() + 1;
        if (off & 1) off += 1;
    }
}

std::vector<SFSEntry> SFSVolume::dir_entries(const SFSEntry& entry) {
    std::vector<SFSEntry> entries;
    uint32_t block = entry.firstdirblock;
    int guard = 0;
    while (block) {
        if (++guard > MAX_CHAIN) throw SFSError("cyclic SFS dir container chain");
        auto raw = read_checked(block, OBJC_ID);
        iter_container_objects(raw, block, [&](const SFSEntry& e) {
            entries.push_back(e);
        });
        block = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
    }
    return entries;
}

bool SFSVolume::names_equal(const std::string& a, const std::string& b) const {
    if (case_sensitive_) return a == b;
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); i++) {
        uint8_t ca = a[i];
        if (ca >= 0x61 && ca <= 0x7A) ca -= 0x20;
        uint8_t cb = b[i];
        if (cb >= 0x61 && cb <= 0x7A) cb -= 0x20;
        if (ca != cb) return false;
    }
    return true;
}

SFSEntry SFSVolume::resolve(const std::string& path) {
    SFSEntry e = root_obj_;
    if (path.empty()) return e;
    
    auto parts = split_path(path);
    for (const auto& part : parts) {
        if (e.is_link()) throw SFSError("resolve through link not implemented");
        if (!e.is_dir()) throw SFSError("not a directory");
        
        bool found = false;
        for (const auto& child : dir_entries(e)) {
            if (names_equal(child.name_str(), part)) {
                e = child;
                found = true;
                break;
            }
        }
        if (!found) throw SFSError("path not found: " + part);
    }
    return e;
}

std::vector<Entry> SFSVolume::list_dir(const std::string& path) {
    auto e = resolve(path);
    if (!e.is_dir()) throw SFSError("not a directory");

    std::vector<Entry> res;
    for (const auto& child : dir_entries(e)) {
        Entry ce;
        ce.name = std::vector<uint8_t>(child.name.begin(), child.name.end());
        ce.type = child.is_dir() ? 2 : (child.is_file() ? 1 : 0);
        ce.size = child.is_file() ? child.size : 0;
        ce.comment = std::vector<uint8_t>(child.comment.begin(), child.comment.end());
        ce.blk = child.objectnode;
        ce.protect = child.protect;
        // SFS stores secs since Amiga epoch (1978-01-01)
        if (child.secs > 0) {
            // Convert to Unix timestamp: add offset from 1970 to 1978
            // 8 years = 2922 days (including leap years 1972, 1976)
            constexpr int64_t AMIGA_UNIX_OFFSET = 2922LL * 86400LL;
            ce.mtime_unix = static_cast<int64_t>(child.secs) + AMIGA_UNIX_OFFSET;
        }
        res.push_back(ce);
    }
    return res;
}

std::pair<uint32_t, uint16_t> SFSVolume::find_extent(uint32_t key) {
    uint32_t blocknr = extentbnoderoot_;
    int guard = 0;
    while (true) {
        if (++guard > MAX_CHAIN) throw SFSError("SFS extent btree too deep");
        auto raw = read_checked(blocknr, BNDC_ID);
        uint16_t nodecount = (raw[12] << 8) | raw[13];
        uint8_t isleaf = raw[14];
        uint8_t nodesize = raw[15];
        
        if (nodecount == 0) throw SFSError("empty SFS extent btree node");
        
        uint32_t base = 16;
        uint32_t chosen = 0;
        for (int n = nodecount - 1; n >= 0; n--) {
            uint32_t noff = base + n * nodesize;
            uint32_t nkey = (raw[noff] << 24) | (raw[noff+1] << 16) | (raw[noff+2] << 8) | raw[noff+3];
            if (n == 0 || key >= nkey) {
                chosen = noff;
                break;
            }
        }
        
        if (isleaf) {
            uint32_t nkey = (raw[chosen] << 24) | (raw[chosen+1] << 16) | (raw[chosen+2] << 8) | raw[chosen+3];
            uint32_t nxt = (raw[chosen+4] << 24) | (raw[chosen+5] << 16) | (raw[chosen+6] << 8) | raw[chosen+7];
            uint16_t blocks = (raw[chosen+12] << 8) | raw[chosen+13];
            if (nkey != key) throw SFSError("SFS extent not found");
            return {nxt, blocks};
        }
        blocknr = (raw[chosen+4] << 24) | (raw[chosen+5] << 16) | (raw[chosen+6] << 8) | raw[chosen+7];
    }
}

void SFSVolume::read_data(const SFSEntry& entry, std::function<void(std::span<const uint8_t>)> callback) {
    uint32_t remaining = entry.size;
    uint32_t key = entry.data;
    
    int guard = 0;
    while (remaining > 0) {
        if (++guard > MAX_CHAIN) throw SFSError("cyclic SFS data chain");
        auto [nxt, blocks] = find_extent(key);
        
        uint32_t blocks_left = blocks;
        uint32_t blk = key;
        while (blocks_left > 0 && remaining > 0) {
            uint32_t n = std::min<uint32_t>(blocks_left, std::max<uint32_t>(1, (16 << 20) / bs_));
            auto data = read_block(blk, n);
            uint32_t send = std::min<uint32_t>(remaining, data.size());
            callback(std::span<const uint8_t>(data.data(), send));
            remaining -= send;
            blk += n;
            blocks_left -= n;
        }
        key = nxt;
    }
}

std::string SFSVolume::read_softlink(const SFSEntry& entry) {
    std::string link;
    read_data(entry, [&](std::span<const uint8_t> data) {
        link.append((const char*)data.data(), data.size());
    });
    return link;
}

void SFSVolume::read_file(const std::string& path, std::function<void(std::span<const uint8_t>)> callback) {
    auto e = resolve(path);
    if (e.is_link()) throw SFSError("hard link: open the link target instead");
    if (!e.is_file()) throw SFSError("not a file");
    read_data(e, callback);
}

void SFSVolume::walk(const std::string& path, std::function<void(const std::string&, const SFSEntry&)> callback) {
    auto start = resolve(path);
    if (!start.is_dir()) throw SFSError("not a directory");
    
    std::string base;
    auto parts = split_path(path);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) base += "/";
        base += parts[i];
    }
    
    std::vector<std::pair<std::string, SFSEntry>> stack;
    stack.push_back({base, start});
    
    while (!stack.empty()) {
        auto [prefix, e] = stack.back();
        stack.pop_back();
        
        for (const auto& child : dir_entries(e)) {
            callback(prefix, child);
            if (child.is_dir()) {
                std::string sub = (prefix.empty() ? "" : prefix + "/") + child.name_str();
                stack.push_back({sub, child});
            }
        }
    }
}

uint32_t SFSVolume::node_shift() const {
    return (bs_ == 32768) ? 15 - 5 : (bs_ == 16384) ? 14 - 5 : (bs_ == 8192) ? 13 - 5 : (bs_ == 4096) ? 12 - 5 : (bs_ == 2048) ? 11 - 5 : (bs_ == 1024) ? 10 - 5 : 9 - 5;
}

std::tuple<std::vector<SFSVolume::NodePathEl>, uint32_t, std::vector<uint8_t>, uint32_t> SFSVolume::node_path(uint32_t nodeno) {
    uint32_t shift = node_shift();
    uint32_t blk = objectnoderoot_;
    std::vector<NodePathEl> path;
    
    int guard = 0;
    while (true) {
        if (++guard > 64) throw SFSError("node tree too deep");
        auto raw = read_block(blk);
        uint32_t id = (raw[0] << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
        if (id != 0x4E444320) throw SFSError("bad NDC block");
        
        uint32_t nodenumber = (raw[12] << 24) | (raw[13] << 16) | (raw[14] << 8) | raw[15];
        uint32_t nodes = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
        
        if (nodes == 1) {
            return {path, blk, raw, nodenumber};
        }
        
        uint32_t idx = (nodeno - nodenumber) / nodes;
        uint32_t ptr_off = 20 + idx * 4;
        uint32_t ptr = (raw[ptr_off] << 24) | (raw[ptr_off+1] << 16) | (raw[ptr_off+2] << 8) | raw[ptr_off+3];
        if (ptr == 0) throw SFSError("node not present in tree");
        
        path.push_back({blk, raw, idx});
        blk = ptr >> shift;
        blk &= ~0;
    }
}

void SFSVolume::update_full_bits(const std::vector<NodePathEl>& path, bool child_full) {
    auto leaf_is_full = [&](const std::vector<uint8_t>& r) {
        uint32_t cap = (bs_ - 20) / 10;
        uint32_t nn = (r[12] << 24) | (r[13] << 16) | (r[14] << 8) | r[15];
        for (uint32_t n = 0; n < cap; ++n) {
            if (nn + n == 0) continue;
            uint32_t d = (r[20 + n * 10] << 24) | (r[20 + n * 10 + 1] << 16) | (r[20 + n * 10 + 2] << 8) | r[20 + n * 10 + 3];
            if (d == 0) return false;
        }
        return true;
    };
    
    auto index_is_full = [&](const std::vector<uint8_t>& r) {
        uint32_t cap = (bs_ - 20) / 4;
        for (uint32_t n = 0; n < cap; ++n) {
            uint32_t p = (r[20 + n * 4] << 24) | (r[20 + n * 4 + 1] << 16) | (r[20 + n * 4 + 2] << 8) | r[20 + n * 4 + 3];
            if (p == 0 || (p & 1) == 0) return false;
        }
        return true;
    };
    
    bool cf = child_full;
    for (int i = path.size() - 1; i >= 0; --i) {
        auto p = path[i];
        uint32_t ptr_off = 20 + p.idx * 4;
        uint32_t ptr = (p.raw[ptr_off] << 24) | (p.raw[ptr_off+1] << 16) | (p.raw[ptr_off+2] << 8) | p.raw[ptr_off+3];
        
        bool was_container_full = index_is_full(p.raw);
        if (cf) ptr |= 1; else ptr &= ~1;
        p.raw[ptr_off] = (ptr >> 24) & 0xFF; p.raw[ptr_off+1] = (ptr >> 16) & 0xFF; p.raw[ptr_off+2] = (ptr >> 8) & 0xFF; p.raw[ptr_off+3] = ptr & 0xFF;
        write_block(p.blk, p.raw);
        
        if (cf) {
            cf = index_is_full(p.raw);
            if (!cf) break;
        } else {
            if (!was_container_full) break;
            cf = false;
        }
    }
}

void SFSVolume::set_node(uint32_t nodeno, uint32_t data, uint32_t nxt, uint16_t h16) {
    auto [path, blk, raw, nodenumber] = node_path(nodeno);
    uint32_t off = 20 + (nodeno - nodenumber) * 10;
    
    raw[off] = (data >> 24) & 0xFF; raw[off+1] = (data >> 16) & 0xFF; raw[off+2] = (data >> 8) & 0xFF; raw[off+3] = data & 0xFF;
    raw[off+4] = (nxt >> 24) & 0xFF; raw[off+5] = (nxt >> 16) & 0xFF; raw[off+6] = (nxt >> 8) & 0xFF; raw[off+7] = nxt & 0xFF;
    raw[off+8] = (h16 >> 8) & 0xFF; raw[off+9] = h16 & 0xFF;
    
    write_block(blk, raw);
    
    auto leaf_is_full = [&](const std::vector<uint8_t>& r) {
        uint32_t cap = (bs_ - 20) / 10;
        uint32_t nn = (r[12] << 24) | (r[13] << 16) | (r[14] << 8) | r[15];
        for (uint32_t n = 0; n < cap; ++n) {
            if (nn + n == 0) continue;
            uint32_t d = (r[20 + n * 10] << 24) | (r[20 + n * 10 + 1] << 16) | (r[20 + n * 10 + 2] << 8) | r[20 + n * 10 + 3];
            if (d == 0) return false;
        }
        return true;
    };
    
    update_full_bits(path, leaf_is_full(raw));
}

void SFSVolume::hash_insert(const SFSEntry& parent, uint32_t nodeno, uint16_t h16) {
    if (parent.hashtable == 0) return;
    auto hraw = read_block(parent.hashtable);
    uint32_t idx = h16 % ((bs_ - 16) / 4);
    uint32_t ptr_off = 16 + idx * 4;
    uint32_t head = (hraw[ptr_off] << 24) | (hraw[ptr_off+1] << 16) | (hraw[ptr_off+2] << 8) | hraw[ptr_off+3];
    
    auto [path, blk, raw, nodenumber] = node_path(nodeno);
    uint32_t off = 20 + (nodeno - nodenumber) * 10;
    uint32_t data = (raw[off] << 24) | (raw[off+1] << 16) | (raw[off+2] << 8) | raw[off+3];
    
    set_node(nodeno, data, head, h16);
    
    hraw[ptr_off] = (nodeno >> 24) & 0xFF; hraw[ptr_off+1] = (nodeno >> 16) & 0xFF; hraw[ptr_off+2] = (nodeno >> 8) & 0xFF; hraw[ptr_off+3] = nodeno & 0xFF;
    write_block(parent.hashtable, hraw);
}

void SFSVolume::hash_remove(const SFSEntry& parent, const SFSEntry& entry) {
    if (parent.hashtable == 0) return;
    
    uint16_t h16 = sfs_hash(entry.name_str(), case_sensitive_);
    uint32_t idx = h16 % ((bs_ - 16) / 4);
    
    auto hraw = read_block(parent.hashtable);
    uint32_t ptr_off = 16 + idx * 4;
    uint32_t node = (hraw[ptr_off] << 24) | (hraw[ptr_off+1] << 16) | (hraw[ptr_off+2] << 8) | hraw[ptr_off+3];
    
    uint32_t prev = 0;
    int guard = 0;
    while (node != 0 && node != entry.objectnode) {
        if (++guard > MAX_CHAIN) throw SFSError("cyclic hash chain");
        prev = node;
        auto [path, blk, raw, nodenumber] = node_path(node);
        uint32_t off = 20 + (node - nodenumber) * 10;
        node = (raw[off+4] << 24) | (raw[off+5] << 16) | (raw[off+6] << 8) | raw[off+7];
    }
    if (node == 0) return;
    
    auto [path, blk, raw, nodenumber] = node_path(entry.objectnode);
    uint32_t off = 20 + (entry.objectnode - nodenumber) * 10;
    uint32_t nxt = (raw[off+4] << 24) | (raw[off+5] << 16) | (raw[off+6] << 8) | raw[off+7];
    
    if (prev == 0) {
        hraw[ptr_off] = (nxt >> 24) & 0xFF; hraw[ptr_off+1] = (nxt >> 16) & 0xFF; hraw[ptr_off+2] = (nxt >> 8) & 0xFF; hraw[ptr_off+3] = nxt & 0xFF;
        write_block(parent.hashtable, hraw);
    } else {
        auto [ppath, pblk, praw, pnodenumber] = node_path(prev);
        uint32_t poff = 20 + (prev - pnodenumber) * 10;
        uint32_t pdata = (praw[poff] << 24) | (praw[poff+1] << 16) | (praw[poff+2] << 8) | praw[poff+3];
        uint16_t ph = (praw[poff+8] << 8) | praw[poff+9];
        set_node(prev, pdata, nxt, ph);
    }
}

void SFSVolume::write_file(const std::string& path, std::span<const uint8_t> data, const WriteParams& params) {
    require_writable();
    auto [parent, name] = split_parent(path);
    if (!parent.is_dir()) throw SFSError("parent is not a directory");
    
    try {
        resolve(path);
        throw SFSError("file already exists: " + path);
    } catch (const SFSError& e) {
        if (std::string(e.what()).find("not found") == std::string::npos) throw;
    }
    
    uint32_t nodeno = alloc_objectnode();
    uint32_t blocks_needed = (data.size() + bs_ - 1) / bs_;
    
    uint32_t first_data_block = 0;
    if (blocks_needed > 0) {
        auto chunks = alloc_blocks(blocks_needed);
        first_data_block = chunks[0].first;
        
        uint32_t written = 0;
        for (size_t i = 0; i < chunks.size(); ++i) {
            uint32_t start_blk = chunks[i].first;
            uint32_t count = chunks[i].second;
            
            uint32_t chunk_len = std::min<uint32_t>(count * bs_, data.size() - written);
            std::vector<uint8_t> chunk_data(data.begin() + written, data.begin() + written + chunk_len);
            if (chunk_data.size() % bs_ != 0) {
                chunk_data.resize(((chunk_data.size() + bs_ - 1) / bs_) * bs_, 0);
            }
            blkdev_->write(start_blk * spb_ * blkdev_->sector_size(), chunk_data);
            written += chunk_len;
            
            uint32_t nxt = (i + 1 < chunks.size()) ? chunks[i+1].first : 0;
            uint32_t prev = (i > 0) ? chunks[i-1].first : 0;
            add_extent(start_blk, nxt, prev, count);
        }
    }
    
    auto obj_data = create_fsobject(name, data.size(), first_data_block, nodeno, false, params.comment);
    insert_object(parent, obj_data);
    
    uint16_t h16 = sfs_hash(name, case_sensitive_);
    
    uint32_t obj_blk = 0;
    uint32_t b = parent.firstdirblock;
    while (b != 0) {
        auto r = read_block(b);
        uint32_t base = 24;
        while (base + 25 < bs_) {
            uint32_t p = base + 25;
            while (p < bs_ && r[p] != 0) p++;
            int32_t nl = p - (base + 25);
            if (nl >= 0 && r[base + 25] != 0) {
                std::string n_str((char*)&r[base + 25], nl);
                if (n_str == name) {
                    obj_blk = b;
                    break;
                }
                uint32_t cs = base + 25 + nl + 1;
                uint32_t cp = cs;
                while (cp < bs_ && r[cp] != 0) cp++;
                uint32_t cl = cp - cs;
                uint32_t tl = 25 + nl + 1 + cl + 1;
                if (tl % 2 != 0) tl += 1;
                base += tl;
            } else {
                break;
            }
        }
        if (obj_blk != 0) break;
        b = (r[16] << 24) | (r[17] << 16) | (r[18] << 8) | r[19];
    }
    
    set_node(nodeno, obj_blk, 0, h16);
    hash_insert(parent, nodeno, h16);
    
    auto root_raw = read_block(rootobjectcontainer_);
    root_raw[bs_ - 36 + 8] = (freeblocks_ >> 24) & 0xFF;
    root_raw[bs_ - 36 + 9] = (freeblocks_ >> 16) & 0xFF;
    root_raw[bs_ - 36 + 10] = (freeblocks_ >> 8) & 0xFF;
    root_raw[bs_ - 36 + 11] = freeblocks_ & 0xFF;
    write_block(rootobjectcontainer_, root_raw);
    
    touch_volume();
}

void SFSVolume::mkdir(const std::string& path) {
    require_writable();
    auto [parent, name] = split_parent(path);
    if (!parent.is_dir()) throw SFSError("parent is not a directory");
    
    try {
        resolve(path);
        throw SFSError("already exists: " + path);
    } catch (const SFSError& e) {
        if (std::string(e.what()).find("not found") == std::string::npos) throw;
    }
    
    uint32_t nodeno = alloc_objectnode();
    auto obj_data = create_fsobject(name, 0, 0, nodeno, true);
    insert_object(parent, obj_data);
    
    uint16_t h16 = sfs_hash(name, case_sensitive_);
    
    uint32_t obj_blk = 0;
    uint32_t b = parent.firstdirblock;
    while (b != 0) {
        auto r = read_block(b);
        uint32_t base = 24;
        while (base + 25 < bs_) {
            uint32_t p = base + 25;
            while (p < bs_ && r[p] != 0) p++;
            int32_t nl = p - (base + 25);
            if (nl >= 0 && r[base + 25] != 0) {
                std::string n_str((char*)&r[base + 25], nl);
                if (n_str == name) {
                    obj_blk = b;
                    break;
                }
                uint32_t cs = base + 25 + nl + 1;
                uint32_t cp = cs;
                while (cp < bs_ && r[cp] != 0) cp++;
                uint32_t cl = cp - cs;
                uint32_t tl = 25 + nl + 1 + cl + 1;
                if (tl % 2 != 0) tl += 1;
                base += tl;
            } else {
                break;
            }
        }
        if (obj_blk != 0) break;
        b = (r[16] << 24) | (r[17] << 16) | (r[18] << 8) | r[19];
    }
    
    set_node(nodeno, obj_blk, 0, h16);
    hash_insert(parent, nodeno, h16);
    
    touch_volume();
}

void SFSVolume::makedirs(const std::string& path) {
    auto parts = split_path(path);
    std::string cur = "";
    for (const auto& seg : parts) {
        if (!cur.empty()) cur += "/";
        cur += seg;
        try {
            auto e = resolve(cur);
            if (!e.is_dir()) throw SFSError("exists and is not a directory: " + cur);
        } catch (const SFSError& e) {
            if (std::string(e.what()).find("not found") != std::string::npos) {
                mkdir(cur);
            } else {
                throw;
            }
        }
    }
}

void SFSVolume::remove_object(const SFSEntry& parent, const SFSEntry& entry) {
    auto raw = read_block(entry.objc_block);
    uint32_t off = entry.objc_offset;
    uint32_t size = 25 + entry.name.length() + 1 + entry.comment.length() + 1;
    if (size % 2 != 0) size += 1;
    
    uint32_t end = 24;
    while (end + 25 <= bs_) {
        int32_t name_len = -1;
        for (uint32_t i = end + 25; i < bs_; ++i) {
            if (raw[i] == 0) { name_len = i - (end + 25); break; }
        }
        if (name_len < 0 || raw[end + 25] == 0) break;
        
        uint32_t comment_start = end + 25 + name_len + 1;
        uint32_t cp = comment_start;
        while (cp < bs_ && raw[cp] != 0) cp++;
        uint32_t comment_len = cp - comment_start;
        
        uint32_t total_len = 25 + name_len + 1 + comment_len + 1;
        if (total_len % 2 != 0) total_len += 1;
        end += total_len;
    }
    
    for (uint32_t i = off; i < end - size; ++i) {
        raw[i] = raw[i + size];
    }
    for (uint32_t i = end - size; i < end; ++i) {
        raw[i] = 0;
    }
    fix_checksum(raw, 0x4F424A43, entry.objc_block);
    write_block(entry.objc_block, raw);
    
    if (end - size == 24 && entry.objc_block != parent.firstdirblock) {
        uint32_t nxt = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
        uint32_t prev = (raw[20] << 24) | (raw[21] << 16) | (raw[22] << 8) | raw[23];
        
        if (prev != 0) {
            auto praw = read_block(prev);
            praw[16] = (nxt >> 24) & 0xFF; praw[17] = (nxt >> 16) & 0xFF; praw[18] = (nxt >> 8) & 0xFF; praw[19] = nxt & 0xFF;
            fix_checksum(praw, 0x4F424A43, prev);
            write_block(prev, praw);
        }
        if (nxt != 0) {
            auto nraw = read_block(nxt);
            nraw[20] = (prev >> 24) & 0xFF; nraw[21] = (prev >> 16) & 0xFF; nraw[22] = (prev >> 8) & 0xFF; nraw[23] = prev & 0xFF;
            fix_checksum(nraw, 0x4F424A43, nxt);
            write_block(nxt, nraw);
        }
        free_adminspace(entry.objc_block);
    }
}

std::pair<uint32_t, uint32_t> SFSVolume::remove_extent(uint32_t key) {
    auto extent_leaf_of = [&](uint32_t ekey) -> uint32_t {
        uint32_t b = extentbnoderoot_;
        int g = 0;
        while (true) {
            if (++g > MAX_CHAIN) throw SFSError("extent btree too deep");
            auto r = read_block(b);
            uint16_t nc = (r[12] << 8) | r[13];
            uint8_t il = r[14];
            uint8_t ns = r[15];
            if (il) return b;
            
            for (int n = nc - 1; n >= 0; --n) {
                uint32_t nk = (r[16 + n * ns] << 24) | (r[16 + n * ns + 1] << 16) | (r[16 + n * ns + 2] << 8) | r[16 + n * ns + 3];
                if (n == 0 || ekey >= nk) {
                    b = (r[16 + n * ns + 4] << 24) | (r[16 + n * ns + 5] << 16) | (r[16 + n * ns + 6] << 8) | r[16 + n * ns + 7];
                    break;
                }
            }
        }
    };

    auto patch_extent_field = [&](uint32_t ekey, std::optional<uint32_t> next_val, std::optional<uint32_t> prev_val) {
        uint32_t b = extent_leaf_of(ekey);
        auto r = read_block(b);
        uint16_t nc = (r[12] << 8) | r[13];
        uint8_t ns = r[15];
        for (int n = 0; n < nc; ++n) {
            uint32_t nk = (r[16 + n * ns] << 24) | (r[16 + n * ns + 1] << 16) | (r[16 + n * ns + 2] << 8) | r[16 + n * ns + 3];
            if (nk == ekey) {
                if (next_val) {
                    uint32_t nv = *next_val;
                    r[16 + n * ns + 4] = (nv >> 24) & 0xFF; r[16 + n * ns + 5] = (nv >> 16) & 0xFF; r[16 + n * ns + 6] = (nv >> 8) & 0xFF; r[16 + n * ns + 7] = nv & 0xFF;
                }
                if (prev_val) {
                    uint32_t pv = *prev_val;
                    r[16 + n * ns + 8] = (pv >> 24) & 0xFF; r[16 + n * ns + 9] = (pv >> 16) & 0xFF; r[16 + n * ns + 10] = (pv >> 8) & 0xFF; r[16 + n * ns + 11] = pv & 0xFF;
                }
                write_block(b, r);
                return;
            }
        }
        throw SFSError("extent not found for patch");
    };

    struct PathEntry {
        uint32_t blk;
        std::vector<uint8_t> raw;
        uint32_t chosen;
        uint8_t nodesize;
        uint16_t nodecount;
    };
    std::vector<PathEntry> path;
    uint32_t blk = extentbnoderoot_;
    int guard = 0;
    std::vector<uint8_t> raw;
    uint16_t nodecount;
    uint8_t nodesize;
    
    while (true) {
        if (++guard > MAX_CHAIN) throw SFSError("extent btree too deep");
        raw = read_block(blk);
        nodecount = (raw[12] << 8) | raw[13];
        uint8_t isleaf = raw[14];
        nodesize = raw[15];
        if (isleaf) break;
        
        uint32_t chosen = 0;
        for (int n = nodecount - 1; n >= 0; --n) {
            uint32_t nkey = (raw[16 + n * nodesize] << 24) | (raw[16 + n * nodesize + 1] << 16) | (raw[16 + n * nodesize + 2] << 8) | raw[16 + n * nodesize + 3];
            if (n == 0 || key >= nkey) {
                chosen = n;
                break;
            }
        }
        path.push_back({blk, raw, chosen, nodesize, nodecount});
        blk = (raw[16 + chosen * nodesize + 4] << 24) | (raw[16 + chosen * nodesize + 5] << 16) | (raw[16 + chosen * nodesize + 6] << 8) | raw[16 + chosen * nodesize + 7];
    }
    
    int found = -1;
    for (int n = 0; n < nodecount; ++n) {
        uint32_t nkey = (raw[16 + n * nodesize] << 24) | (raw[16 + n * nodesize + 1] << 16) | (raw[16 + n * nodesize + 2] << 8) | raw[16 + n * nodesize + 3];
        if (nkey == key) { found = n; break; }
    }
    if (found < 0) throw SFSError("extent not found");
    
    uint32_t nxt = (raw[16 + found * nodesize + 4] << 24) | (raw[16 + found * nodesize + 5] << 16) | (raw[16 + found * nodesize + 6] << 8) | raw[16 + found * nodesize + 7];
    uint32_t prv = (raw[16 + found * nodesize + 8] << 24) | (raw[16 + found * nodesize + 9] << 16) | (raw[16 + found * nodesize + 10] << 8) | raw[16 + found * nodesize + 11];
    uint16_t blocks = (raw[16 + found * nodesize + 12] << 8) | raw[16 + found * nodesize + 13];
    
    uint32_t end = 16 + nodecount * nodesize;
    uint32_t s = 16 + found * nodesize;
    for (uint32_t i = s; i < end - nodesize; ++i) {
        raw[i] = raw[i + nodesize];
    }
    for (uint32_t i = end - nodesize; i < end; ++i) {
        raw[i] = 0;
    }
    raw[12] = ((nodecount - 1) >> 8) & 0xFF; raw[13] = (nodecount - 1) & 0xFF;
    write_block(blk, raw);
    
    while (nodecount - 1 == 0 && !path.empty()) {
        uint32_t child = blk;
        auto p = path.back(); path.pop_back();
        blk = p.blk;
        raw = p.raw;
        nodesize = p.nodesize;
        nodecount = p.nodecount;
        
        for (int n = 0; n < nodecount; ++n) {
            uint32_t nchild = (raw[16 + n * nodesize + 4] << 24) | (raw[16 + n * nodesize + 5] << 16) | (raw[16 + n * nodesize + 6] << 8) | raw[16 + n * nodesize + 7];
            if (nchild == child) {
                uint32_t cend = 16 + nodecount * nodesize;
                uint32_t cs = 16 + n * nodesize;
                for (uint32_t i = cs; i < cend - nodesize; ++i) raw[i] = raw[i + nodesize];
                for (uint32_t i = cend - nodesize; i < cend; ++i) raw[i] = 0;
                raw[12] = ((nodecount - 1) >> 8) & 0xFF; raw[13] = (nodecount - 1) & 0xFF;
                write_block(blk, raw);
                break;
            }
        }
        free_adminspace(child);
        nodecount -= 1;
    }
    
    if (prv != 0) patch_extent_field(prv, nxt, std::nullopt);
    if (nxt != 0) patch_extent_field(nxt, std::nullopt, prv);
    
    return {nxt, blocks};
}

void SFSVolume::delete_file_data(const SFSEntry& entry) {
    uint32_t key = entry.data;
    int guard = 0;
    while (key != 0) {
        if (++guard > MAX_CHAIN) throw SFSError("cyclic extent chain");
        auto [nxt, blocks] = remove_extent(key);
        mark_space(key, blocks, true);
        key = nxt;
    }
}

void SFSVolume::delete_by_name(const SFSEntry& parent, const std::string& name, bool recursive) {
    auto kids = dir_entries(parent);
    auto it = std::find_if(kids.begin(), kids.end(), [&](const SFSEntry& e) {
        if (case_sensitive_) return e.name == name;
        std::string a = e.name; std::transform(a.begin(), a.end(), a.begin(), ::toupper);
        std::string b = name; std::transform(b.begin(), b.end(), b.begin(), ::toupper);
        return a == b;
    });
    if (it == kids.end()) throw SFSError("entry not found: " + name);
    SFSEntry entry = *it;
    
    if (entry.is_dir()) {
        auto subkids = dir_entries(entry);
        if (!subkids.empty() && !recursive) throw SFSError("directory not empty: " + entry.name);
        while (!subkids.empty()) {
            delete_by_name(entry, subkids[0].name, true);
            auto new_kids = dir_entries(parent);
            auto it2 = std::find_if(new_kids.begin(), new_kids.end(), [&](const SFSEntry& e) { return e.name == entry.name; });
            if (it2 == new_kids.end()) break;
            entry = *it2;
            subkids = dir_entries(entry);
        }
        
        uint32_t blk = entry.firstdirblock;
        int guard = 0;
        while (blk != 0 && guard < MAX_CHAIN) {
            guard++;
            auto raw = read_block(blk);
            uint32_t nxt = (raw[16] << 24) | (raw[17] << 16) | (raw[18] << 8) | raw[19];
            free_adminspace(blk);
            blk = nxt;
        }
        if (entry.hashtable != 0) {
            free_adminspace(entry.hashtable);
        }
    } else if (entry.is_file()) {
        delete_file_data(entry);
    } else if (entry.is_link() && entry.data != 0) {
        free_adminspace(entry.data);
    }
    
    hash_remove(parent, entry);
    free_objectnode(entry.objectnode);
    remove_object(parent, entry);
}

void SFSVolume::delete_path(const std::string& path, bool recursive) {
    require_writable();
    auto [parent, name] = split_parent(path);
    if (name.empty()) throw SFSError("cannot delete root directory");
    delete_by_name(parent, name, recursive);
    touch_volume();
}

CheckReport SFSVolume::check(bool deep) {
    CheckReport rep;
    rep.ok = true;
    
    int n_files = 0, n_dirs = 0;
    std::unordered_set<uint32_t> seen;
    
    try {
        walk("", [&](const std::string& prefix, const SFSEntry& e) {
            std::string path = (prefix.empty() ? "" : prefix + "/") + e.name_str();
            if (seen.count(e.objectnode) && !e.is_link()) {
                rep.errors.push_back(path + ": objectnode " + std::to_string(e.objectnode) + " reused");
            }
            seen.insert(e.objectnode);
            
            if (e.is_dir()) {
                n_dirs++;
            } else if (e.is_file()) {
                n_files++;
                try {
                    uint32_t remaining = e.size;
                    uint32_t key = e.data;
                    // uint64_t total_extent_blocks = 0;
                    
                    int guard = 0;
                    while (remaining > 0) {
                        if (++guard > MAX_CHAIN) throw SFSError("cyclic extent");
                        auto [nxt, blocks] = find_extent(key);
                        if (nxt + blocks > total_) throw SFSError("data run out of volume");
                        uint32_t chunk_bytes = blocks * bs_;
                        uint32_t consume = std::min<uint32_t>(remaining, chunk_bytes);
                        remaining -= consume;
                        key += blocks;
                        // total_extent_blocks += blocks;
                    }
                    if (deep) {
                        read_data(e, [](std::span<const uint8_t>){});
                    }
                } catch (const std::exception& ex) {
                    rep.errors.push_back(path + ": " + ex.what());
                }
            }
        });
    } catch (const std::exception& ex) {
        rep.errors.push_back(std::string("tree walk aborted: ") + ex.what());
    }
    
    rep.files = n_files;
    rep.dirs = n_dirs;
    rep.used_blocks = totalblocks_ - freeblocks_;
    rep.ok = rep.errors.empty();
    
    return rep;
}

VolumeInfo SFSVolume::get_info() const {
    VolumeInfo info;
    info.label = label_;
    info.dos_type = dos_type_str();
    info.filesystem = "SFS";
    info.total_blocks = totalblocks_;
    info.free_blocks = freeblocks_;
    info.used_blocks = totalblocks_ > freeblocks_ ? totalblocks_ - freeblocks_ : 0;
    info.block_size = bs_;
    info.free_bytes = (uint64_t)freeblocks_ * bs_;
    info.root_block = 0;
    info.read_only = read_only_;
    return info;
}
} // namespace amidisk

