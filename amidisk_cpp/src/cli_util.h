#pragma once

#include <string>
#include <cstdint>
#include <map>

namespace amidisk {

// human_size — format bytes as human-readable (B, KB, MB, GB, TB)
std::string human_size(uint64_t n);

// parse_size — parse a size string with K/M/G suffixes (e.g. "10M", "512K", "2G")
uint64_t parse_size(const std::string& text);

// DOS_TYPES — accepted dostype name → 32-bit value
const std::map<std::string, uint32_t>& dos_types();

// parse_dostype — accept named dostype (ffs, pfs3, sfs, etc.) or hex
// Returns 0 on unknown
uint32_t parse_dostype(const std::string& s);

// truncate_name — truncate a filename to max_len, preserving extension with "..." notation
std::string truncate_name(const std::string& name, uint32_t max_len);

// print_progress — dynamic progress bar (throttled)
void print_progress(uint64_t current_bytes, uint64_t total_bytes, const std::string& current_file = "");

// print_transfer_stats — summary of a file transfer operation
void print_transfer_stats(uint32_t file_count, uint32_t dir_count, uint64_t total_bytes, double elapsed);

} // namespace amidisk
