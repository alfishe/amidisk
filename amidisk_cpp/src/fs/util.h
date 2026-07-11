#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <cstdint>

namespace amidisk {

// Amiga timestamps are days/mins/ticks since 1978-01-01
std::chrono::system_clock::time_point amiga_to_datetime(uint32_t days, uint32_t mins, uint32_t ticks);
void datetime_to_amiga(const std::chrono::system_clock::time_point& dt, uint32_t& days, uint32_t& mins, uint32_t& ticks);

// Protection bits (HSPARWED)
std::string protect_to_str(uint32_t protect);
uint32_t str_to_protect(const std::string& s);

// Name hashing and comparison (OFS/FFS standard)
uint32_t hash_name(const std::vector<uint8_t>& name, uint32_t ht_size, bool intl);
bool names_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool intl);

// Upper character translation
uint8_t upper_char(uint8_t c, bool intl);

} // namespace amidisk
