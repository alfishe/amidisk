#include <gtest/gtest.h>
#include "fs/util.h"
#include <chrono>
#include <ctime>

using namespace amidisk;

// Test Amiga timestamp conversion
class TimestampTest : public ::testing::Test {};

TEST_F(TimestampTest, AmigaEpoch) {
    // Amiga epoch is 1978-01-01 00:00:00 UTC
    // This should convert to Unix timestamp 252460800 (8 years * 365.25 days)
    auto tp = amiga_to_datetime(0, 0, 0);
    auto unix_time = std::chrono::duration_cast<std::chrono::seconds>(
        tp.time_since_epoch()).count();

    // 1978-01-01 00:00:00 UTC = 252460800 Unix time
    // 8 years = 2922 days (1972 and 1976 are leap years)
    constexpr int64_t expected = 2922LL * 86400LL;
    EXPECT_EQ(unix_time, expected);
}

TEST_F(TimestampTest, AmigaToUnixKnownDate) {
    // Test a specific date: days=0, mins=0 should be 1978-01-01
    auto tp = amiga_to_datetime(0, 0, 0);
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm* tm = std::gmtime(&t);

    EXPECT_EQ(tm->tm_year + 1900, 1978);
    EXPECT_EQ(tm->tm_mon + 1, 1);  // January
    EXPECT_EQ(tm->tm_mday, 1);
    EXPECT_EQ(tm->tm_hour, 0);
    EXPECT_EQ(tm->tm_min, 0);
}

TEST_F(TimestampTest, UnixToAmigaAndBack) {
    // Round trip test: create a time point, convert to Amiga, convert back
    // Use a known date: 2020-06-15 10:20:30
    std::tm tm_in = {};
    tm_in.tm_year = 2020 - 1900;
    tm_in.tm_mon = 5;   // June (0-indexed)
    tm_in.tm_mday = 15;
    tm_in.tm_hour = 10;
    tm_in.tm_min = 20;
    tm_in.tm_sec = 30;

    // Convert to time_t (assuming UTC)
    std::time_t t = timegm(&tm_in);
    auto tp = std::chrono::system_clock::from_time_t(t);

    uint32_t days, mins, ticks;
    datetime_to_amiga(tp, days, mins, ticks);

    // Convert back
    auto tp2 = amiga_to_datetime(days, mins, ticks);

    // Should be within 1 second (ticks have 1/50 sec resolution)
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(tp2 - tp).count();
    EXPECT_LE(std::abs(diff), 1);
}

TEST_F(TimestampTest, BeforeAmigaEpoch) {
    // Dates before 1978-01-01 should return 0,0,0
    std::tm tm_in = {};
    tm_in.tm_year = 1970 - 1900;
    tm_in.tm_mon = 0;
    tm_in.tm_mday = 1;

    std::time_t t = timegm(&tm_in);
    auto tp = std::chrono::system_clock::from_time_t(t);

    uint32_t days, mins, ticks;
    datetime_to_amiga(tp, days, mins, ticks);

    EXPECT_EQ(days, 0);
    EXPECT_EQ(mins, 0);
    EXPECT_EQ(ticks, 0);
}

// Test protection bits conversion
class ProtectTest : public ::testing::Test {};

TEST_F(ProtectTest, DefaultProtection) {
    // Default Amiga protection is ----rwed (RWED allowed, no HSPA flags)
    // Binary: 0000 0000 (all zeros means RWED allowed since they're inverted)
    std::string s = protect_to_str(0);
    EXPECT_EQ(s, "----rwed");
}

TEST_F(ProtectTest, NoPermissions) {
    // All RWED bits set means NO permissions (since they're inverted)
    // Binary: 0000 1111
    std::string s = protect_to_str(0x0F);
    EXPECT_EQ(s, "--------");
}

TEST_F(ProtectTest, ScriptPureFlags) {
    // S and P flags set: binary 0110 0000 = 0x60
    // Order is HSPARWED: bit 6=S, bit 5=P
    std::string s = protect_to_str(0x60);
    EXPECT_EQ(s, "-sp-rwed");
}

TEST_F(ProtectTest, AllFlags) {
    // All flags set: HSPA set, RWED cleared (no permissions)
    // Binary: 1111 1111 = 0xFF
    std::string s = protect_to_str(0xFF);
    EXPECT_EQ(s, "hspa----");
}

TEST_F(ProtectTest, StrToProtect) {
    EXPECT_EQ(str_to_protect("----rwed"), 0x00);
    EXPECT_EQ(str_to_protect("--------"), 0x0F);
    EXPECT_EQ(str_to_protect("hsparwed"), 0xF0);
    EXPECT_EQ(str_to_protect("-sp-rwed"), 0x60);
    EXPECT_EQ(str_to_protect("-s-arwed"), 0x50);  // S and A flags
}

TEST_F(ProtectTest, RoundTrip) {
    for (uint32_t p = 0; p <= 0xFF; ++p) {
        std::string s = protect_to_str(p);
        uint32_t p2 = str_to_protect(s);
        EXPECT_EQ(p, p2) << "Failed for protect=" << p << " str=" << s;
    }
}

// Test name hashing
class HashTest : public ::testing::Test {};

TEST_F(HashTest, BasicHash) {
    std::vector<uint8_t> name = {'T', 'e', 's', 't'};
    uint32_t h = hash_name(name, 72, false);
    EXPECT_LT(h, 72);  // Hash should be less than hash table size
}

TEST_F(HashTest, CaseInsensitive) {
    std::vector<uint8_t> lower = {'t', 'e', 's', 't'};
    std::vector<uint8_t> upper = {'T', 'E', 'S', 'T'};

    EXPECT_EQ(hash_name(lower, 72, false), hash_name(upper, 72, false));
}

TEST_F(HashTest, NamesEqual) {
    std::vector<uint8_t> a = {'H', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> b = {'h', 'E', 'L', 'L', 'O'};

    EXPECT_TRUE(names_equal(a, b, false));
    EXPECT_TRUE(names_equal(a, b, true));
}

TEST_F(HashTest, NamesNotEqual) {
    std::vector<uint8_t> a = {'H', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> b = {'W', 'o', 'r', 'l', 'd'};

    EXPECT_FALSE(names_equal(a, b, false));
}

TEST_F(HashTest, IntlCharacters) {
    // Test international character handling (ä -> Ä)
    std::vector<uint8_t> lower = {0xE4};  // ä
    std::vector<uint8_t> upper = {0xC4};  // Ä

    // With intl=true, these should be equal
    EXPECT_TRUE(names_equal(lower, upper, true));
    // Without intl, they should not be equal
    EXPECT_FALSE(names_equal(lower, upper, false));
}
