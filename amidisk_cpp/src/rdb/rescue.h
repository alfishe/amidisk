#pragma once

#include "rdisk.h"
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <string>

namespace amidisk {

struct Candidate {
    uint32_t dos_type;
    uint32_t start_sec;
    uint32_t num_secs;
    uint32_t score;
    bool bootable;
    int32_t boot_pri;
    std::string source;
    std::string label;
    uint32_t sec_per_blk = 1;
    std::unique_ptr<PartitionBlock> part_blk;
    bool exact = true;

    std::string describe() const;
};

class Rescue {
public:
    static std::pair<std::vector<Candidate>, std::vector<std::string>> scan(std::shared_ptr<BlockDevice> blkdev, uint64_t rdb_area_secs = 0, std::function<void(uint64_t, uint64_t)> progress = nullptr);
    static std::unique_ptr<RDisk> rebuild(std::shared_ptr<BlockDevice> blkdev, const std::vector<Candidate>& cands, const std::string& drv_prefix = "DH");
};

} // namespace amidisk
