#include "safety.h"
#include "cli_util.h"
#include "fs/volume.h"
#include <iostream>
#include <fstream>

namespace amidisk {

std::pair<std::vector<std::string>, bool>
volume_report(DiskImage& img, bool list_root, bool deep) {
    std::vector<std::string> lines;
    bool all_ok = true;

    for (const auto& v : img.volumes()) {
        Volume* vol = nullptr;
        try {
            vol = v->mount();
        } catch (const std::exception& ex) {
            lines.push_back("  " + v->name() + ":  UNMOUNTABLE: " + ex.what());
            all_ok = false;
            continue;
        }

        VolumeInfo info;
        try {
            info = vol->get_info();
        } catch (const std::exception& ex) {
            lines.push_back("  " + v->name() + ":  get_info failed: " + ex.what());
            all_ok = false;
            continue;
        }

        uint64_t total_b = info.total_blocks * info.block_size;
        uint64_t used_b = info.used_blocks * info.block_size;
        auto rep = vol->check(deep);

        std::string ok_str = rep.ok ? "check OK" :
            "CHECK FAILED (" + std::to_string(rep.errors.size()) + " errors)";
        if (!rep.ok) all_ok = false;

        char buf[512];
        snprintf(buf, sizeof(buf),
                 "  %-8s %-16s %-7s %9s used / %9s free of %9s -- %u files, %u dirs -- %s",
                 (v->name() + ":").c_str(),
                 info.label.c_str(),
                 info.dos_type.c_str(),
                 human_size(used_b).c_str(),
                 human_size(info.free_bytes).c_str(),
                 human_size(total_b).c_str(),
                 rep.files, rep.dirs, ok_str.c_str());
        lines.push_back(buf);

        for (size_t i = 0; i < rep.errors.size() && i < 3; ++i) {
            lines.push_back("      error: " + rep.errors[i]);
        }

        if (list_root) {
            try {
                auto entries = vol->list_dir("");
                std::string sample;
                for (size_t i = 0; i < entries.size() && i < 10; ++i) {
                    if (i) sample += " ";
                    sample += entries[i].name_str();
                    if (entries[i].is_dir()) sample += "/";
                }
                if (entries.size() > 10) sample += " ...";
                lines.push_back("      root (" + std::to_string(entries.size()) +
                                " entries): " + sample);
            } catch (const std::exception& ex) {
                lines.push_back("      root unreadable: " + std::string(ex.what()));
                all_ok = false;
            }
        }
    }

    return {lines, all_ok};
}

bool commit_overlay(OverlayBlockDevice& overlay, const std::string& path,
                    const std::string& backup_path) {
    constexpr uint32_t BACKUP_SECS = 2048;

    auto target = open_blkdev(path, false);
    try {
        // Optional RDB-area backup
        if (!backup_path.empty()) {
            uint32_t area = std::min<uint64_t>(target->num_blocks(), BACKUP_SECS);
            std::vector<uint8_t> backup_data(area * target->sector_size());
            target->read(0, backup_data);
            std::ofstream f(backup_path, std::ios::binary);
            if (f.is_open()) {
                f.write(reinterpret_cast<const char*>(backup_data.data()), backup_data.size());
                std::cout << "backup of first " << area << " sectors saved to "
                          << backup_path << std::endl;
            }
        }

        // Commit dirty blocks
        overlay.commit(*target);

        // Read-back verification
        auto bad = overlay.verify_committed(*target);
        if (!bad.empty()) {
            std::cerr << "error: read-back verification FAILED on "
                      << bad.size() << " blocks!" << std::endl;
            target->close();
            return false;
        }

        auto dirty = overlay.dirty_blocks();
        std::cout << "committed " << human_size(overlay.dirty_bytes())
                  << " in " << dirty.size()
                  << " changed blocks, read-back verified" << std::endl;

        target->flush();
        target->close();
        return true;
    } catch (...) {
        target->close();
        throw;
    }
}

} // namespace amidisk
