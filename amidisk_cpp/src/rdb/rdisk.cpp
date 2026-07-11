#include "rdisk.h"
#include "../blkdev/part_blkdev.h"
#include <fstream>

namespace amidisk {

Partition::Partition(std::shared_ptr<BlockDevice> blkdev, std::unique_ptr<PartitionBlock> part_blk, uint32_t num)
    : blkdev_(blkdev), part_blk_(std::move(part_blk)), num_(num) {
    drv_name_ = part_blk_->drive_name;
    dos_env_ = part_blk_->envec;
}

uint64_t Partition::get_num_blocks() const {
    uint32_t spb = std::max<uint32_t>(dos_env_.sec_per_blk, 1);
    return dos_env_.num_secs() / spb;
}

uint32_t Partition::get_block_size() const {
    uint32_t spb = std::max<uint32_t>(dos_env_.sec_per_blk, 1);
    return dos_env_.size_block * 4 * spb;
}

uint64_t Partition::get_byte_offset() const {
    return dos_env_.start_sec() * blkdev_->sector_size();
}

uint64_t Partition::get_byte_size() const {
    return dos_env_.num_secs() * blkdev_->sector_size();
}

bool Partition::is_bootable() const {
    return (part_blk_->flags & 1) != 0; // flag 1 is bootable in AmigaOS
}

std::shared_ptr<BlockDevice> Partition::create_blkdev() const {
    return std::make_shared<PartBlockDevice>(blkdev_, dos_env_.start_sec(), dos_env_.num_secs(), blkdev_->sector_size());
}

nlohmann::json Partition::get_info() const {
    nlohmann::json info = {
        {"num", num_},
        {"drv_name", drv_name_},
        {"dos_type", get_dos_type_str()},
        {"low_cyl", dos_env_.low_cyl},
        {"high_cyl", dos_env_.high_cyl},
        {"surfaces", dos_env_.surfaces},
        {"blk_per_trk", dos_env_.blk_per_trk},
        {"sec_per_blk", std::max<uint32_t>(dos_env_.sec_per_blk, 1)},
        {"block_size", get_block_size()},
        {"reserved", dos_env_.reserved},
        {"size_bytes", get_byte_size()},
        {"bootable", is_bootable()},
        {"boot_pri", dos_env_.boot_pri},
        {"max_transfer", dos_env_.max_transfer},
        {"mask", dos_env_.mask},
        {"num_buffer", dos_env_.num_buffer}
    };
    return info;
}

FileSystem::FileSystem(std::shared_ptr<BlockDevice> blkdev, std::unique_ptr<FSHeaderBlock> fshd_blk, uint32_t num)
    : blkdev_(blkdev), fshd_blk_(std::move(fshd_blk)), num_(num) {}

std::string FileSystem::get_dos_type_str() const {
    return dos_type_to_str(fshd_blk_->type);
}

std::string FileSystem::get_version_string() const {
    return std::to_string(fshd_blk_->version) + "." + std::to_string(fshd_blk_->revision);
}

std::vector<uint8_t> FileSystem::get_data() const {
    std::vector<uint8_t> data;
    int32_t blk_num = fshd_blk_->seg_list_blk;
    int count = 0;
    while (blk_num != 0 && blk_num != -1) {
        if (++count > 1024) throw RDiskError("Cyclic LSEG chain");
        LoadSegBlock lseg(blkdev_, blk_num);
        if (!lseg.read()) throw RDiskError("Invalid LSEG block");
        data.insert(data.end(), lseg.load_data.begin(), lseg.load_data.end());
        blk_num = lseg.next;
    }
    return data;
}

nlohmann::json FileSystem::get_info() const {
    nlohmann::json info = {
        {"num", num_},
        {"dos_type", get_dos_type_str()},
        {"version", get_version_string()},
        {"size", get_data().size()}
    };
    return info;
}

// RDisk
std::unique_ptr<RDisk> RDisk::peek(std::shared_ptr<BlockDevice> blkdev) {
    for (uint64_t i = 0; i < 16; ++i) {
        auto rdsk = std::make_unique<RDBlock>(blkdev, i);
        if (rdsk->read()) {
            return std::make_unique<RDisk>(blkdev, std::move(rdsk));
        }
    }
    return nullptr;
}

std::unique_ptr<RDisk> RDisk::open(std::shared_ptr<BlockDevice> blkdev) {
    auto rdisk = peek(blkdev);
    if (!rdisk) throw RDiskError("No RDB found on device");
    return rdisk;
}

std::unique_ptr<RDisk> RDisk::create(std::shared_ptr<BlockDevice> blkdev, uint32_t sectors, uint32_t heads, uint32_t rdb_cyls) {
    if (heads == 0) {
        uint32_t candidate_heads[] = {1, 2, 4, 8, 16, 32, 64, 128, 255};
        for (uint32_t h : candidate_heads) {
            heads = h;
            if (blkdev->num_blocks() / (h * sectors) <= 65535) break;
        }
    }
    
    uint32_t cyl_blocks = heads * sectors;
    uint32_t cyls = blkdev->num_blocks() / cyl_blocks;
    
    if (rdb_cyls == 0) {
        uint32_t want_secs = std::min<uint32_t>(512, std::max<uint32_t>(64, blkdev->num_blocks() / 20));
        rdb_cyls = std::max<uint32_t>(2, (want_secs + cyl_blocks - 1) / cyl_blocks);
    }
    
    if (cyls <= rdb_cyls) throw RDiskError("image too small for an RDB");
    
    auto rdsk = RDBlock::create(blkdev, 0);
    rdsk->host_id = 7;
    rdsk->block_bytes = blkdev->sector_size();
    rdsk->flags = 7;
    rdsk->cylinders = cyls;
    rdsk->sectors = sectors;
    rdsk->heads = heads;
    rdsk->interleave = 1;
    rdsk->park = cyls;
    rdsk->write_precomp = cyls;
    rdsk->reduced_write = cyls;
    rdsk->step_rate = 3;
    rdsk->rdb_blocks_lo = 0;
    rdsk->rdb_blocks_hi = std::max<uint32_t>(0, rdb_cyls * cyl_blocks - 1);
    rdsk->lo_cylinder = rdb_cyls;
    rdsk->hi_cylinder = cyls - 1;
    rdsk->cyl_blocks = cyl_blocks;
    rdsk->auto_park_seconds = 0;
    rdsk->high_rdsk_block = 63;
    
    rdsk->disk_vendor = "AMIDISK ";
    rdsk->disk_product = "VIRTUAL DRIVE   ";
    rdsk->disk_revision = "1.0 ";
    rdsk->write();
    
    return std::make_unique<RDisk>(blkdev, std::move(rdsk));
}

RDisk::RDisk(std::shared_ptr<BlockDevice> blkdev, std::unique_ptr<RDBlock> rdsk)
    : blkdev_(blkdev), rdsk_(std::move(rdsk)) {
    scan();
}

void RDisk::scan() {
    uint32_t pblk = rdsk_->partition_list;
    int count = 0;
    while (pblk != 0xFFFFFFFF && pblk != 0) {
        if (++count > 128) throw RDiskError("Too many partitions");
        auto part = std::make_unique<PartitionBlock>(blkdev_, pblk);
        if (!part->read()) throw RDiskError("Failed to read PART block");
        pblk = part->next;
        partitions_.push_back(std::make_unique<Partition>(blkdev_, std::move(part), partitions_.size()));
    }
    
    uint32_t fblk = rdsk_->fs_header_list;
    count = 0;
    while (fblk != 0xFFFFFFFF && fblk != 0) {
        if (++count > 128) throw RDiskError("Too many filesystems");
        auto fs = std::make_unique<FSHeaderBlock>(blkdev_, fblk);
        if (!fs->read()) throw RDiskError("Failed to read FSHD block");
        fblk = fs->next;
        filesystems_.push_back(std::make_unique<FileSystem>(blkdev_, std::move(fs), filesystems_.size()));
    }
}

uint64_t RDisk::allocate_block() {
    uint32_t high = rdsk_->high_rdsk_block;
    if (high < rdsk_->blk_num()) high = 63; // Default
    
    std::vector<bool> used(high + 1, false);
    if (rdsk_->blk_num() <= high) used[rdsk_->blk_num()] = true;
    for (const auto& p : partitions_) {
        if (p->part_blk().blk_num() <= high) used[p->part_blk().blk_num()] = true;
    }
    for (const auto& f : filesystems_) {
        if (f->fshd_blk().blk_num() <= high) used[f->fshd_blk().blk_num()] = true;
        int32_t blk_num = f->fshd_blk().seg_list_blk;
        while (blk_num != 0 && blk_num != -1) {
            if (blk_num <= high) used[blk_num] = true;
            LoadSegBlock lseg(blkdev_, blk_num);
            if (!lseg.read()) break;
            blk_num = lseg.next;
        }
    }
    
    for (uint32_t i = rdsk_->rdb_blocks_lo; i <= high; ++i) {
        if (!used[i]) return i;
    }
    throw RDiskError("RDB area full");
}

Partition* RDisk::add_partition(const std::string& drv_name, uint64_t size_bytes, uint32_t dos_type, uint32_t sec_per_blk, bool bootable, int32_t boot_pri) {
    for (const auto& p : partitions_) {
        if (p->drv_name() == drv_name) throw RDiskError("partition " + drv_name + " already exists");
    }
    
    uint64_t cyl_bytes = static_cast<uint64_t>(rdsk_->cyl_blocks) * blkdev_->sector_size();
    uint32_t num_cyls = 0;
    uint32_t lo = 0, hi = 0;
    
    if (size_bytes == 0) {
        // Find largest free block
        uint32_t best = 0;
        uint32_t cur = rdsk_->lo_cylinder;
        
        std::vector<std::pair<uint32_t, uint32_t>> taken;
        for (const auto& p : partitions_) {
            taken.push_back({p->dos_env().low_cyl, p->dos_env().high_cyl});
        }
        std::sort(taken.begin(), taken.end());
        taken.push_back({rdsk_->hi_cylinder + 1, 0});
        
        for (const auto& [tlo, thi] : taken) {
            if (tlo > cur && (tlo - cur) > best) {
                best = tlo - cur;
                lo = cur;
                hi = tlo - 1;
            }
            cur = std::max(cur, thi + 1);
        }
        if (best == 0) throw RDiskError("no free space left");
    } else {
        num_cyls = (size_bytes + cyl_bytes - 1) / cyl_bytes;
        
        std::vector<std::pair<uint32_t, uint32_t>> taken;
        for (const auto& p : partitions_) {
            taken.push_back({p->dos_env().low_cyl, p->dos_env().high_cyl});
        }
        std::sort(taken.begin(), taken.end());
        
        uint32_t cur = rdsk_->lo_cylinder;
        bool found = false;
        for (const auto& [tlo, thi] : taken) {
            if (tlo >= cur && (tlo - cur) >= num_cyls) {
                lo = cur;
                hi = cur + num_cyls - 1;
                found = true;
                break;
            }
            cur = std::max(cur, thi + 1);
        }
        if (!found) {
            if (rdsk_->hi_cylinder >= cur && (rdsk_->hi_cylinder - cur + 1) >= num_cyls) {
                lo = cur;
                hi = cur + num_cyls - 1;
                found = true;
            }
        }
        if (!found) throw RDiskError("no free space for " + std::to_string(num_cyls) + " cylinders");
    }
    
    uint64_t blk_num = allocate_block();
    auto pb = PartitionBlock::create(blkdev_, blk_num);
    pb->host_id = rdsk_->host_id;
    pb->next = 0xFFFFFFFF;
    pb->flags = bootable ? 1 : 0;
    pb->drive_name = drv_name;
    
    DosEnvec& env = pb->envec;
    env.table_size = 16;
    env.size_block = blkdev_->sector_size() / 4;
    env.surfaces = rdsk_->heads;
    env.sec_per_blk = sec_per_blk;
    env.blk_per_trk = rdsk_->sectors;
    env.reserved = 2;
    env.low_cyl = lo;
    env.high_cyl = hi;
    env.num_buffer = 30;
    env.max_transfer = 0x1FE00;
    env.mask = 0x7FFFFFFE;
    env.boot_pri = boot_pri;
    env.dos_type = dos_type;
    
    pb->write();
    
    if (!partitions_.empty()) {
        auto& last_pb = const_cast<PartitionBlock&>(partitions_.back()->part_blk());
        last_pb.next = blk_num;
        last_pb.write();
    } else {
        rdsk_->partition_list = blk_num;
        rdsk_->write();
    }
    
    partitions_.push_back(std::make_unique<Partition>(blkdev_, std::move(pb), partitions_.size()));
    return partitions_.back().get();
}

Partition* RDisk::add_partition_exact(const std::string& drv_name, uint32_t low_cyl, uint32_t high_cyl, uint32_t dos_type, uint32_t sec_per_blk, bool bootable, int32_t boot_pri) {
    uint64_t blk_num = allocate_block();
    auto pb = PartitionBlock::create(blkdev_, blk_num);
    pb->host_id = rdsk_->host_id;
    pb->drive_name = drv_name;
    pb->flags = bootable ? 1 : 0;
    
    DosEnvec& env = pb->envec;
    env.table_size = 16;
    env.size_block = blkdev_->sector_size() / 4;
    env.surfaces = rdsk_->heads;
    env.sec_per_blk = sec_per_blk;
    env.blk_per_trk = rdsk_->sectors;
    env.reserved = 2;
    env.low_cyl = low_cyl;
    env.high_cyl = high_cyl;
    env.num_buffer = 30;
    env.max_transfer = 0x1FE00;
    env.mask = 0x7FFFFFFE;
    env.boot_pri = boot_pri;
    env.dos_type = dos_type;
    
    pb->write();
    
    if (!partitions_.empty()) {
        auto& last_pb = const_cast<PartitionBlock&>(partitions_.back()->part_blk());
        last_pb.next = blk_num;
        last_pb.write();
    } else {
        rdsk_->partition_list = blk_num;
        rdsk_->write();
    }
    
    partitions_.push_back(std::make_unique<Partition>(blkdev_, std::move(pb), partitions_.size()));
    return partitions_.back().get();
}

void RDisk::delete_partition(const std::string& drv_name) {
    auto it = std::find_if(partitions_.begin(), partitions_.end(),
        [&drv_name](const auto& p) { return p->drv_name() == drv_name; });
    if (it == partitions_.end()) throw RDiskError("partition " + drv_name + " not found");
    
    uint32_t blk_num = (*it)->part_blk().blk_num();
    uint32_t next = (*it)->part_blk().next;
    
    if (it == partitions_.begin()) {
        rdsk_->partition_list = next;
        rdsk_->write();
    } else {
        auto& prev = const_cast<PartitionBlock&>((*(it - 1))->part_blk());
        prev.next = next;
        prev.write();
    }
    
    partitions_.erase(it);
}

void RDisk::add_filesystem(const std::string& path, uint32_t dos_type, uint16_t version, uint16_t revision) {
    for (const auto& f : filesystems_) {
        if (f->fshd_blk().type == dos_type) {
            throw RDiskError("filesystem for " + f->get_dos_type_str() + " already embedded");
        }
    }
    
    std::ifstream file(path, std::ios::binary);
    if (!file) throw RDiskError("failed to open filesystem file: " + path);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    uint32_t bb = blkdev_->sector_size();
    uint32_t per = bb - 20; // 5 longs for header
    uint32_t nseg = (data.size() + per - 1) / per;
    
    std::vector<uint64_t> free_blks;
    uint32_t high = rdsk_->high_rdsk_block;
    if (high < rdsk_->blk_num()) high = 63;
    
    std::vector<bool> used(high + 1, false);
    if (rdsk_->blk_num() <= high) used[rdsk_->blk_num()] = true;
    for (const auto& p : partitions_) {
        if (p->part_blk().blk_num() <= high) used[p->part_blk().blk_num()] = true;
    }
    for (const auto& f : filesystems_) {
        if (f->fshd_blk().blk_num() <= high) used[f->fshd_blk().blk_num()] = true;
        int32_t blk_num = f->fshd_blk().seg_list_blk;
        while (blk_num != 0 && blk_num != -1) {
            if (blk_num <= high) used[blk_num] = true;
            LoadSegBlock lseg(blkdev_, blk_num);
            if (!lseg.read()) break;
            blk_num = lseg.next;
        }
    }
    
    for (uint32_t i = rdsk_->rdb_blocks_lo; i <= high; ++i) {
        if (!used[i]) free_blks.push_back(i);
        if (free_blks.size() >= nseg + 1) break;
    }
    
    if (free_blks.size() < nseg + 1) throw RDiskError("RDB area too small: need " + std::to_string(nseg + 1) + " blocks");
    
    uint64_t fshd_blk_num = free_blks[0];
    
    for (uint32_t i = 0; i < nseg; ++i) {
        uint32_t chunk_size = std::min<uint32_t>(per, data.size() - i * per);
        auto lseg = LoadSegBlock::create(blkdev_, free_blks[i + 1]);
        lseg->host_id = rdsk_->host_id;
        lseg->next = (i + 1 < nseg) ? free_blks[i + 2] : 0xFFFFFFFF;
        lseg->load_data.assign(data.begin() + i * per, data.begin() + i * per + chunk_size);
        
        while (lseg->load_data.size() % 4 != 0) lseg->load_data.push_back(0);
        lseg->write();
    }
    
    auto fshd = FSHeaderBlock::create(blkdev_, fshd_blk_num);
    fshd->host_id = rdsk_->host_id;
    fshd->next = 0xFFFFFFFF;
    fshd->version = version;
    fshd->revision = revision;
    fshd->patch_flags = 0x180;
    fshd->type = dos_type;
    fshd->seg_list_blk = free_blks[1];
    fshd->write();
    
    if (!filesystems_.empty()) {
        auto& last_fs = const_cast<FSHeaderBlock&>(filesystems_.back()->fshd_blk());
        last_fs.next = fshd_blk_num;
        last_fs.write();
    } else {
        rdsk_->fs_header_list = fshd_blk_num;
        rdsk_->write();
    }
    
    filesystems_.push_back(std::make_unique<FileSystem>(blkdev_, std::move(fshd), filesystems_.size()));
}

void RDisk::extract_filesystem(uint32_t dos_type, const std::string& out_path) const {
    for (const auto& f : filesystems_) {
        if (f->fshd_blk().type == dos_type) {
            auto data = f->get_data();
            std::ofstream file(out_path, std::ios::binary);
            if (!file) throw RDiskError("failed to open output file: " + out_path);
            file.write(reinterpret_cast<const char*>(data.data()), data.size());
            return;
        }
    }
    throw RDiskError("filesystem not found");
}

} // namespace amidisk
