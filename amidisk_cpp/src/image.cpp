#include "image.h"
#include "fs/ffs.h"
#include "fs/pfs3.h"
#include "fs/sfs.h"
#include "blkdev/part_blkdev.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace amidisk {

// ---------------------------------------------------------------------------
//  IdentifyInfo
// ---------------------------------------------------------------------------

nlohmann::json IdentifyInfo::to_json() const {
    nlohmann::json j;
    if (!declared_dos_type.empty()) j["declared_dos_type"] = declared_dos_type;
    j["size_bytes"] = size_bytes;
    if (!boot_magic.empty()) j["boot_magic"] = boot_magic;
    j["guesses"] = guesses;
    return j;
}

// ---------------------------------------------------------------------------
//  Engine selection helper
// ---------------------------------------------------------------------------

std::unique_ptr<Volume> create_engine_for_dostype(
    std::shared_ptr<BlockDevice> bdev, uint32_t dos_type,
    uint32_t sec_per_blk, uint32_t reserved)
{
    uint32_t tag3 = dos_type >> 8;
    bool is_pfs3 = (tag3 == 0x504653 || tag3 == 0x504453 ||
                    dos_type == 0x6D754146 || dos_type == 0x6D755046 ||
                    dos_type == 0x41465301);
    bool is_sfs = (tag3 == 0x534653);

    if (is_pfs3) {
        return std::make_unique<PFS3Volume>(bdev, sec_per_blk, reserved, dos_type);
    } else if (is_sfs) {
        return std::make_unique<SFSVolume>(bdev, dos_type);
    } else {
        return std::make_unique<FFSVolume>(bdev, sec_per_blk, reserved, dos_type);
    }
}

// ---------------------------------------------------------------------------
//  DiskImage
// ---------------------------------------------------------------------------

std::unique_ptr<DiskImage> DiskImage::open(const std::string& path, bool read_only) {
    auto blkdev = open_blkdev(path, read_only);
    return std::make_unique<DiskImage>(blkdev, path, !read_only);
}

DiskImage::DiskImage(std::shared_ptr<BlockDevice> blkdev, const std::string& path, bool writable)
    : blkdev_(blkdev), path_(path), writable_(writable)
{
    detect();
}

void DiskImage::detect() {
    // 1. RDB within the first 16 blocks
    try {
        rdisk_ = RDisk::peek(blkdev_);
        if (rdisk_) {
            if (blkdev_->kind() == DeviceKind::Vhd)
                kind_ = "vhd+rdb";
            else
                kind_ = "hdf+rdb";
            for (const auto& p : rdisk_->partitions()) {
                volumes_.push_back(std::make_unique<VolumeRef>(this, p->drv_name(), p.get()));
            }
            return;
        }
    } catch (const BlockDeviceError&) {}

    // 2. MBR-wrapped RDB (PiStorm / Emu68 SD cards)
    rdisk_ = probe_mbr_rdb();
    if (rdisk_) {
        kind_ = "mbr+rdb";
        for (const auto& p : rdisk_->partitions()) {
            volumes_.push_back(std::make_unique<VolumeRef>(this, p->drv_name(), p.get()));
        }
        return;
    }

    // 3. ADF floppy image
    uint64_t sz = blkdev_->size_bytes();
    if (sz == ADF_SIZE_DD || sz == ADF_SIZE_HD) {
        kind_ = "adf";
        volumes_.push_back(std::make_unique<VolumeRef>(this, "DF0", nullptr));
        return;
    }

    // 4. RDB-less hardfile: single volume spanning the whole image
    try {
        auto probe = std::make_unique<VolumeRef>(this, "DH0", nullptr);
        probe->mount();
        kind_ = "hdf-bare";
        volumes_.push_back(std::move(probe));
        return;
    } catch (const std::exception&) {}

    // 5. Not recognised — keep it addressable so format can claim it
    kind_ = "blank";
    volumes_.push_back(std::make_unique<VolumeRef>(this, "DH0", nullptr));
}

std::unique_ptr<RDisk> DiskImage::probe_mbr_rdb() {
    // Find an RDB nested inside an MBR partition (type 0x76 etc.)
    std::vector<uint8_t> mbr(blkdev_->sector_size());
    try {
        blkdev_->read(0, mbr);
    } catch (...) {
        return nullptr;
    }

    // Check MBR signature 0x55AA at offset 510
    if (mbr.size() < 512) return nullptr;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return nullptr;

    uint64_t total_blocks = blkdev_->num_blocks();

    for (int i = 0; i < 4; i++) {
        uint32_t off = 446 + i * 16;
        uint8_t ptype = mbr[off + 4];

        // Read start LBA and sector count (little-endian)
        uint32_t start = mbr[off + 8]  | (mbr[off + 9]  << 8) |
                        (mbr[off + 10] << 16) | (mbr[off + 11] << 24);
        uint32_t count = mbr[off + 12] | (mbr[off + 13] << 8) |
                        (mbr[off + 14] << 16) | (mbr[off + 15] << 24);

        if (!ptype || !count) continue;
        if ((uint64_t)start + count > total_blocks) continue;

        // Create a sub-range block device and probe for RDSK
        auto sub = std::make_shared<PartBlockDevice>(blkdev_, start, count, blkdev_->sector_size());
        auto peeked = RDisk::peek(sub);
        if (peeked) {
            return peeked;
        }
    }
    return nullptr;
}

VolumeRef* DiskImage::get_volume(const std::string& name) const {
    // Empty → return sole volume or error
    if (name.empty()) {
        if (volumes_.size() == 1) return volumes_[0].get();
        std::string names;
        for (size_t i = 0; i < volumes_.size(); i++) {
            if (i) names += ", ";
            names += volumes_[i]->name();
        }
        throw ImageError("image has " + std::to_string(volumes_.size()) +
                         " volumes, specify one of: " + names);
    }

    // Case-insensitive name match
    std::string lo = name;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const auto& vol : volumes_) {
        std::string vn = vol->name();
        std::transform(vn.begin(), vn.end(), vn.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (vn == lo) return vol.get();
    }

    // Numeric index
    if (!lo.empty() && std::all_of(lo.begin(), lo.end(), ::isdigit)) {
        size_t idx = std::stoul(lo);
        if (idx < volumes_.size()) return volumes_[idx].get();
    }

    // Label match
    for (const auto& vol : volumes_) {
        std::string lbl = vol->label();
        if (!lbl.empty()) {
            std::transform(lbl.begin(), lbl.end(), lbl.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lbl == lo) return vol.get();
        }
    }

    std::string names;
    for (size_t i = 0; i < volumes_.size(); i++) {
        if (i) names += ", ";
        names += volumes_[i]->name();
    }
    throw ImageError("no volume '" + name + "' in image (have: " + names + ")");
}

std::pair<VolumeRef*, std::string> DiskImage::parse_path(const std::string& spec) const {
    std::string volname, path;
    size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        volname = spec.substr(0, colon);
        path = spec.substr(colon + 1);
    } else {
        path = spec;
    }
    return {get_volume(volname), path};
}

nlohmann::json DiskImage::get_info() const {
    nlohmann::json info;
    info["path"] = path_;
    info["kind"] = kind_;
    info["size_bytes"] = blkdev_->size_bytes();
    info["writable"] = writable_;

    if (rdisk_) {
        info["rdb_present"] = true;
        nlohmann::json parts = nlohmann::json::array();
        for (const auto& p : rdisk_->partitions()) {
            parts.push_back(p->get_info());
        }
        info["partitions"] = parts;

        nlohmann::json fss = nlohmann::json::array();
        for (const auto& f : rdisk_->filesystems()) {
            fss.push_back(f->get_info());
        }
        info["filesystems"] = fss;
    }

    nlohmann::json vols = nlohmann::json::array();
    for (const auto& v : volumes_) {
        nlohmann::json entry;
        entry["name"] = v->name();
        try {
            auto* mounted = v->mount();
            auto vi = mounted->get_info();
            nlohmann::json vj;
            vj["label"] = vi.label;
            vj["dos_type"] = vi.dos_type;
            vj["filesystem"] = vi.filesystem;
            vj["total_blocks"] = vi.total_blocks;
            vj["free_blocks"] = vi.free_blocks;
            vj["used_blocks"] = vi.used_blocks;
            vj["free_bytes"] = vi.free_bytes;
            vj["block_size"] = vi.block_size;
            entry["volume"] = vj;
        } catch (const std::exception& ex) {
            entry["error"] = ex.what();
            try {
                entry["identify"] = v->identify().to_json();
            } catch (...) {}
        }
        vols.push_back(entry);
    }
    info["volumes"] = vols;
    return info;
}

void DiskImage::close() {
    if (blkdev_) {
        blkdev_->close();
        blkdev_.reset();
    }
}

// ---------------------------------------------------------------------------
//  VolumeRef
// ---------------------------------------------------------------------------

VolumeRef::VolumeRef(DiskImage* image, const std::string& name, const Partition* partition)
    : image_(image), name_(name), partition_(partition) {}

VolumeRef::~VolumeRef() = default;

std::shared_ptr<BlockDevice> VolumeRef::create_blkdev() const {
    if (partition_) return partition_->create_blkdev();
    return image_->blkdev();
}

std::unique_ptr<Volume> VolumeRef::raw_volume(uint32_t override_dos_type) const {
    if (partition_) {
        const auto& env = partition_->dos_env();
        uint32_t dt = override_dos_type ? override_dos_type : env.dos_type;
        uint32_t spb = std::max<uint32_t>(env.sec_per_blk, 1);
        uint32_t res = env.reserved;
        return create_engine_for_dostype(partition_->create_blkdev(), dt, spb, res);
    }
    // Bare volume: sniff bootblock to pick engine
    uint32_t dt = override_dos_type;
    if (!dt) {
        std::vector<uint8_t> boot(image_->blkdev()->sector_size());
        image_->blkdev()->read(0, boot);
        if (boot.size() >= 4)
            dt = read_be32(boot, 0);
    }
    return create_engine_for_dostype(image_->blkdev(), dt ? dt : 0x444F5300, 1, 2);
}

Volume* VolumeRef::mount() {
    if (!vol_) {
        vol_ = raw_volume(0);
        vol_->open();
    }
    return vol_.get();
}

std::string VolumeRef::dos_type_str() const {
    if (partition_) {
        return partition_->get_dos_type_str();
    }
    if (vol_) {
        return vol_->dos_type_str();
    }
    return "?";
}

std::string VolumeRef::label() const {
    try {
        auto v = raw_volume();
        v->open();
        return v->get_label();
    } catch (...) {
        return "";
    }
}

IdentifyInfo VolumeRef::identify() const {
    if (partition_) {
        return identify_volume(partition_->create_blkdev(), partition_->dos_env().dos_type);
    }
    return identify_volume(image_->blkdev(), 0);
}

// ---------------------------------------------------------------------------
//  identify_volume — best-effort identification of an unmountable volume
// ---------------------------------------------------------------------------

IdentifyInfo identify_volume(std::shared_ptr<BlockDevice> dev, uint32_t dos_type) {
    IdentifyInfo out;
    out.size_bytes = dev->size_bytes();

    if (dos_type) {
        out.declared_dos_type = dos_type_to_str(dos_type);
    }

    std::vector<uint8_t> b0;
    try {
        b0.resize(dev->sector_size());
        dev->read(0, b0);
    } catch (const std::exception& ex) {
        out.guesses.push_back(std::string("unreadable first block: ") + ex.what());
        return out;
    }

    // Printable boot magic
    if (b0.size() >= 4) {
        std::string magic;
        for (int i = 0; i < 4; i++) {
            unsigned char c = b0[i];
            magic += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
        }
        out.boot_magic = magic;
    }

    auto& guesses = out.guesses;
    if (b0.size() >= 4) {
        uint8_t m0 = b0[0], m1 = b0[1], m2 = b0[2], m3 = b0[3];

        // AmigaDOS bootblock
        if (m0 == 'D' && m1 == 'O' && m2 == 'S' && m3 <= 7) {
            guesses.push_back("AmigaDOS bootblock (DOS\\" + std::to_string(m3) + ")");
        } else if (m0 == 'N' && m1 == 'D' && m2 == 'O' && m3 == 'S') {
            guesses.push_back("NDOS marker: intentionally not a DOS disk");
        } else if (m0 == 'D' && m1 == 'O' && m2 == 'S') {
            guesses.push_back("AmigaDOS-style bootblock with unknown flavor " + std::to_string(m3));
        }

        // SFS root block
        if ((m0 == 'S' && m1 == 'F' && m2 == 'S' && m3 == 0) ||
            (m0 == 'S' && m1 == 'F' && m2 == 'S' && m3 == 2)) {
            if (b0.size() >= 14) {
                uint16_t version = (b0[12] << 8) | b0[13];
                guesses.push_back("SFS root block, structure version " + std::to_string(version));
            }
        }

        // CD filesystem markers
        if ((m0 == 'C' && m1 == 'D' && m2 == 'S' && m3 == 'F') ||
            (m0 == 'C' && m1 == 'D' && m2 == '0' && m3 == '1')) {
            guesses.push_back("possible CD filesystem marker");
        }

        // muFS family
        if ((m0 == 'm' && m1 == 'u' && m2 == 'F' && m3 == 'S') ||
            (m0 == 'm' && m1 == 'u' && m2 == 'A' && m3 == 'F') ||
            (m0 == 'm' && m1 == 'u' && m2 == 'P' && m3 == 'F')) {
            guesses.push_back("multiuser filesystem (muFS family) bootblock");
        }
    }

    // PFS rootblock at block 2
    try {
        if (dev->num_blocks() > 2) {
            std::vector<uint8_t> r2(dev->sector_size());
            dev->read(2 * dev->sector_size(), r2);
            if (r2.size() >= 4) {
                uint32_t r2magic = read_be32(r2, 0);
                if (r2magic == 0x50465301 || r2magic == 0x50465302 || r2magic == 0x41465301) {
                    guesses.push_back("PFS rootblock at block 2");
                }
            }
        }
    } catch (...) {}

    // PC-world signatures
    if (b0.size() >= 512 && b0[510] == 0x55 && b0[511] == 0xAA) {
        if (b0.size() >= 59) {
            if (std::memcmp(&b0[54], "FAT12", 5) == 0 || std::memcmp(&b0[54], "FAT16", 5) == 0) {
                guesses.push_back("FAT12/16 boot sector");
            } else if (b0.size() >= 87 && std::memcmp(&b0[82], "FAT32", 5) == 0) {
                guesses.push_back("FAT32 boot sector");
            } else {
                guesses.push_back("PC boot sector / MBR signature (0x55AA)");
            }
        } else {
            guesses.push_back("PC boot sector / MBR signature (0x55AA)");
        }
    }

    // ext2/3/4 superblock at byte offset 1024 (block 2)
    try {
        if (dev->size_bytes() > 1084) {
            std::vector<uint8_t> sb(dev->sector_size());
            dev->read(1024, sb);
            // ext2 magic at offset 56 within the superblock (0x53EF)
            if (sb.size() >= 58 && sb[56] == 0x53 && sb[57] == 0xEF) {
                guesses.push_back("Linux ext2/3/4 superblock");
            }
        }
    } catch (...) {}

    // AmigaDOS root-block probe (valid midpoint structure without bootblock)
    try {
        uint64_t total = dev->num_blocks();
        for (uint32_t spb : {1u, 2u, 4u, 8u}) {
            uint64_t tot = total / spb;
            if (tot < 2) continue;
            uint64_t root = (2 + tot - 1) / 2;
            uint32_t bs = 512 * spb;
            uint64_t byte_off = root * spb * dev->sector_size();
            std::vector<uint8_t> raw(bs);
            dev->read(byte_off, raw);
            if (raw.size() < (size_t)bs) continue;

            uint32_t t0 = read_be32(raw, 0);
            int32_t t_last = (int32_t)read_be32(raw, bs - 4);
            if (t0 == 2 && t_last == 1) {
                // Checksum: sum of all longs should be 0
                uint32_t sum = 0;
                for (size_t o = 0; o + 4 <= (size_t)bs; o += 4) {
                    sum += read_be32(raw, o);
                }
                if (sum == 0) {
                    uint8_t nl = raw[bs - 80];
                    size_t lbl_len = std::min((size_t)nl, (size_t)30);
                    std::string label(raw.begin() + bs - 79, raw.begin() + bs - 79 + lbl_len);
                    guesses.push_back("valid OFS/FFS root block at midpoint (label '" +
                                      label + "', block size " + std::to_string(bs) +
                                      ") -- bootblock may be damaged");
                    break;
                }
            }
        }
    } catch (...) {}

    // Fallback
    if (guesses.empty()) {
        if (b0.size() >= 4 && b0[0] == 0 && b0[1] == 0 && b0[2] == 0 && b0[3] == 0) {
            guesses.push_back("first block is blank: likely unformatted");
        } else {
            guesses.push_back("no known filesystem signature found");
        }
    }

    return out;
}

} // namespace amidisk
