// Intset tests, including byte-level agreement with Redis 8.10.1.
#include <cstdint>
#include <string>
#include <vector>

#include "core/intset.h"
#include "test_harness.h"

using namespace mnemos::core;

namespace {

std::string toHex(const std::vector<std::uint8_t>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

void testMatchesRealRedisBytes() {
    // Extracted from Redis 8.10.1 via SADD + DUMP, one case per width.
    struct Case {
        std::vector<std::int64_t> members;
        const char*               expected_hex;
    };
    const Case cases[] = {
        {{1, 2, 3, -5},       "0200000004000000fbff010002000300"},
        {{1, 100000},         "040000000200000001000000a0860100"},
        {{1, 9999999999LL},   "08000000020000000100000000000000ffe30b5402000000"},
    };

    for (const Case& c : cases) {
        IntSet set;
        for (std::int64_t v : c.members) CHECK(set.add(v));
        CHECK_EQ(toHex(set.toBytes()), std::string(c.expected_hex));
    }
}

void testSortedAndDeduplicated() {
    IntSet set;
    // Inserted out of order; an intset is always stored ascending, which is why
    // SMEMBERS on an intset-encoded set comes back sorted.
    for (std::int64_t v : {50, 10, 30, 20, 40}) CHECK(set.add(v));
    CHECK_EQ(set.values(), (std::vector<std::int64_t>{10, 20, 30, 40, 50}));

    CHECK(!set.add(30));  // duplicate rejected
    CHECK_EQ(set.size(), std::size_t{5});
}

void testWidthUpgradeIsOneWay() {
    IntSet set;
    set.add(1);
    set.add(2);
    CHECK(set.width() == IntSet::Width::Int16);

    set.add(100000);  // no longer fits in int16
    CHECK(set.width() == IntSet::Width::Int32);

    set.add(9999999999LL);
    CHECK(set.width() == IntSet::Width::Int64);

    // Removing the wide members does NOT narrow the encoding back again.
    CHECK(set.remove(9999999999LL));
    CHECK(set.remove(100000));
    CHECK(set.width() == IntSet::Width::Int64);
    CHECK_EQ(set.size(), std::size_t{2});
}

void testMembership() {
    IntSet set;
    for (std::int64_t v : {-1000, -1, 0, 1, 1000}) set.add(v);
    for (std::int64_t v : {-1000, -1, 0, 1, 1000}) CHECK(set.contains(v));
    for (std::int64_t v : {-1001, -2, 2, 999, 1001}) CHECK(!set.contains(v));

    CHECK(set.remove(0));
    CHECK(!set.contains(0));
    CHECK(!set.remove(0));
}

void testBoundaryValues() {
    IntSet set;
    const std::int64_t values[] = {
        -32768, 32767,              // int16 limits
        -32769, 32768,              // just past
        -2147483648LL, 2147483647LL,
        -2147483649LL, 2147483648LL,
        INT64_MIN, INT64_MAX,
    };
    for (std::int64_t v : values) CHECK(set.add(v));
    CHECK(set.width() == IntSet::Width::Int64);
    for (std::int64_t v : values) CHECK(set.contains(v));

    auto restored = IntSet::fromBytes(set.toBytes());
    CHECK(restored.has_value());
    if (restored) CHECK_EQ(restored->values(), set.values());
}

void testRoundTripAndValidation() {
    IntSet set;
    for (std::int64_t i = 0; i < 100; ++i) set.add(i * 7);

    auto restored = IntSet::fromBytes(set.toBytes());
    CHECK(restored.has_value());
    if (restored) {
        CHECK_EQ(restored->values(), set.values());
        CHECK(restored->width() == set.width());
    }

    CHECK(!IntSet::fromBytes(std::vector<std::uint8_t>{}).has_value());

    // A bad encoding width must be rejected.
    std::vector<std::uint8_t> bad = set.toBytes();
    bad[0] = 3;
    CHECK(!IntSet::fromBytes(bad).has_value());

    // A length that disagrees with the payload must be rejected.
    std::vector<std::uint8_t> short_payload = set.toBytes();
    short_payload.resize(short_payload.size() - 1);
    CHECK(!IntSet::fromBytes(short_payload).has_value());

    // Unsorted contents must be rejected: binary search depends on the order.
    IntSet two;
    two.add(10);
    two.add(20);
    std::vector<std::uint8_t> unsorted = two.toBytes();
    std::swap(unsorted[8], unsorted[10]);
    std::swap(unsorted[9], unsorted[11]);
    CHECK(!IntSet::fromBytes(unsorted).has_value());
}

}  // namespace

int main() {
    testMatchesRealRedisBytes();
    testSortedAndDeduplicated();
    testWidthUpgradeIsOneWay();
    testMembership();
    testBoundaryValues();
    testRoundTripAndValidation();
    return mnemos::test::summarise("intset");
}
