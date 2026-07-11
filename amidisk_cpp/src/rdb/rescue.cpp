#include "rescue.h"

namespace amidisk {

std::string Candidate::describe() const {
    char buf[128];
    std::string exact_str = exact ? "" : "  (size estimated)";
    std::string lbl = label.empty() ? "-" : ("'" + label + "'");
    snprintf(buf, sizeof(buf), "%-5s @sector %-9llu %8.1f MB  %-7s %s%s",
             source.c_str(), (unsigned long long)start_sec, (num_secs * 512) / 1e6,
             dos_type_to_str(dos_type).c_str(), lbl.c_str(), exact_str.c_str());
    return std::string(buf);
}

static bool ffs_root_valid(const std::vector<uint8_t>& raw, uint32_t bs) {
    if (raw.size() < bs) return false;
    uint32_t type = (raw[0] << 24) | (raw[1] << 16) | (raw[2] << 8) | raw[3];
    if (type != 2) return false;
    
    int32_t ext = (raw[bs-4] << 24) | (raw[bs-3] << 16) | (raw[bs-2] << 8) | raw[bs-1];
    if (ext != 1) return false;
    
    uint32_t s = 0;
    for (uint32_t i = 0; i < bs; i += 4) {
        uint32_t v = (raw[i] << 24) | (raw[i+1] << 16) | (raw[i+2] << 8) | raw[i+3];
        s += v;
    }
    return s == 0;
}

std::pair<std::vector<Candidate>, std::vector<std::string>> Rescue::scan(std::shared_ptr<BlockDevice> blkdev, uint64_t rdb_area_secs, std::function<void(uint64_t, uint64_t)> progress) {
    uint64_t total = blkdev->num_blocks();
    if (rdb_area_secs == 0) rdb_area_secs = std::min<uint64_t>(total, 4096);
    
    std::vector<std::string> notes;
    std::vector<std::unique_ptr<PartitionBlock>> parts;
    std::vector<Candidate> anchors;
    std::map<uint64_t, uint32_t> boots;
    
    struct RootNode { uint64_t sec; uint32_t spb; std::string label; };
    std::vector<RootNode> roots;
    
    uint64_t pos = 0;
    const uint64_t CHUNK_SECS = 8192;
    
    auto find_aligned = [](const std::vector<uint8_t>& chunk, const std::vector<uint8_t>& needle, std::function<void(uint32_t)> handler) {
        if (needle.empty() || chunk.size() < needle.size()) return;
        for (size_t i = 0; i <= chunk.size() - needle.size(); i += 512) {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                if (chunk[i+j] != needle[j]) { match = false; break; }
            }
            if (match) handler(i);
        }
    };
    
    std::vector<uint8_t> pat_part = {'P','A','R','T'};
    std::vector<uint8_t> pat_dos = {'D','O','S'};
    std::vector<uint8_t> pat_sfs = {'S','F','S','\0'};
    std::vector<uint8_t> pat_pfs3 = {'P','F','S','\x03'};
    std::vector<uint8_t> pat_afs = {'A','F','S','\x01'};
    std::vector<uint8_t> pat_root = {0,0,0,2,0,0,0,0,0,0,0,0};
    
    while (pos < total) {
        uint64_t n = std::min<uint64_t>(CHUNK_SECS, total - pos);
        std::vector<uint8_t> chunk(n * 512);
        blkdev->read(pos * 512, chunk);
        
        find_aligned(chunk, pat_part, [&](uint32_t off) {
            uint64_t sec = pos + off / 512;
            if (sec < rdb_area_secs) {
                auto pb = std::make_unique<PartitionBlock>(blkdev, sec);
                if (pb->read()) parts.push_back(std::move(pb));
            }
        });
        
        find_aligned(chunk, pat_dos, [&](uint32_t off) {
            uint8_t flavor = chunk[off + 3];
            if (flavor <= 7) boots[pos + off / 512] = 0x444F5300 + flavor;
        });
        
        find_aligned(chunk, pat_sfs, [&](uint32_t off) {
            if (off + 512 > chunk.size()) return;
            uint16_t version = (chunk[off+12] << 8) | chunk[off+13];
            uint32_t ownblock = (chunk[off+8] << 24) | (chunk[off+9] << 16) | (chunk[off+10] << 8) | chunk[off+11];
            if (version == 3 && ownblock == 0) {
                uint32_t totalblocks = (chunk[off+48] << 24) | (chunk[off+49] << 16) | (chunk[off+50] << 8) | chunk[off+51];
                uint32_t blocksize = (chunk[off+52] << 24) | (chunk[off+53] << 16) | (chunk[off+54] << 8) | chunk[off+55];
                if (blocksize >= 512 && blocksize <= 32768 && totalblocks > 0) {
                    uint32_t spb = blocksize / 512;
                    Candidate c;
                    c.source = "sfs";
                    c.start_sec = pos + off / 512;
                    c.num_secs = totalblocks * spb;
                    c.dos_type = 0x53465300;
                    c.sec_per_blk = spb;
                    c.exact = true;
                    anchors.push_back(std::move(c));
                }
            }
        });
        
        auto on_pfs = [&](uint32_t off) {
            if (off + 512 > chunk.size()) return;
            uint32_t disksize = (chunk[off+84] << 24) | (chunk[off+85] << 16) | (chunk[off+86] << 8) | chunk[off+87];
            uint32_t options = (chunk[off+4] << 24) | (chunk[off+5] << 16) | (chunk[off+6] << 8) | chunk[off+7];
            uint64_t sec = pos + off / 512;
            if (disksize > 0 && (options & 1) && sec >= 2) {
                uint8_t nlen = chunk[off+20];
                std::string label((const char*)&chunk[off+21], std::min<uint8_t>(nlen, 31));
                Candidate c;
                c.source = "pfs3";
                c.start_sec = sec - 2;
                c.num_secs = disksize;
                c.dos_type = 0x50465303; // usually 03 for pfs3, we can just use 0x50465303
                c.sec_per_blk = 1; // wait, PFS3 doesn't rely on spb like this for geometry but default to 1
                c.label = label;
                c.exact = true;
                anchors.push_back(std::move(c));
            }
        };
        
        find_aligned(chunk, pat_pfs3, on_pfs);
        find_aligned(chunk, pat_afs, on_pfs);
        
        find_aligned(chunk, pat_root, [&](uint32_t off) {
            uint64_t sec = pos + off / 512;
            for (uint32_t spb : {1, 2, 4, 8}) {
                if (sec + spb > total) break;
                std::vector<uint8_t> raw(spb * 512);
                blkdev->read(sec * 512, raw);
                if (ffs_root_valid(raw, 512 * spb)) {
                    uint32_t bs = 512 * spb;
                    uint8_t nlen = raw[bs - 80];
                    std::string label((const char*)&raw[bs - 79], std::min<uint8_t>(nlen, 30));
                    roots.push_back({sec, spb, label});
                    break;
                }
            }
        });
        
        pos += n;
        if (progress) progress(pos, total);
    }
    
    std::vector<Candidate> ffs;
    std::vector<uint64_t> boot_list;
    for (auto const& [k, v] : boots) boot_list.push_back(k);
    
    for (size_t i = 0; i < boot_list.size(); ++i) {
        uint64_t bsec = boot_list[i];
        uint32_t flavor = boots[bsec];
        uint64_t nxt = (i + 1 < boot_list.size()) ? boot_list[i+1] : total;
        
        for (const auto& r : roots) {
            if (r.sec > bsec && r.sec < nxt) {
                uint64_t root_idx = (r.sec - bsec) / r.spb;
                if (root_idx < 2 || (r.sec - bsec) % r.spb != 0) continue;
                
                uint64_t tot = 2 * root_idx - 1;
                Candidate c;
                c.source = "ffs";
                c.start_sec = bsec;
                c.num_secs = tot * r.spb;
                c.dos_type = flavor;
                c.label = r.label;
                c.sec_per_blk = r.spb;
                c.exact = false;
                ffs.push_back(std::move(c));
                break;
            }
        }
    }
    
    std::vector<Candidate> cands;
    for (auto& pb : parts) {
        Candidate c;
        c.source = "part";
        c.start_sec = pb->envec.start_sec();
        c.num_secs = pb->envec.num_secs();
        c.dos_type = pb->envec.dos_type;
        c.label = pb->drive_name;
        c.sec_per_blk = std::max<uint32_t>(pb->envec.sec_per_blk, 1);
        c.part_blk = std::move(pb);
        c.exact = true;
        cands.push_back(std::move(c));
    }
    
    for (auto& a : anchors) {
        bool overlap = false;
        for (const auto& c : cands) {
            if (a.start_sec >= c.start_sec && a.start_sec < c.start_sec + c.num_secs) {
                overlap = true; break;
            }
        }
        if (!overlap) cands.push_back(std::move(a));
    }
    for (auto& a : ffs) {
        bool overlap = false;
        for (const auto& c : cands) {
            if (a.start_sec >= c.start_sec && a.start_sec < c.start_sec + c.num_secs) {
                overlap = true; break;
            }
        }
        if (!overlap) cands.push_back(std::move(a));
    }
    
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        return a.start_sec < b.start_sec;
    });
    
    std::vector<Candidate> pruned;
    for (auto& c : cands) {
        if (!pruned.empty() && c.start_sec < pruned.back().start_sec + pruned.back().num_secs) {
            auto& prev = pruned.back();
            if (c.exact && !prev.exact) {
                pruned.back() = std::move(c);
            } else {
                notes.push_back("dropped overlapping candidate: " + c.describe());
            }
            continue;
        }
        pruned.push_back(std::move(c));
    }
    
    if (pruned.empty()) {
        notes.push_back("no partition evidence found");
    }
    
    return {std::move(pruned), notes};
}

static std::pair<uint32_t, uint32_t> pick_geometry(const std::vector<Candidate>& candidates, uint64_t total_secs) {
    std::vector<std::pair<uint32_t, uint32_t>> options;
    uint32_t heads_arr[] = {16, 8, 4, 2, 1, 32, 64, 128, 255};
    uint32_t sectors_arr[] = {63, 32, 127, 126, 16};
    
    for (uint32_t heads : heads_arr) {
        for (uint32_t sectors : sectors_arr) {
            uint32_t cyl = heads * sectors;
            if (total_secs / cyl > 65535 || total_secs / cyl < 3) continue;
            
            bool ok = true;
            for (const auto& c : candidates) {
                if (c.start_sec % cyl != 0) { ok = false; break; }
            }
            if (ok) options.push_back({heads, sectors});
        }
    }
    if (!options.empty()) return options[0];
    return {1, 1};
}

std::unique_ptr<RDisk> Rescue::rebuild(std::shared_ptr<BlockDevice> blkdev, const std::vector<Candidate>& candidates, const std::string& drv_prefix) {
    if (blkdev->is_read_only()) throw std::runtime_error("device is read-only");
    if (candidates.empty()) throw std::runtime_error("nothing to rebuild");
    
    bool reuse = true;
    for (const auto& c : candidates) {
        if (!c.part_blk) { reuse = false; break; }
    }
    
    uint32_t heads, sectors;
    if (reuse) {
        heads = candidates[0].part_blk->envec.surfaces;
        sectors = candidates[0].part_blk->envec.blk_per_trk;
    } else {
        auto geom = pick_geometry(candidates, blkdev->num_blocks());
        heads = geom.first;
        sectors = geom.second;
    }
    
    uint64_t first_start = candidates[0].start_sec;
    for (const auto& c : candidates) {
        if (c.start_sec < first_start) first_start = c.start_sec;
    }
    
    uint32_t cyl = heads * sectors;
    uint32_t rdb_cyls = std::max<uint32_t>(1, first_start / cyl);
    if (first_start >= cyl) rdb_cyls = std::min<uint32_t>(rdb_cyls, 2);
    else rdb_cyls = 1;
    
    auto rd = RDisk::create(blkdev, sectors, heads, rdb_cyls);
    
    if (reuse) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            auto& pb = candidates[i].part_blk;
            pb->next = (i + 1 < candidates.size()) ? candidates[i+1].part_blk->blk_num() : 0xFFFFFFFF;
            pb->write();
            if (pb->blk_num() > rd->rdsk_blk().high_rdsk_block) 
                const_cast<RDBlock&>(rd->rdsk_blk()).high_rdsk_block = pb->blk_num();
        }
        const_cast<RDBlock&>(rd->rdsk_blk()).partition_list = candidates[0].part_blk->blk_num();
        const_cast<RDBlock&>(rd->rdsk_blk()).write();
        
        // RDisk::create already sets up the structures, but since we manually linked the partition list
        // we might want to reload it, but just returning `rd` is fine if the user calls `RDisk::open` next.
        // Actually we can just reload it into a fresh instance.
        return RDisk::open(blkdev);
    }
    
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        uint32_t low = c.start_sec / cyl;
        uint32_t high = ((c.start_sec + c.num_secs) + cyl - 1) / cyl - 1;
        if (high > rd->rdsk_blk().hi_cylinder) high = rd->rdsk_blk().hi_cylinder;
        
        if (i + 1 < candidates.size()) {
            uint32_t next_low = candidates[i+1].start_sec / cyl;
            if (next_low > 0 && high > next_low - 1) {
                high = next_low - 1;
            }
        }
        
        std::string name = (c.source == "part" && !c.label.empty()) ? c.label : (drv_prefix + std::to_string(i));
        rd->add_partition_exact(name, low, high, c.dos_type, c.sec_per_blk);
    }
    
    return rd;
}

} // namespace amidisk
