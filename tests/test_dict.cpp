// Dict tests. The SCAN guarantee is the interesting one: elements present for
// the whole iteration must be returned even if the table resizes mid-scan.
#include <set>
#include <string>
#include <unordered_set>

#include "core/dict.h"
#include "test_harness.h"

using namespace mnemos::core;

namespace {

void testBasicOperations() {
    Dict<int> d;
    CHECK(d.empty());

    CHECK(d.insert("alpha", 1));
    CHECK(d.insert("beta", 2));
    CHECK(!d.insert("alpha", 10));  // overwrite reports "not new"
    CHECK_EQ(d.size(), std::size_t{2});

    const int* v = d.find("alpha");
    CHECK(v != nullptr);
    if (v) CHECK_EQ(*v, 10);

    CHECK(d.find("missing") == nullptr);
    CHECK(d.erase("alpha"));
    CHECK(!d.erase("alpha"));
    CHECK_EQ(d.size(), std::size_t{1});
}

void testGrowthAndRehash() {
    Dict<int> d;
    constexpr int kCount = 5000;
    for (int i = 0; i < kCount; ++i) d.insert("key:" + std::to_string(i), i);
    CHECK_EQ(d.size(), static_cast<std::size_t>(kCount));

    // Every key must remain findable across all the intermediate rehashes.
    for (int i = 0; i < kCount; ++i) {
        const int* v = d.find("key:" + std::to_string(i));
        CHECK(v != nullptr);
        if (v) CHECK_EQ(*v, i);
    }

    for (int i = 0; i < kCount; ++i) d.erase("key:" + std::to_string(i));
    CHECK_EQ(d.size(), std::size_t{0});
}

void testScanVisitsEverything() {
    Dict<int> d;
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i) d.insert("k" + std::to_string(i), i);

    std::unordered_set<std::string> seen;
    std::uint64_t cursor = 0;
    int guard = 0;
    do {
        cursor = d.scan(cursor, [&seen](const std::string& k, const int&) { seen.insert(k); });
    } while (cursor != 0 && ++guard < 100000);

    CHECK_EQ(cursor, std::uint64_t{0});
    CHECK_EQ(seen.size(), static_cast<std::size_t>(kCount));
}

void testScanDuringRehash() {
    // Start a scan, force a resize partway through by inserting heavily, and
    // confirm the keys that existed for the whole iteration were all returned.
    Dict<int> d;
    constexpr int kInitial = 128;
    for (int i = 0; i < kInitial; ++i) d.insert("stable:" + std::to_string(i), i);

    std::unordered_set<std::string> seen;
    std::uint64_t cursor = 0;
    int step  = 0;
    int guard = 0;
    do {
        cursor = d.scan(cursor, [&seen](const std::string& k, const int&) { seen.insert(k); });
        if (step == 2) {
            for (int i = 0; i < 4000; ++i) d.insert("extra:" + std::to_string(i), i);
        }
        ++step;
    } while (cursor != 0 && ++guard < 200000);

    CHECK_EQ(cursor, std::uint64_t{0});
    int stable_seen = 0;
    for (int i = 0; i < kInitial; ++i) {
        if (seen.count("stable:" + std::to_string(i))) ++stable_seen;
    }
    CHECK_EQ(stable_seen, kInitial);
}

void testRandomEntry() {
    Dict<int> d;
    for (int i = 0; i < 100; ++i) d.insert("k" + std::to_string(i), i);

    // Sampling should reach a decent spread rather than returning one bucket.
    std::set<std::string> distinct;
    for (int i = 0; i < 500; ++i) {
        const auto* e = d.randomEntry();
        CHECK(e != nullptr);
        if (e) distinct.insert(e->key);
    }
    CHECK(distinct.size() > 20);

    Dict<int> empty;
    CHECK(empty.randomEntry() == nullptr);
}

void testReverseBits() {
    CHECK_EQ(reverseBits(0), std::uint64_t{0});
    CHECK_EQ(reverseBits(1), std::uint64_t{1} << 63);
    CHECK_EQ(reverseBits(reverseBits(0x0123456789ABCDEFULL)), std::uint64_t{0x0123456789ABCDEFULL});
}

}  // namespace

int main() {
    testBasicOperations();
    testGrowthAndRehash();
    testScanVisitsEverything();
    testScanDuringRehash();
    testRandomEntry();
    testReverseBits();
    return mnemos::test::summarise("dict");
}
