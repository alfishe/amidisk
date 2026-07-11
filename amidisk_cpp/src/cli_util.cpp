#include "cli_util.h"
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>

namespace amidisk {

std::string human_size(uint64_t n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double dn = static_cast<double>(n);
    int i = 0;
    while (dn >= 1024.0 && i < 4) {
        dn /= 1024.0;
        i++;
    }
    char buf[64];
    if (i == 0) {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", dn, units[i]);
    }
    return std::string(buf);
}

uint64_t parse_size(const std::string& text) {
    std::string t = text;
    // Trim whitespace
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(0, 1);

    // Lowercase
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    uint64_t mult = 1;
    if (!t.empty()) {
        char suffix = t.back();
        if (suffix == 'k') { mult = 1024ULL; t.pop_back(); }
        else if (suffix == 'm') { mult = 1024ULL * 1024; t.pop_back(); }
        else if (suffix == 'g') { mult = 1024ULL * 1024 * 1024; t.pop_back(); }
    }

    double val = std::stod(t);
    return static_cast<uint64_t>(val * mult);
}

const std::map<std::string, uint32_t>& dos_types() {
    static const std::map<std::string, uint32_t> types = {
        {"ofs", 0x444F5300}, {"dos0", 0x444F5300},
        {"ffs", 0x444F5301}, {"dos1", 0x444F5301},
        {"ofs-intl", 0x444F5302}, {"dos2", 0x444F5302},
        {"ffs-intl", 0x444F5303}, {"dos3", 0x444F5303},
        {"ofs-dc", 0x444F5304}, {"dos4", 0x444F5304},
        {"ffs-dc", 0x444F5305}, {"dos5", 0x444F5305},
        {"ofs-intl-lnfs", 0x444F5306}, {"dos6", 0x444F5306},
        {"ffs-intl-lnfs", 0x444F5307}, {"dos7", 0x444F5307},
        {"sfs", 0x53465300}, {"sfs0", 0x53465300},
        {"sfs2", 0x53465302},
        {"pfs0", 0x50465300},
        {"pfs1", 0x50465301},
        {"pfs2", 0x50465302},
        {"pfs3", 0x50465303}, {"pds3", 0x50445303},
        {"pfs3-modern", 0x50465333},
        {"jxfs", 0x4A584604},
        {"swap", 0x53574150},
    };
    return types;
}

uint32_t parse_dostype(const std::string& s) {
    // Try named dostype (case-insensitive)
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const auto& types = dos_types();
    auto it = types.find(lower);
    if (it != types.end()) return it->second;

    // Try hex (with or without 0x prefix)
    try {
        if (lower.size() > 2 && lower[0] == '0' && lower[1] == 'x') {
            return std::stoul(lower.substr(2), nullptr, 16);
        }
        return std::stoul(lower, nullptr, 16);
    } catch (...) {
        return 0;
    }
}

std::string truncate_name(const std::string& name, uint32_t max_len) {
    if (name.length() <= max_len) return name;

    // Split into base and extension
    auto dot_pos = name.find_last_of('.');
    std::string base, ext;
    if (dot_pos != std::string::npos && dot_pos > 0) {
        base = name.substr(0, dot_pos);
        ext = name.substr(dot_pos);
    } else {
        base = name;
    }

    if (ext.length() >= max_len) {
        return name.substr(0, max_len);
    }

    uint32_t allowed_base_len = max_len - ext.length() - 3;  // 3 for "..."
    if (allowed_base_len < 2) {
        return name.substr(0, max_len);
    }

    uint32_t left = allowed_base_len / 2 + (allowed_base_len % 2);
    uint32_t right = allowed_base_len / 2;
    return base.substr(0, left) + "..." + base.substr(base.length() - right) + ext;
}

// --- Progress bar state ---
static uint64_t g_last_bytes = 0;
static double g_last_time = 0;

static double now_seconds() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

void print_progress(uint64_t current_bytes, uint64_t total_bytes, const std::string& current_file) {
    uint64_t step = std::max<uint64_t>(262144, total_bytes / 500);

    if (current_bytes < total_bytes && (current_bytes - g_last_bytes < step)) return;

    double now = now_seconds();
    if (current_bytes < total_bytes && (now - g_last_time < 0.20)) return;

    g_last_time = now;
    g_last_bytes = current_bytes;

    double percent = total_bytes > 0 ? (static_cast<double>(current_bytes) / total_bytes) * 100.0 : 0;
    int bar_len = 30;
    int filled = percent <= 100 ? static_cast<int>(percent / 100 * bar_len) : bar_len;

    std::string bar(filled, '#');
    bar += std::string(bar_len - filled, '-');

    std::string file_display = current_file.substr(0, std::min((size_t)40, current_file.length()));

    printf("\x1b[2K\r[%s] %5.1f%% (%s / %s) %s",
           bar.c_str(), percent,
           human_size(current_bytes).c_str(),
           human_size(total_bytes).c_str(),
           file_display.c_str());
    fflush(stdout);
}

void print_transfer_stats(uint32_t file_count, uint32_t dir_count, uint64_t total_bytes, double elapsed) {
    printf("\x1b[2K\r");
    if (elapsed <= 0) elapsed = 0.001;
    double speed = (total_bytes / 1024.0 / 1024.0) / elapsed;
    printf("Transferred: %u files, %u dirs\n", file_count, dir_count);
    printf("Total size:  %s\n", human_size(total_bytes).c_str());
    printf("Time taken:  %.2f seconds\n", elapsed);
    printf("Avg speed:   %.2f MB/s\n", speed);
}

} // namespace amidisk
