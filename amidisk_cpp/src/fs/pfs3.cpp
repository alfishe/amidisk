#include "pfs3.h"
#include "util.h"
#include <cstring>
#include <algorithm>
#include <unordered_set>

namespace amidisk {

static inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}
static inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}
static inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
}

// Rootblock.disktype values
constexpr uint32_t ID_PFS_DISK = 0x50465301;  // 'PFS\1'
constexpr uint32_t ID_AFS_DISK = 0x41465301;  // 'AFS\1'
constexpr uint32_t ID_MUAF_DISK = 0x6D754146; // 'muAF'
constexpr uint32_t ID_MUPFS_DISK = 0x6D755046;// 'muPF'
constexpr uint32_t ID_PFS2_DISK = 0x50465302; // 'PFS\2'

// Options bits
constexpr uint32_t MODE_HARDDISK = 1;
constexpr uint32_t MODE_SPLITTED_ANODES = 2;
constexpr uint32_t MODE_DIR_EXTENSION = 4;
constexpr uint32_t MODE_DELDIR = 8;
constexpr uint32_t MODE_EXTENSION = 32;
constexpr uint32_t MODE_DATESTAMP = 64;
constexpr uint32_t MODE_SUPERINDEX = 128;
constexpr uint32_t MODE_LARGEFILE = 2048;

// Block IDs
constexpr uint16_t DBLKID = 0x4442;   // 'DB'
constexpr uint16_t ABLKID = 0x4142;   // 'AB'
constexpr uint16_t IBLKID = 0x4942;   // 'IB'
constexpr uint16_t SBLKID = 0x5342;   // 'SB'
constexpr uint16_t BMBLKID = 0x424D;  // 'BM'
constexpr uint16_t BMIBLKID = 0x4D49; // 'MI'
constexpr uint16_t EXTENSIONID = 0x4558; // 'EX'

constexpr uint32_t ANODE_ROOTDIR = 5;

// direntry.type values (dos secondary types)
constexpr int8_t ST_USERDIR = 2;
constexpr int8_t ST_FILE = -3;
constexpr int8_t ST_ROOT = 1;
constexpr int8_t ST_SOFTLINK = 3;
constexpr int8_t ST_LINKDIR = 4;
constexpr int8_t ST_LINKFILE = -4;
constexpr int8_t ST_ROLLOVERFILE = -16;

constexpr uint32_t ROOTBLOCK = 2;
constexpr uint32_t MAX_CHAIN = 1000000;

static inline uint32_t read_u32(const uint8_t* p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static inline uint16_t read_u16(const uint8_t* p) {
    return (p[0] << 8) | p[1];
}

bool PFS3Entry::is_dir() const { return type == ST_USERDIR || type == ST_ROOT; }
bool PFS3Entry::is_file() const { return type == ST_FILE || type == ST_ROLLOVERFILE; }
bool PFS3Entry::is_link() const { return type == ST_SOFTLINK || type == ST_LINKDIR || type == ST_LINKFILE; }

std::string PFS3Entry::type_str() const {
    if (type == ST_USERDIR) return "dir";
    if (type == ST_FILE) return "file";
    if (type == ST_ROLLOVERFILE) return "rollover-file";
    if (type == ST_SOFTLINK) return "softlink";
    if (type == ST_LINKDIR) return "hardlink-dir";
    if (type == ST_LINKFILE) return "hardlink-file";
    return "unknown(" + std::to_string(type) + ")";
}

PFS3Volume::PFS3Volume(std::shared_ptr<BlockDevice> dev, uint32_t sec_per_blk, uint32_t reserved, uint32_t dos_type)
    : dev_(std::move(dev)), spb_(std::max<uint32_t>(sec_per_blk, 1)), dos_type_(dos_type), read_only_(true) {
    bytes_per_block_ = dev_->sector_size() * spb_;
    total_ = dev_->size_bytes() / bytes_per_block_;
}

std::vector<uint8_t> PFS3Volume::read_blocks(uint64_t blocknr, uint32_t count) {
    if (blocknr + count > total_ || blocknr < 0) {
        throw PFS3Error("PFS3 block out of volume");
    }
    std::vector<uint8_t> raw(count * bytes_per_block_);
    dev_->read(blocknr * bytes_per_block_, raw);
    return raw;
}

std::vector<uint8_t> PFS3Volume::read_reserved(uint64_t blocknr) {
    return read_blocks(blocknr, rescluster_);
}

void PFS3Volume::open() {
    auto raw = read_blocks(ROOTBLOCK, 1);
    disktype_ = read_u32(&raw[0]);
    if (disktype_ != ID_PFS_DISK && disktype_ != ID_AFS_DISK && disktype_ != ID_MUAF_DISK && 
        disktype_ != ID_MUPFS_DISK && disktype_ != ID_PFS2_DISK) {
        throw PFS3Error("no PFS3 rootblock");
    }
    
    options_ = read_u32(&raw[4]);
    datestamp_ = read_u32(&raw[8]);
    uint8_t nlen = raw[20];
    label_ = std::string(reinterpret_cast<const char*>(&raw[21]), std::min<uint8_t>(nlen, 31));
    
    lastreserved_ = read_u32(&raw[52]);
    firstreserved_ = read_u32(&raw[56]);
    reserved_free_ = read_u32(&raw[60]);
    reserved_blksize_ = read_u16(&raw[64]);
    rblkcluster_ = read_u16(&raw[66]);
    blocksfree_ = read_u32(&raw[68]);
    alwaysfree_ = read_u32(&raw[72]);
    roving_ptr_ = read_u32(&raw[76]);
    deldir_ = read_u32(&raw[80]);
    disksize_ = read_u32(&raw[84]);
    extension_ = read_u32(&raw[88]);
    
    rescluster_ = reserved_blksize_ / bytes_per_block_;
    if (rescluster_ < 1) throw PFS3Error("bad reserved_blksize");
    
    uint32_t cluster = (rblkcluster_ >= rescluster_) ? rblkcluster_ : rescluster_;
    root_raw_ = read_blocks(ROOTBLOCK, cluster);
    
    anodes_per_block_ = (reserved_blksize_ - 16) / 12;
    index_per_block_ = (reserved_blksize_ - 12) / 4;
    super_index_ = (options_ & MODE_SUPERINDEX) != 0;
    split_anodes_ = (options_ & MODE_SPLITTED_ANODES) != 0;
    large_file_ = (options_ & MODE_LARGEFILE) != 0;
    
    superindex_tab_.resize(16, 0);
    if (extension_ && (options_ & MODE_EXTENSION)) {
        auto ext = read_reserved(extension_);
        if (read_u16(&ext[0]) == EXTENSIONID) {
            for (int i = 0; i < 16; ++i) {
                superindex_tab_[i] = read_u32(&ext[64 + i * 4]);
            }
        }
    }
}

std::string PFS3Volume::dos_type_str() const {
    char buf[5] = {0};
    uint32_t dt = dos_type_ ? dos_type_ : disktype_;
    buf[0] = (dt >> 24) & 0xFF;
    buf[1] = (dt >> 16) & 0xFF;
    buf[2] = (dt >> 8) & 0xFF;
    buf[3] = dt & 0xFF;
    return std::string(buf);
}


void PFS3Volume::write_raw(uint64_t blocknr, std::span<const uint8_t> data) {
    dev_->write(blocknr * bytes_per_block_, data);
}

void PFS3Volume::write_reserved(uint64_t blocknr, std::vector<uint8_t>& data) {
    write_be32(data.data() + 4, datestamp_);
    write_raw(blocknr, data);
}

void PFS3Volume::require_writable() const {
    if (read_only_) throw PFS3Error("PFS3 volume is read-only");
}

void PFS3Volume::begin() {
    require_writable();
    datestamp_++;
    if (bulk_depth_ && !bmb_cache_.empty()) {
        if (!roving_) roving_ = lastreserved_ + 1;
        return;
    }
    bmb_cache_.clear();
    bmb_dirty_.clear();
    ab_cache_.clear();
    ab_dirty_.clear();
    if (!roving_) roving_ = lastreserved_ + 1;
}

void PFS3Volume::flush_meta(bool touch_root_date) {
    // Collect and sort dirty reserved blocks (bitmaps and anodes)
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> dirty;
    for (uint32_t s : bmb_dirty_) dirty.push_back(bmb_cache_[s]);
    for (uint32_t s : ab_dirty_) dirty.push_back(ab_cache_[s]);
    
    std::sort(dirty.begin(), dirty.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    
    size_t i = 0;
    while (i < dirty.size()) {
        size_t j = i + 1;
        uint32_t step = rescluster_;
        while (j < dirty.size() && dirty[j].first == dirty[j - 1].first + step) {
            j++;
        }
        std::vector<uint8_t> payload;
        for (size_t k = i; k < j; ++k) {
            payload.insert(payload.end(), dirty[k].second.begin(), dirty[k].second.end());
        }
        dev_->write(dirty[i].first * bytes_per_block_, payload);
        i = j;
    }
    
    bmb_dirty_.clear();
    ab_dirty_.clear();
    
    // update root block
    write_be32(root_raw_.data() + 8, datestamp_);
    write_be32(root_raw_.data() + 60, reserved_free_);
    write_be32(root_raw_.data() + 68, blocksfree_);
    write_be32(root_raw_.data() + 76, 0); // roving_ptr (hint only)
    write_raw(ROOTBLOCK, root_raw_);
    
    if (has_rext_) {
        write_be32(rext_raw_.data() + 8, datestamp_);
        if (touch_root_date) {
            // Write current date (dummy for now since AmiDisk hasn't fully abstracted time)
            write_be16(rext_raw_.data() + 16, 0); // days
            write_be16(rext_raw_.data() + 18, 0); // mins
            write_be16(rext_raw_.data() + 20, 0); // ticks
        }
        write_reserved(extension_, rext_raw_);
    }
}

void PFS3Volume::drop_txn_caches() {
    bmb_cache_.clear();
    bmb_dirty_.clear();
    ab_cache_.clear();
    ab_dirty_.clear();
    anode_cache_.clear();
    index_cache_.clear();
}

void PFS3Volume::commit(bool touch_root_date) {
    if (bulk_depth_) {
        bulk_ops_++;
        if (bulk_ops_ % bulk_every_ == 0) flush_meta(touch_root_date);
        return;
    }
    flush_meta(touch_root_date);
    drop_txn_caches();
}

const std::vector<uint8_t>& PFS3Volume::get_index_block(uint32_t nr) {
    if (index_cache_.count(nr)) return index_cache_[nr];
    
    uint32_t blocknr = 0;
    if (super_index_) {
        uint32_t snr = nr / index_per_block_;
        uint32_t soff = nr % index_per_block_;
        if (snr >= superindex_tab_.size() || !superindex_tab_[snr]) throw PFS3Error("PFS3 superindex missing");
        
        auto sraw = read_reserved(superindex_tab_[snr]);
        uint16_t sid = read_u16(&sraw[0]);
        if (sid != SBLKID && sid != IBLKID) throw PFS3Error("bad PFS3 superblock id");
        blocknr = read_u32(&sraw[12 + soff * 4]);
    } else {
        if (nr >= 99) throw PFS3Error("PFS3 index block out of range");
        blocknr = read_u32(&root_raw_[116 + nr * 4]);
    }
    
    if (!blocknr) throw PFS3Error("PFS3 index block is zero");
    auto raw = read_reserved(blocknr);
    if (read_u16(&raw[0]) != IBLKID) throw PFS3Error("bad PFS3 index block id");
    
    index_cache_[nr] = std::move(raw);
    return index_cache_[nr];
}

PFS3Volume::AnodeData PFS3Volume::get_anode(uint32_t anodenr) {
    if (anode_cache_.count(anodenr)) return anode_cache_[anodenr];
    
    uint32_t seqnr = 0, offset = 0;
    if (split_anodes_) {
        seqnr = anodenr >> 16;
        offset = anodenr & 0xFFFF;
    } else {
        seqnr = anodenr / anodes_per_block_;
        offset = anodenr % anodes_per_block_;
    }
    
    uint32_t inr = seqnr / index_per_block_;
    uint32_t ioff = seqnr % index_per_block_;
    const auto& iraw = get_index_block(inr);
    
    uint32_t ablk = read_u32(&iraw[12 + ioff * 4]);
    if (ablk == 0) throw PFS3Error("PFS3 anode block missing");
    
    auto araw = read_reserved(ablk);
    if (read_u16(&araw[0]) != ABLKID) throw PFS3Error("bad PFS3 anode block id");
    if (offset >= anodes_per_block_) throw PFS3Error("PFS3 anode offset out of range");
    
    AnodeData data;
    data.cs = read_u32(&araw[16 + offset * 12]);
    data.bn = read_u32(&araw[20 + offset * 12]);
    data.nxt = read_u32(&araw[24 + offset * 12]);
    
    anode_cache_[anodenr] = data;
    return data;
}

std::vector<std::pair<uint32_t, uint32_t>> PFS3Volume::anode_chain(uint32_t anodenr) {
    std::vector<std::pair<uint32_t, uint32_t>> chain;
    uint32_t guard = 0;
    while (anodenr) {
        if (++guard > MAX_CHAIN) throw PFS3Error("cyclic PFS3 anode chain");
        auto adata = get_anode(anodenr);
        if (adata.cs && adata.bn != 0xFFFFFFFF) {
            chain.push_back({adata.cs, adata.bn});
        }
        anodenr = adata.nxt;
    }
    return chain;
}

PFS3Entry PFS3Volume::parse_direntry(const std::vector<uint8_t>& raw, uint32_t off) {
    PFS3Entry e;
    uint8_t nxt = raw[off];
    e.type = static_cast<int8_t>(raw[off + 1]);
    e.anode = read_u32(&raw[off + 2]);
    e.size = read_u32(&raw[off + 6]);
    e.days = read_u16(&raw[off + 10]);
    e.mins = read_u16(&raw[off + 12]);
    e.ticks = read_u16(&raw[off + 14]);
    uint8_t prot_low = raw[off + 16];
    uint8_t nlen = raw[off + 17];
    e.name = std::string(reinterpret_cast<const char*>(&raw[off + 18]), nlen);
    
    uint32_t coff = off + 18 + nlen;
    uint8_t clen = (coff < off + nxt) ? raw[coff] : 0;
    e.comment = std::string(reinterpret_cast<const char*>(&raw[coff + 1]), clen);
    
    e.link = e.uid = e.gid = 0;
    e.protect = prot_low;
    
    if (options_ & MODE_DIR_EXTENSION) {
        uint32_t fields_end = off + nxt;
        uint16_t flags = read_u16(&raw[fields_end - 2]);
        uint32_t pos = fields_end - 2;
        std::vector<uint16_t> vals(11, 0);
        uint16_t f = flags;
        for (int i = 0; i < 11; ++i) {
            if (f & 1) {
                pos -= 2;
                vals[i] = read_u16(&raw[pos]);
            }
            f >>= 1;
        }
        e.link = (vals[0] << 16) | vals[1];
        e.uid = vals[2];
        e.gid = vals[3];
        e.protect = (((vals[4] << 16) | vals[5]) & 0xFFFFFF00) | prot_low;
        if (large_file_) {
            uint64_t fsizex = vals[10];
            e.size |= (fsizex << 32);
        }
    }
    return e;
}

std::vector<PFS3Entry> PFS3Volume::dir_entries(uint32_t anodenr) {
    std::vector<PFS3Entry> entries;
    uint32_t cap = reserved_blksize_;
    auto chain = anode_chain(anodenr);
    for (const auto& [cs, bn] : chain) {
        for (uint32_t k = 0; k < cs; ++k) {
            auto raw = read_reserved(bn + k);
            if (read_u16(&raw[0]) != DBLKID) throw PFS3Error("bad PFS3 dir block");
            uint32_t off = 20;
            while (off < cap && raw[off]) {
                entries.push_back(parse_direntry(raw, off));
                off += raw[off];
            }
        }
    }
    return entries;
}

PFS3Entry PFS3Volume::root_entry() {
    PFS3Entry e;
    e.name = label_;
    e.type = ST_USERDIR;
    e.anode = ANODE_ROOTDIR;
    e.size = 0;
    e.protect = 0;
    return e;
}

static std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    size_t end = path.find_first_of("/\\");
    while (end != std::string::npos) {
        if (end > start) parts.push_back(path.substr(start, end - start));
        start = end + 1;
        end = path.find_first_of("/\\", start);
    }
    if (start < path.length()) parts.push_back(path.substr(start));
    return parts;
}

static char upper(char c) {
    if (c >= 0x61 && c <= 0x7A) return c - 0x20;
    auto uc = static_cast<unsigned char>(c);
    if (uc >= 0xE0 && uc <= 0xFE && uc != 0xF7) return uc - 0x20;
    return c;
}

static bool names_equal(const std::string& a, const std::string& b) {
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); ++i) {
        if (upper(a[i]) != upper(b[i])) return false;
    }
    return true;
}

PFS3Entry PFS3Volume::resolve(const std::string& path) {
    auto parts = split_path(path);
    auto cur = root_entry();
    for (const auto& seg : parts) {
        if (!cur.is_dir()) throw PFS3Error("not a directory");
        bool found = false;
        auto entries = dir_entries(cur.anode);
        for (const auto& e : entries) {
            if (names_equal(e.name, seg)) {
                cur = e;
                found = true;
                break;
            }
        }
        if (!found) throw PFS3Error("path not found");
    }
    return cur;
}

std::vector<Entry> PFS3Volume::list_dir(const std::string& path) {
    auto e = resolve(path);
    if (!e.is_dir()) throw PFS3Error("not a directory");
    auto pfsentries = dir_entries(e.anode);
    std::vector<Entry> entries;
    for (const auto& p : pfsentries) {
        Entry gen;
        gen.name = std::vector<uint8_t>(p.name.begin(), p.name.end());
        gen.comment = std::vector<uint8_t>(p.comment.begin(), p.comment.end());
        gen.size = p.size;
        gen.type = p.is_dir() ? 2 : 1;
        gen.protect = p.protect;
        if (p.days > 0 || p.mins > 0 || p.ticks > 0) {
            auto tp = amiga_to_datetime(p.days, p.mins, p.ticks);
            gen.mtime_unix = std::chrono::duration_cast<std::chrono::seconds>(
                tp.time_since_epoch()).count();
        }
        entries.push_back(gen);
    }
    return entries;
}

void PFS3Volume::read_file(const std::string& path, std::function<void(std::span<const uint8_t>)> callback) {
    auto e = resolve(path);
    if (!e.is_file()) throw PFS3Error("not a file");
    uint64_t remaining = e.size;
    auto chain = anode_chain(e.anode);
    for (const auto& [cs, bn] : chain) {
        for (uint32_t k = 0; k < cs; ++k) {
            if (remaining == 0) return;
            auto raw = read_blocks(bn + k, 1);
            uint64_t to_read = std::min<uint64_t>(remaining, bytes_per_block_);
            callback(std::span<const uint8_t>(raw.data(), to_read));
            remaining -= to_read;
        }
    }
}

void PFS3Volume::walk(const std::string& path, std::function<void(const std::string&, const PFS3Entry&)> callback) {
    auto start = resolve(path);
    if (!start.is_dir()) throw PFS3Error("not a directory");
    
    std::string base;
    auto parts = split_path(path);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) base += "/";
        base += parts[i];
    }
    
    std::vector<std::pair<std::string, uint32_t>> stack;
    stack.push_back({base, start.anode});
    
    while (!stack.empty()) {
        auto [prefix, anode] = stack.back();
        stack.pop_back();
        
        for (const auto& e : dir_entries(anode)) {
            callback(prefix, e);
            if (e.is_dir()) {
                std::string sub = (prefix.empty() ? "" : prefix + "/") + e.name_str();
                stack.push_back({sub, e.anode});
            }
        }
    }
}

CheckReport PFS3Volume::check(bool deep) {
    CheckReport rep;
    rep.ok = true;
    std::unordered_set<uint32_t> seen_anodes;
    
    try {
        walk("", [&](const std::string& prefix, const PFS3Entry& e) {
            std::string path = (prefix.empty() ? "" : prefix + "/") + e.name_str();
            if (seen_anodes.count(e.anode) && !e.is_link()) {
                rep.errors.push_back(path + ": anode " + std::to_string(e.anode) + " reused");
            }
            seen_anodes.insert(e.anode);
            
            if (e.is_dir()) {
                rep.dirs++;
            } else if (e.is_file()) {
                rep.files++;
                try {
                    auto chain = anode_chain(e.anode);
                    uint64_t total = 0;
                    for (const auto& [cs, bn] : chain) {
                        total += cs * bytes_per_block_;
                    }
                    if (total < e.size) {
                        rep.errors.push_back(path + ": anode chain holds " + std::to_string(total) + " bytes < size " + std::to_string(e.size));
                    }
                    for (const auto& [cs, bn] : chain) {
                        if (bn + cs > total_) {
                            rep.errors.push_back(path + ": data run out of volume");
                        }
                    }
                    if (deep) {
                        read_file(path, [](std::span<const uint8_t>){});
                    }
                } catch (const std::exception& ex) {
                    rep.errors.push_back(path + ": " + ex.what());
                }
            }
        });
    } catch (const std::exception& ex) {
        rep.errors.push_back(std::string("tree walk aborted: ") + ex.what());
    }
    
    rep.ok = rep.errors.empty();
    return rep;
}

std::pair<uint8_t*, size_t> PFS3Volume::res_bitmap_view() {
    return {root_raw_.data(), bytes_per_block_ + 12};
}

uint32_t PFS3Volume::numreserved() const {
    return (lastreserved_ - firstreserved_ + 1) / rescluster_;
}

uint32_t PFS3Volume::alloc_reserved() {
    auto [raw, base] = res_bitmap_view();
    uint32_t nres = numreserved();
    for (uint32_t i = 0; i < (nres + 31) / 32; ++i) {
        uint32_t field = read_be32(raw + base + i * 4);
        if (!field) continue;
        for (int j = 31; j >= 0; --j) {
            if (field & (1U << j)) {
                uint32_t idx = i * 32 + (31 - j);
                uint32_t blocknr = firstreserved_ + idx * rescluster_;
                if (blocknr <= lastreserved_) {
                    write_be32(raw + base + i * 4, field & ~(1U << j));
                    reserved_free_--;
                    return blocknr;
                }
            }
        }
    }
    throw PFS3Error("PFS3 reserved area is full");
}

void PFS3Volume::free_reserved(uint32_t blocknr) {
    auto [raw, base] = res_bitmap_view();
    uint32_t idx = (blocknr - firstreserved_) / rescluster_;
    uint32_t i = idx / 32;
    uint32_t j = 31 - (idx % 32);
    uint32_t field = read_be32(raw + base + i * 4);
    write_be32(raw + base + i * 4, field | (1U << j));
    reserved_free_++;
}

uint32_t PFS3Volume::lpb() const {
    return reserved_blksize_ / 4 - 3;
}

uint32_t PFS3Volume::bitmapstart() const {
    return lastreserved_ + 1;
}

uint32_t PFS3Volume::bitmapindex_capacity() const {
    return super_index_ ? 104 : 5;
}

uint32_t PFS3Volume::get_bitmapindex_blocknr(uint32_t nr) {
    if (nr >= bitmapindex_capacity()) throw PFS3Error("bitmap index out of range");
    return read_be32(&root_raw_[96 + nr * 4]);
}

std::pair<uint32_t, std::vector<uint8_t>>& PFS3Volume::get_bmb(uint32_t seqnr) {
    if (bmb_cache_.count(seqnr)) return bmb_cache_[seqnr];
    
    uint32_t inr = seqnr / index_per_block_;
    uint32_t ioff = seqnr % index_per_block_;
    uint32_t miblk = get_bitmapindex_blocknr(inr);
    if (!miblk) throw PFS3Error("missing bitmap index block");
    
    auto miraw = read_reserved(miblk);
    if (read_be16(&miraw[0]) != BMIBLKID) throw PFS3Error("bad bitmap index id");
    
    uint32_t bmblk = read_be32(&miraw[12 + ioff * 4]);
    if (!bmblk) throw PFS3Error("missing bitmap block");
    
    auto raw = read_reserved(bmblk);
    if (read_be16(&raw[0]) != BMBLKID) throw PFS3Error("bad bitmap block id");
    
    bmb_cache_[seqnr] = {bmblk, raw};
    return bmb_cache_[seqnr];
}

PFS3Volume::MainBit PFS3Volume::main_bit(uint32_t blocknr) const {
    uint32_t bit = blocknr - bitmapstart();
    uint32_t nr = bit / 32;
    uint32_t i = bit % 32;
    uint32_t lp = lpb();
    return {nr / lp, nr % lp, 1U << (31 - i)};
}

bool PFS3Volume::block_is_free(uint32_t blocknr) {
    auto b = main_bit(blocknr);
    auto& [bmblk, raw] = get_bmb(b.seq);
    return (read_be32(&raw[12 + b.off * 4]) & b.mask) != 0;
}

void PFS3Volume::set_main_bit(uint32_t blocknr, bool free) {
    auto b = main_bit(blocknr);
    auto& [bmblk, raw] = get_bmb(b.seq);
    uint32_t field = read_be32(&raw[12 + b.off * 4]);
    write_be32(raw.data() + 12 + b.off * 4, free ? (field | b.mask) : (field & ~b.mask));
    bmb_dirty_.push_back(b.seq);
}

std::vector<std::pair<uint32_t, uint32_t>> PFS3Volume::alloc_main(uint32_t count) {
    if (count > blocksfree_) throw PFS3Error("PFS3 volume full");
    
    uint32_t total_blocks = disksize_ ? disksize_ : total_;
    uint32_t start = bitmapstart();
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    uint32_t got = 0;
    
    uint32_t align = (bytes_per_block_ < 4096) ? (4096 / bytes_per_block_) : 1;
    if (count < 4096 || blocksfree_ - count < 4096) align = 1;
    
    uint32_t current_roving = roving_;
    if (align > 1) {
        current_roving = ((current_roving + align - 1) / align) * align;
    }
    
    auto find_run = [&](uint32_t search_start, uint32_t search_end) -> bool {
        uint32_t current = search_start;
        while (current <= search_end && got < count) {
            if (block_is_free(current)) {
                uint32_t run_start = current;
                uint32_t run_len = 0;
                while (current <= search_end && got < count && block_is_free(current)) {
                    set_main_bit(current, false);
                    run_len++;
                    got++;
                    current++;
                }
                runs.push_back({run_start, run_len});
            } else {
                current += align; // simplified alignment skipping
            }
        }
        return got >= count;
    };
    
    if (find_run(current_roving, total_blocks - 1)) {
        blocksfree_ -= got;
        roving_ = (runs.back().first + runs.back().second) % total_blocks;
        if (roving_ < start) roving_ = start;
        return runs;
    }
    
    // Wrap around
    align = 1; // Drop alignment for second pass
    if (find_run(start, current_roving - 1)) {
        blocksfree_ -= got;
        roving_ = (runs.back().first + runs.back().second) % total_blocks;
        if (roving_ < start) roving_ = start;
        return runs;
    }
    
    throw PFS3Error("Failed to allocate main blocks despite blocksfree_");
}

void PFS3Volume::free_main_run(uint32_t start, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        set_main_bit(start + i, true);
    }
    blocksfree_ += length;
}

uint32_t PFS3Volume::anode_index_blocknr(uint32_t inr, bool create) {
    if (super_index_) {
        uint32_t snr = inr / index_per_block_;
        uint32_t soff = inr % index_per_block_;
        if (snr >= 16) throw PFS3Error("superindex exhausted");
        uint32_t sblk = superindex_tab_[snr];
        if (!sblk) {
            if (!create) return 0;
            sblk = alloc_reserved();
            std::vector<uint8_t> raw(reserved_blksize_, 0);
            write_be16(&raw[0], SBLKID);
            write_be16(&raw[2], 0);
            write_be32(&raw[4], 0);
            write_be32(&raw[8], snr);
            write_reserved(sblk, raw);
            superindex_tab_[snr] = sblk;
            if (has_rext_) {
                write_be32(&rext_raw_[64 + snr * 4], sblk);
                // We should flush this to disk at some point... handled by flush_meta
            }
        }
        auto sraw = read_reserved(sblk);
        uint32_t iblk = read_be32(&sraw[12 + soff * 4]);
        if (!iblk && create) {
            iblk = alloc_reserved();
            std::vector<uint8_t> iraw(reserved_blksize_, 0);
            write_be16(&iraw[0], IBLKID);
            write_be16(&iraw[2], 0);
            write_be32(&iraw[4], 0);
            write_be32(&iraw[8], inr);
            write_reserved(iblk, iraw);
            write_be32(&sraw[12 + soff * 4], iblk);
            write_reserved(sblk, sraw);
        }
        return iblk;
    }
    
    if (inr >= 99) throw PFS3Error("anode index table exhausted");
    uint32_t iblk = read_be32(&root_raw_[116 + inr * 4]);
    if (!iblk && create) {
        iblk = alloc_reserved();
        std::vector<uint8_t> iraw(reserved_blksize_, 0);
        write_be16(&iraw[0], IBLKID);
        write_be16(&iraw[2], 0);
        write_be32(&iraw[4], 0);
        write_be32(&iraw[8], inr);
        write_reserved(iblk, iraw);
        write_be32(&root_raw_[116 + inr * 4], iblk);
    }
    return iblk;
}

std::pair<uint32_t, std::vector<uint8_t>>& PFS3Volume::anode_block(uint32_t seqnr, bool create) {
    if (ab_cache_.count(seqnr)) return ab_cache_[seqnr];
    
    uint32_t inr = seqnr / index_per_block_;
    uint32_t ioff = seqnr % index_per_block_;
    uint32_t iblk = anode_index_blocknr(inr, create);
    
    static std::pair<uint32_t, std::vector<uint8_t>> empty_ret = {0, {}};
    if (!iblk) return empty_ret;
    
    auto iraw = read_reserved(iblk);
    int32_t ablk_raw = read_be32(&iraw[12 + ioff * 4]);
    uint32_t ablk = static_cast<uint32_t>(ablk_raw);
    
    if (ablk_raw <= 0) {
        if (!create) return empty_ret;
        ablk = alloc_reserved();
        std::vector<uint8_t> araw(reserved_blksize_, 0);
        write_be16(&araw[0], ABLKID);
        write_be16(&araw[2], 0);
        write_be32(&araw[4], 0);
        write_be32(&araw[8], seqnr);
        write_reserved(ablk, araw);
        write_be32(&iraw[12 + ioff * 4], ablk);
        write_reserved(iblk, iraw);
        ab_cache_[seqnr] = {ablk, araw};
        return ab_cache_[seqnr];
    }
    
    auto araw = read_reserved(ablk);
    if (read_be16(&araw[0]) != ABLKID) throw PFS3Error("bad anode block id");
    ab_cache_[seqnr] = {ablk, araw};
    return ab_cache_[seqnr];
}

void PFS3Volume::save_anode(uint32_t anodenr, uint32_t cs, uint32_t bn, uint32_t nxt) {
    uint32_t seqnr = anodenr >> 16;
    uint32_t offset = anodenr & 0xFFFF;
    auto& [ablk, araw] = anode_block(seqnr, true);
    write_be32(&araw[16 + offset * 12], cs);
    write_be32(&araw[20 + offset * 12], bn);
    write_be32(&araw[24 + offset * 12], nxt);
    ab_dirty_.push_back(seqnr);
    anode_cache_.erase(anodenr);
}

std::optional<uint32_t> PFS3Volume::try_alloc_anode_in(uint32_t s) {
    auto& [ablk1, araw1] = anode_block(s, false);
    if (!ablk1) {
        auto& [ablk2, araw2] = anode_block(s, true);
        if (!ablk2) return std::nullopt; // should not happen
        
        for (uint32_t k = 0; k < anodes_per_block_; ++k) {
            if (s == 0 && k <= 2) continue; // ANODE_ROOTDIR etc
            uint32_t cs = read_be32(&araw2[16 + k * 12]);
            uint32_t bn = read_be32(&araw2[20 + k * 12]);
            uint32_t nxt = read_be32(&araw2[24 + k * 12]);
            if (bn == 0 && cs == 0 && nxt == 0) {
                uint32_t anodenr = (s << 16) | k;
                save_anode(anodenr, 0, 0xFFFFFFFF, 0);
                return anodenr;
            }
        }
    } else {
        for (uint32_t k = 0; k < anodes_per_block_; ++k) {
            if (s == 0 && k <= 2) continue;
            uint32_t cs = read_be32(&araw1[16 + k * 12]);
            uint32_t bn = read_be32(&araw1[20 + k * 12]);
            uint32_t nxt = read_be32(&araw1[24 + k * 12]);
            if (bn == 0 && cs == 0 && nxt == 0) {
                uint32_t anodenr = (s << 16) | k;
                save_anode(anodenr, 0, 0xFFFFFFFF, 0);
                return anodenr;
            }
        }
    }
    return std::nullopt;
}

uint32_t PFS3Volume::alloc_anode(uint32_t hint_seqnr) {
    if (hint_seqnr) {
        auto r = try_alloc_anode_in(hint_seqnr);
        if (r) return *r;
    }
    for (uint32_t seqnr = 0; seqnr < 0x10000; ++seqnr) {
        if (seqnr == hint_seqnr && hint_seqnr) continue;
        auto r = try_alloc_anode_in(seqnr);
        if (r) return *r;
    }
    throw PFS3Error("PFS3 anode space exhausted");
}

void PFS3Volume::free_anode(uint32_t anodenr) {
    save_anode(anodenr, 0, 0, 0);
}

std::vector<uint8_t> PFS3Volume::build_direntry(int8_t etype, uint32_t anode, uint32_t fsize, const std::string& name, const std::string& comment, uint32_t protect) {
    if (name.length() < 1 || name.length() > 107) throw PFS3Error("invalid PFS3 name length");
    if (name.find('/') != std::string::npos || name.find(':') != std::string::npos) throw PFS3Error("invalid characters in name");
    
    std::string comm = comment.substr(0, 79);
    
    std::vector<uint8_t> entry;
    entry.push_back(0); // placeholder for next
    entry.push_back(etype);
    
    uint8_t buf[16];
    write_be32(&buf[0], anode);
    write_be32(&buf[4], fsize);
    write_be16(&buf[8], 0); // days
    write_be16(&buf[10], 0); // mins
    write_be16(&buf[12], 0); // ticks
    buf[14] = protect & 0xFF;
    buf[15] = name.length();
    entry.insert(entry.end(), buf, buf + 16);
    entry.insert(entry.end(), name.begin(), name.end());
    entry.push_back(comm.length());
    entry.insert(entry.end(), comm.begin(), comm.end());
    
    if (entry.size() % 2 != 0) entry.push_back(0); // align
    if (options_ & MODE_DIR_EXTENSION) {
        entry.push_back(0);
        entry.push_back(0); // empty extrafields
    }
    
    entry[0] = entry.size();
    return entry;
}

uint32_t PFS3Volume::entries_end(const std::vector<uint8_t>& raw) {
    uint32_t off = 20;
    while (off < reserved_blksize_ && raw[off]) {
        off += raw[off];
    }
    return off;
}

std::string PFS3Volume::entry_name(const std::vector<uint8_t>& entry_bytes) {
    uint32_t nlen = entry_bytes[17];
    return std::string((const char*)&entry_bytes[18], nlen);
}

void PFS3Volume::add_entry(const PFS3Entry& dir_entry, const std::vector<uint8_t>& entry_bytes) {
    if (dir_tail_.count(dir_entry.anode)) {
        auto tail = dir_tail_[dir_entry.anode];
        if (tail.end + entry_bytes.size() + 1 <= reserved_blksize_) {
            auto raw = read_reserved(tail.bn);
            if (entries_end(raw) == tail.end) {
                std::copy(entry_bytes.begin(), entry_bytes.end(), raw.begin() + tail.end);
                if (tail.end + entry_bytes.size() < reserved_blksize_) {
                    raw[tail.end + entry_bytes.size()] = 0;
                }
                write_reserved(tail.bn, raw);
                dir_tail_[dir_entry.anode] = {tail.nr, tail.bn, (uint32_t)(tail.end + entry_bytes.size())};
                dir_names(dir_entry.anode).insert(upper_key(entry_name(entry_bytes)));
                return;
            }
        }
        dir_tail_.erase(dir_entry.anode);
    }
    
    auto chain = anode_chain(dir_entry.anode);
    uint32_t parent = dirblock_parent(dir_entry.anode);
    for (auto& [nr, bn] : chain) {
        auto raw = read_reserved(bn);
        uint32_t end = entries_end(raw);
        if (end + entry_bytes.size() + 1 <= reserved_blksize_) {
            std::copy(entry_bytes.begin(), entry_bytes.end(), raw.begin() + end);
            if (end + entry_bytes.size() < reserved_blksize_) {
                raw[end + entry_bytes.size()] = 0;
            }
            write_reserved(bn, raw);
            dir_tail_[dir_entry.anode] = {nr, bn, (uint32_t)(end + entry_bytes.size())};
            dir_names(dir_entry.anode).insert(upper_key(entry_name(entry_bytes)));
            return;
        }
    }
    
    uint32_t newblk = alloc_reserved();
    uint32_t hint_seqnr = chain.empty() ? 0 : (chain.back().first >> 16);
    uint32_t newnr = alloc_anode(hint_seqnr);
    uint32_t last_nr = chain.empty() ? dir_entry.anode : chain.back().first;
    
    auto cs_bn_nxt = get_anode(last_nr);
    save_anode(last_nr, cs_bn_nxt.cs, cs_bn_nxt.bn, newnr);
    save_anode(newnr, 1, newblk, 0);
    
    std::vector<uint8_t> raw(reserved_blksize_, 0);
    write_be16(&raw[0], DBLKID);
    write_be16(&raw[2], 0);
    write_be32(&raw[4], 0);
    write_be32(&raw[8], 0);
    write_be32(&raw[12], dir_entry.anode);
    write_be32(&raw[16], parent);
    std::copy(entry_bytes.begin(), entry_bytes.end(), raw.begin() + 20);
    write_reserved(newblk, raw);
    
    dir_tail_[dir_entry.anode] = {newnr, newblk, (uint32_t)(20 + entry_bytes.size())};
    dir_names(dir_entry.anode).insert(upper_key(entry_name(entry_bytes)));
}

uint32_t PFS3Volume::dirblock_parent(uint32_t dir_anodenr) {
    if (dir_anodenr == 2) return 0; // ANODE_ROOTDIR
    auto chain = anode_chain(dir_anodenr);
    if (chain.empty()) return 0;
    auto raw = read_reserved(chain[0].second);
    return read_be32(&raw[16]);
}

std::string PFS3Volume::upper_key(const std::string& name) const {
    std::string out = name;
    for (char& c : out) c = upper(c);
    return out;
}

std::unordered_set<std::string>& PFS3Volume::dir_names(uint32_t dir_anodenr) {
    if (dir_index_.count(dir_anodenr)) return dir_index_[dir_anodenr];
    std::unordered_set<std::string> names;
    for (auto& [nr, bn] : anode_chain(dir_anodenr)) {
        auto raw = read_reserved(bn);
        uint32_t off = 20;
        while (off < reserved_blksize_ && raw[off]) {
            uint32_t nlen = raw[off + 17];
            names.insert(upper_key(std::string((const char*)&raw[off + 18], nlen)));
            off += raw[off];
        }
    }
    dir_index_[dir_anodenr] = names;
    return dir_index_[dir_anodenr];
}

void PFS3Volume::dir_cache_drop(std::optional<uint32_t> dir_anodenr) {
    if (!dir_anodenr) {
        dir_index_.clear();
        dir_tail_.clear();
    } else {
        dir_index_.erase(*dir_anodenr);
        dir_tail_.erase(*dir_anodenr);
    }
}

std::optional<PFS3Volume::DirLocation> PFS3Volume::locate_entry(uint32_t dir_anodenr, const std::string& name) {
    if (!dir_names(dir_anodenr).count(upper_key(name))) return std::nullopt;
    for (auto& [nr, bn] : anode_chain(dir_anodenr)) {
        auto raw = read_reserved(bn);
        uint32_t off = 20;
        while (off < reserved_blksize_ && raw[off]) {
            auto e = parse_direntry(raw, off);
            if (names_equal(e.name, name)) {
                return DirLocation{bn, off, e};
            }
            off += raw[off];
        }
    }
    return std::nullopt;
}

PFS3Entry PFS3Volume::remove_entry(uint32_t dir_anodenr, const std::string& name) {
    auto loc = locate_entry(dir_anodenr, name);
    if (!loc) throw PFS3Error("entry not found: " + name);
    
    dir_index_[dir_anodenr].erase(upper_key(name));
    dir_tail_.erase(dir_anodenr);
    
    uint32_t bn = loc->bn;
    uint32_t off = loc->off;
    PFS3Entry e = loc->entry;
    
    auto raw = read_reserved(bn);
    uint32_t size = raw[off];
    uint32_t end = entries_end(raw);
    
    std::copy(raw.begin() + off + size, raw.begin() + end, raw.begin() + off);
    std::fill(raw.begin() + end - size, raw.begin() + end, 0);
    write_reserved(bn, raw);
    
    if (raw[20] == 0) { // empty block
        auto chain = anode_chain(dir_anodenr);
        for (size_t i = 0; i < chain.size(); ++i) {
            if (chain[i].second == bn && i > 0) {
                uint32_t prev_nr = chain[i - 1].first;
                auto [pcs, pbn, pnxt] = get_anode(prev_nr);
                auto [cs, b, nxt] = get_anode(chain[i].first);
                save_anode(prev_nr, pcs, pbn, nxt);
                free_anode(chain[i].first);
                free_reserved(bn);
                break;
            }
        }
    }
    return e;
}

PFS3Entry PFS3Volume::resolve_dir(const std::string& path) {
    auto e = resolve(path);
    if (!e.is_dir()) throw PFS3Error("not a directory: " + path);
    return e;
}

std::pair<PFS3Entry, std::string> PFS3Volume::split_parent(const std::string& path) {
    auto parts = split_path(path);
    if (parts.empty()) throw PFS3Error("empty path");
    std::string key;
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        if (i > 0) key += "/";
        key += parts[i];
    }
    if (last_split_key_ == key && last_split_parent_) {
        return {*last_split_parent_, parts.back()};
    }
    auto parent = resolve_dir(key);
    last_split_key_ = key;
    last_split_parent_ = parent;
    return {parent, parts.back()};
}

void PFS3Volume::mkdir(const std::string& path) {
    begin();
    auto [parent, name] = split_parent(path);
    if (locate_entry(parent.anode, name)) throw PFS3Error("already exists: " + path);
    
    uint32_t anodenr = alloc_anode();
    uint32_t dirblk = alloc_reserved();
    std::vector<uint8_t> raw(reserved_blksize_, 0);
    write_be16(&raw[0], DBLKID);
    write_be32(&raw[12], anodenr);
    write_be32(&raw[16], parent.anode);
    write_reserved(dirblk, raw);
    save_anode(anodenr, 1, dirblk, 0);
    
    dir_cache_drop(anodenr);
    auto entry_bytes = build_direntry(ST_USERDIR, anodenr, 0, name);
    add_entry(parent, entry_bytes);
    commit();
}

void PFS3Volume::makedirs(const std::string& path) {
    if (last_makedirs_path_ == path) return;
    last_makedirs_path_ = path;
    
    auto parts = split_path(path);
    std::string cur;
    for (const auto& seg : parts) {
        if (!cur.empty()) cur += "/";
        cur += seg;
        try {
            auto e = resolve(cur);
            if (!e.is_dir()) throw PFS3Error("exists and is not a directory");
        } catch (const PFS3Error& ex) {
            std::string msg = ex.what();
            if (msg.find("not found") == std::string::npos) throw;
            mkdir(cur);
        }
    }
}

void PFS3Volume::delete_path(const std::string& path, bool recursive) {
    begin();
    auto [parent, name] = split_parent(path);
    auto loc = locate_entry(parent.anode, name);
    if (!loc) throw PFS3Error("not found: " + path);
    
    if (loc->entry.is_dir()) {
        if (!recursive) {
            auto entries = dir_entries(loc->entry.anode);
            if (!entries.empty()) throw PFS3Error("directory not empty");
        } else {
            auto entries = dir_entries(loc->entry.anode);
            for (const auto& e : entries) {
                delete_path(path + "/" + e.name, true);
            }
        }
        
        auto chain = anode_chain(loc->entry.anode);
        for (auto& [nr, bn] : chain) {
            free_reserved(bn);
            free_anode(nr);
        }
        dir_cache_drop(loc->entry.anode);
        remove_entry(parent.anode, name);
    } else {
        delete_file_data(loc->entry);
        remove_entry(parent.anode, name);
    }
    commit();
}

void PFS3Volume::delete_file_data(const PFS3Entry& entry) {
    uint32_t nr = entry.anode;
    uint32_t guard = 0;
    while (nr) {
        guard++;
        if (guard > 1000000) throw PFS3Error("cyclic anode chain");
        auto [cs, bn, nxt] = get_anode(nr);
        if (cs && bn != 0 && bn != 0xFFFFFFFF) {
            free_main_run(bn, cs);
        }
        free_anode(nr);
        nr = nxt;
    }
}

void PFS3Volume::write_file(const std::string& path, std::span<const uint8_t> data, const WriteParams& params) {
    begin();
    auto [parent, name] = split_parent(path);
    uint32_t size = data.size();
    
    auto existing = locate_entry(parent.anode, name);
    if (existing) {
        if (!existing->entry.is_file()) throw PFS3Error("exists and is not a file: " + path);
        delete_file_data(existing->entry);
        remove_entry(parent.anode, name);
    }
    
    uint32_t nblocks = (size + bytes_per_block_ - 1) / bytes_per_block_;
    uint32_t anodenr = alloc_anode();
    
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    try {
        if (nblocks) {
            runs = alloc_main(nblocks);
            uint32_t pos = 0;
            for (auto [s, n] : runs) {
                uint32_t want = std::min<uint32_t>(n * bytes_per_block_, size - pos);
                uint32_t full = want - (want % bytes_per_block_);
                if (full) {
                    write_raw(s, data.subspan(pos, full));
                }
                if (want != full) {
                    std::vector<uint8_t> tail;
                    tail.assign(data.begin() + pos + full, data.begin() + pos + want);
                    tail.resize(bytes_per_block_, 0);
                    write_raw(s + full / bytes_per_block_, tail);
                }
                pos += n * bytes_per_block_;
            }
            
            uint32_t cur = anodenr;
            for (size_t i = 0; i < runs.size(); ++i) {
                uint32_t nxt = 0;
                if (i + 1 < runs.size()) nxt = alloc_anode(cur >> 16);
                save_anode(cur, runs[i].second, runs[i].first, nxt);
                cur = nxt;
            }
        } else {
            save_anode(anodenr, 0, 0xFFFFFFFF, 0);
        }
    } catch (...) {
        for (auto [s, n] : runs) free_main_run(s, n);
        free_anode(anodenr);
        throw;
    }
    
    auto entry = build_direntry(ST_FILE, anodenr, size, name, params.comment, 0);
    add_entry(parent, entry);
    commit();
}

void PFS3Volume::format(const std::string& label, uint32_t dos_type) {
    if (dev_->is_read_only()) throw PFS3Error("device is read-only");
    if (label.empty() || label.length() > 31) throw PFS3Error("invalid label length");
    
    uint32_t bs = bytes_per_block_;
    uint32_t tot = total_;
    uint64_t MAXSMALLDISK = 5ULL * 253 * 253 * 32;
    uint64_t MAXDISKSIZE1K = 104ULL * 253 * 253 * 32;
    if (tot > MAXDISKSIZE1K) throw PFS3Error("PFS3 format beyond 100 GB not supported here");
    
    bool supermode = tot > MAXSMALLDISK;
    uint32_t resblocksize = std::max<uint32_t>(1024, bs);
    uint32_t opts = MODE_HARDDISK | MODE_SPLITTED_ANODES | MODE_DIR_EXTENSION |
                    16 | MODE_DATESTAMP | 512 | 1024 | MODE_EXTENSION;
    if (supermode) opts |= MODE_SUPERINDEX;
    
    uint32_t rescluster = resblocksize / bs;
    
    uint64_t taken = 32;
    uint64_t i = 2048;
    while (i && i / 2 < tot) {
        uint32_t m = (i >= 512 * 2048) ? 10 : 14;
        taken += taken * m / 16;
        i <<= 1;
    }
    taken /= resblocksize / 1024;
    taken = std::min<uint64_t>(4096 + 255 * 1024 * 8, taken - 1);
    uint32_t numreserved = (taken + 31) & ~31;
    
    uint32_t numblocks_1k = 1;
    uint32_t j = 125;
    while (j < numreserved / 32) {
        numblocks_1k++;
        j += 256;
    }
    uint32_t rb_blocks = (1024 * numblocks_1k + resblocksize - 1) / resblocksize;
    uint32_t rblkcluster = rescluster * rb_blocks;
    
    uint32_t firstreserved = 2;
    uint32_t lastreserved = rescluster * numreserved + firstreserved - 1;
    if (lastreserved + 8 >= tot) throw PFS3Error("volume too small for PFS3");
    
    uint32_t reserved_free = numreserved - rb_blocks - 1;
    uint32_t blocksfree = tot - rescluster * numreserved - firstreserved;
    
    // Time
    uint16_t d = 0, m = 0, t = 0; // dummy for now, maybe use real time
    
    // Bootblock
    std::vector<uint8_t> boot(2 * bs, 0);
    write_be32(&boot[0], ID_PFS_DISK);
    write_raw(0, boot);
    
    // Root cluster
    std::vector<uint8_t> root(rblkcluster * bs, 0);
    write_be32(&root[0], ID_PFS_DISK);
    write_be32(&root[4], opts);
    write_be32(&root[8], 1);
    write_be16(&root[12], d);
    write_be16(&root[14], m);
    write_be16(&root[16], t);
    write_be16(&root[18], 0xF0);
    root[20] = label.length();
    std::copy(label.begin(), label.end(), root.begin() + 21);
    write_be32(&root[52], lastreserved);
    write_be32(&root[56], firstreserved);
    write_be32(&root[60], reserved_free);
    write_be16(&root[64], resblocksize);
    write_be16(&root[66], rblkcluster);
    write_be32(&root[68], blocksfree);
    write_be32(&root[72], blocksfree / 20);
    write_be32(&root[76], 0);
    write_be32(&root[80], 0);
    write_be32(&root[84], tot);
    write_be32(&root[88], firstreserved + rblkcluster);
    write_be32(&root[92], 0);
    
    uint32_t bmoff = bs;
    write_be16(&root[bmoff], BMBLKID);
    write_be16(&root[bmoff + 2], 0);
    write_be32(&root[bmoff + 4], 1);
    write_be32(&root[bmoff + 8], 0);
    
    uint32_t base = bmoff + 12;
    for (uint32_t k = 0; k < numreserved / 32; ++k) {
        write_be32(&root[base + k * 4], 0xFFFFFFFF);
    }
    uint32_t last = 0;
    for (uint32_t k = 0; k < numreserved % 32; ++k) {
        last |= 0x80000000 >> k;
    }
    if (numreserved % 32) {
        write_be32(&root[base + (numreserved / 32) * 4], last);
    }
    for (uint32_t k = 0; k < rb_blocks + 1; ++k) {
        uint32_t o = base + (k / 32) * 4;
        uint32_t v = read_be32(&root[o]);
        write_be32(&root[o], v ^ (0x80000000 >> (k % 32)));
    }
    
    disktype_ = ID_PFS_DISK;
    options_ = opts;
    datestamp_ = 1;
    label_ = label;
    lastreserved_ = lastreserved;
    firstreserved_ = firstreserved;
    reserved_free_ = reserved_free;
    reserved_blksize_ = resblocksize;
    rescluster_ = rescluster;
    blocksfree_ = blocksfree;
    alwaysfree_ = blocksfree / 20;
    disksize_ = tot;
    extension_ = firstreserved + rblkcluster;
    root_raw_ = root;
    anodes_per_block_ = (resblocksize - 16) / 12;
    index_per_block_ = (resblocksize - 12) / 4;
    super_index_ = supermode;
    split_anodes_ = true;
    large_file_ = false;
    superindex_tab_.assign(16, 0);
    small_indexblocks_.assign(99, 0);
    has_rext_ = true;
    read_only_ = false;
    
    std::vector<uint8_t> rext(resblocksize, 0);
    write_be16(&rext[0], EXTENSIONID);
    write_be16(&rext[2], 0);
    write_be32(&rext[4], 0);
    write_be32(&rext[8], 1);
    write_be32(&rext[12], (19 << 16) + 2);
    write_be16(&rext[16], d);
    write_be16(&rext[18], m);
    write_be16(&rext[20], t);
    write_be16(&rext[56], 32);
    rext_raw_ = rext;
    
    begin();
    datestamp_ = 1;
    
    uint32_t lpb_val = lpb();
    uint32_t bits_needed = (tot - (lastreserved + 1) + 31) / 32;
    uint32_t no_bmb = (bits_needed + lpb_val - 1) / lpb_val;
    uint32_t cap = bitmapindex_capacity();
    uint32_t n_mi = (no_bmb + index_per_block_ - 1) / index_per_block_;
    if (n_mi > cap) throw PFS3Error("volume too large for bitmap index");
    
    uint32_t seq = 0;
    for (uint32_t minr = 0; minr < n_mi; ++minr) {
        uint32_t miblk = alloc_reserved();
        std::vector<uint8_t> miraw(resblocksize, 0);
        write_be16(&miraw[0], BMIBLKID);
        write_be16(&miraw[2], 0);
        write_be32(&miraw[4], 1);
        write_be32(&miraw[8], minr);
        
        for (uint32_t o = 0; o < index_per_block_; ++o) {
            if (seq >= no_bmb) break;
            uint32_t bmblk = alloc_reserved();
            std::vector<uint8_t> bmraw(resblocksize, 0);
            write_be16(&bmraw[0], BMBLKID);
            write_be16(&bmraw[2], 0);
            write_be32(&bmraw[4], 1);
            write_be32(&bmraw[8], seq);
            for (uint32_t k = 0; k < lpb_val; ++k) {
                write_be32(&bmraw[12 + k * 4], 0xFFFFFFFF);
            }
            write_reserved(bmblk, bmraw);
            write_be32(&miraw[12 + o * 4], bmblk);
            seq++;
        }
        write_reserved(miblk, miraw);
        write_be32(&root_raw_[96 + minr * 4], miblk);
    }
    
    auto& [ablk, araw] = anode_block(0, true);
    for (uint32_t k = 0; k < 5; ++k) {
        write_be32(&araw[16 + k * 12], 0);
        write_be32(&araw[20 + k * 12], 0xFFFFFFFF);
        write_be32(&araw[24 + k * 12], 0);
    }
    uint32_t rootdirblk = alloc_reserved();
    write_be32(&araw[16 + 5 * 12], 1); // ANODE_ROOTDIR = 5
    write_be32(&araw[20 + 5 * 12], rootdirblk);
    write_be32(&araw[24 + 5 * 12], 0);
    ab_dirty_.push_back(0);
    
    std::vector<uint8_t> draw(resblocksize, 0);
    write_be16(&draw[0], DBLKID);
    write_be16(&draw[2], 0);
    write_be32(&draw[4], 0);
    write_be32(&draw[8], 0);
    write_be32(&draw[12], 5);
    write_be32(&draw[16], 0);
    write_reserved(rootdirblk, draw);
    
    commit(false);
}

VolumeInfo PFS3Volume::get_info() const {
    VolumeInfo info;
    info.label = label_;
    info.dos_type = dos_type_str();
    info.filesystem = "PFS3";
    uint64_t tot = disksize_ ? disksize_ : total_;
    info.total_blocks = tot;
    info.free_blocks = blocksfree_;
    info.used_blocks = tot > blocksfree_ ? tot - blocksfree_ : 0;
    info.block_size = bytes_per_block_;
    info.free_bytes = (uint64_t)blocksfree_ * bytes_per_block_;
    info.root_block = 2; // PFS3 Root Block
    info.read_only = read_only_;
    return info;
}
} // namespace amidisk
