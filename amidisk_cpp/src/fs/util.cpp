#include "util.h"
#include <algorithm>

namespace amidisk {

constexpr uint32_t TICKS_PER_SEC = 50;

// Amiga epoch: 1978-01-01 00:00:00 UTC
// Offset from Unix epoch (1970-01-01) = 8 years = 2922 days (including leap years 1972, 1976)
constexpr int64_t AMIGA_UNIX_OFFSET_SECS = 2922LL * 86400LL;

static std::chrono::system_clock::time_point get_epoch() {
    return std::chrono::system_clock::from_time_t(AMIGA_UNIX_OFFSET_SECS);
}

std::chrono::system_clock::time_point amiga_to_datetime(uint32_t days, uint32_t mins, uint32_t ticks) {
    auto epoch = get_epoch();
    auto dur = std::chrono::hours(days * 24) + std::chrono::minutes(mins) + std::chrono::milliseconds((ticks * 1000) / TICKS_PER_SEC);
    return epoch + dur;
}

void datetime_to_amiga(const std::chrono::system_clock::time_point& dt, uint32_t& days, uint32_t& mins, uint32_t& ticks) {
    auto epoch = get_epoch();
    if (dt < epoch) {
        days = 0; mins = 0; ticks = 0;
        return;
    }
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(dt - epoch);
    
    auto h = std::chrono::duration_cast<std::chrono::hours>(dur);
    days = h.count() / 24;
    
    auto m = std::chrono::duration_cast<std::chrono::minutes>(dur - std::chrono::hours(days * 24));
    mins = m.count();
    
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur - std::chrono::hours(days * 24) - m);
    ticks = (ms.count() * TICKS_PER_SEC) / 1000;
}

static const char* PROT_FLAGS = "hsparwed";

std::string protect_to_str(uint32_t protect) {
    std::string out;
    for (int i = 0; i < 8; ++i) {
        uint32_t bit = 1 << (7 - i);
        bool is_set = (protect & bit) != 0;
        if (7 - i <= 3) {
            // rwed inverted
            out += is_set ? "-" : std::string(1, PROT_FLAGS[i]);
        } else {
            out += is_set ? std::string(1, PROT_FLAGS[i]) : "-";
        }
    }
    return out;
}

uint32_t str_to_protect(const std::string& s) {
    uint32_t protect = 0;
    std::string lower_s = s;
    std::transform(lower_s.begin(), lower_s.end(), lower_s.begin(), ::tolower);
    while (lower_s.length() < 8) lower_s += "-";

    for (int i = 0; i < 8; ++i) {
        uint32_t bit = 1 << (7 - i);
        bool present = (lower_s[i] == PROT_FLAGS[i]);
        if (7 - i <= 3) {
            if (!present) protect |= bit;
        } else {
            if (present) protect |= bit;
        }
    }
    return protect;
}

uint8_t upper_char(uint8_t c, bool intl) {
    if (c >= 0x61 && c <= 0x7A) return c - 0x20;
    if (intl && c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    return c;
}

uint32_t hash_name(const std::vector<uint8_t>& name, uint32_t ht_size, bool intl) {
    uint32_t h = name.size();
    for (uint8_t c : name) {
        h = (h * 13 + upper_char(c, intl)) & 0x7FF;
    }
    return h % ht_size;
}

bool names_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool intl) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (upper_char(a[i], intl) != upper_char(b[i], intl)) return false;
    }
    return true;
}

} // namespace amidisk
