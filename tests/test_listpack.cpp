// Listpack tests.
//
// The headline test is testMatchesRealRedisBytes(): the expected buffer is not
// something we invented, it was extracted from Redis 8.10.1 via DUMP and
// decoded by hand. If our encoder ever drifts from the real format, this fails --
// which is what makes the later "real redis-server can load our RDB" claim
// credible rather than aspirational.
#include <cstdint>
#include <string>
#include <vector>

#include "core/listpack.h"
#include "test_harness.h"

using namespace mnemos::core;

namespace {

std::string toHex(const std::vector<std::uint8_t>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

void testBacklenRoundTrip() {
    // Every boundary of the 1..5 byte variable-length encoding.
    const std::uint64_t values[] = {0, 1, 127, 128, 16383, 16384, 2097151,
                                    2097152, 268435455, 268435456};
    for (std::uint64_t value : values) {
        std::uint8_t buf[5] = {0};
        const int written = encodeBacklen(value, buf);
        int read = 0;
        // Decoding walks backwards from the *last* byte, as it does in a real
        // right-to-left listpack traversal.
        const std::uint64_t decoded = decodeBacklenBackwards(buf + written - 1, read);
        CHECK_EQ(decoded, value);
        CHECK_EQ(read, written);
    }
}

void testMatchesRealRedisBytes() {
    // Produced by: HSET h f1 v1 n7 100 n13 4000 n16 30000 n64 9999999999
    // then DUMP h against redis-server 8.10.1, stripping the RDB type byte,
    // string-length prefix, version and CRC footer.
    const std::string kRedisHex =
        "350000000a008266310382763103826e37036401836e313304cfa002836e313604f130"
        "7503836e363404f4ffe30b540200000009ff";

    Listpack lp;
    for (const char* field : {"f1", "v1", "n7", "100", "n13", "4000", "n16", "30000",
                              "n64", "9999999999"}) {
        lp.append(field);
    }

    CHECK_EQ(toHex(lp.bytes()), kRedisHex);
    CHECK_EQ(lp.numElements(), std::size_t{10});
    CHECK_EQ(lp.totalBytes(), std::size_t{53});
}

void testIntegerEncodingSelection() {
    // Each value should land in the smallest encoding that fits, which is
    // observable through the total size of a single-element listpack.
    struct Case { std::int64_t value; std::size_t payload_bytes; };
    const Case cases[] = {
        {0,           1},  // 7-bit uint
        {127,         1},
        {128,         2},  // 13-bit int
        {-1,          2},
        {4095,        2},
        {-4096,       2},
        {4096,        3},  // int16
        {32767,       3},
        {-32768,      3},
        {32768,       4},  // int24
        {8388607,     4},
        {8388608,     5},  // int32
        {2147483647,  5},
        {2147483648LL, 9}, // int64
    };
    for (const Case& c : cases) {
        Listpack lp;
        lp.appendInteger(c.value);
        // header(6) + payload + backlen(1) + EOF(1)
        CHECK_EQ(lp.totalBytes(), 6 + c.payload_bytes + 1 + 1);

        Listpack::Element element;
        CHECK(lp.get(0, element));
        CHECK(element.is_integer);
        CHECK_EQ(element.integer, c.value);
    }
}

void testStringEncodingSelection() {
    struct Case { std::size_t length; std::size_t header_bytes; };
    const Case cases[] = {
        {1,    1},   // 6-bit string length
        {63,   1},
        {64,   2},   // 12-bit
        {4095, 2},
        {4096, 5},   // 32-bit
    };
    for (const Case& c : cases) {
        // 'a' repeated: never parses as an integer, so it stays a string.
        const std::string value(c.length, 'a');
        Listpack lp;
        lp.append(value);
        std::uint8_t backlen[5];
        const int backlen_size = encodeBacklen(c.header_bytes + c.length, backlen);
        CHECK_EQ(lp.totalBytes(),
                 6 + c.header_bytes + c.length + static_cast<std::size_t>(backlen_size) + 1);

        Listpack::Element element;
        CHECK(lp.get(0, element));
        CHECK(!element.is_integer);
        CHECK_EQ(std::string(element.string), value);
    }
}

void testNumericStringsBecomeIntegers() {
    Listpack lp;
    lp.append("12345");
    Listpack::Element element;
    CHECK(lp.get(0, element));
    // Stored as an int64, but reads back as the same text.
    CHECK(element.is_integer);
    CHECK_EQ(element.integer, std::int64_t{12345});
    CHECK_EQ(element.toString(), std::string("12345"));

    // Non-canonical numerics stay strings, matching the string2ll rule.
    Listpack lz;
    lz.append("007");
    CHECK(lz.get(0, element));
    CHECK(!element.is_integer);
    CHECK_EQ(std::string(element.string), std::string("007"));
}

void testMutations() {
    Listpack lp;
    for (const char* v : {"a", "b", "c", "d", "e"}) lp.append(v);
    CHECK_EQ(lp.numElements(), std::size_t{5});

    CHECK(lp.eraseAt(2));  // remove "c"
    CHECK_EQ(lp.numElements(), std::size_t{4});
    CHECK_EQ(lp.toStrings(), (std::vector<std::string>{"a", "b", "d", "e"}));

    CHECK(lp.insertAt(1, "z"));
    CHECK_EQ(lp.toStrings(), (std::vector<std::string>{"a", "z", "b", "d", "e"}));

    CHECK(lp.replaceAt(0, "999"));
    CHECK_EQ(lp.toStrings(), (std::vector<std::string>{"999", "z", "b", "d", "e"}));

    // Replacing with a different-width encoding must keep the buffer consistent.
    CHECK(lp.replaceAt(0, std::string(100, 'x')));
    CHECK_EQ(lp.numElements(), std::size_t{5});
    CHECK_EQ(lp.toStrings()[0], std::string(100, 'x'));

    CHECK(lp.eraseRange(1, 2));
    CHECK_EQ(lp.numElements(), std::size_t{3});

    CHECK(lp.insertAt(lp.numElements(), "tail"));
    CHECK_EQ(lp.toStrings().back(), std::string("tail"));
}

void testFindWithStep() {
    // A hash stored flat as field,value,field,value: searching fields alone
    // requires step 2, otherwise a value could shadow a field name.
    Listpack lp;
    for (const char* v : {"name", "bob", "age", "30", "city", "name"}) lp.append(v);

    CHECK(lp.find("name", 0, 2).value_or(999) == 0);
    CHECK(lp.find("age", 0, 2).value_or(999) == 2);
    CHECK(lp.find("city", 0, 2).value_or(999) == 4);
    // "name" also appears as a *value* at index 5, but a field search must miss it.
    CHECK(!lp.find("bob", 0, 2).has_value());
    // Searching values instead: start at 1.
    CHECK(lp.find("bob", 1, 2).value_or(999) == 1);

    // Numeric comparison: "30" is stored as an integer and must still be found.
    CHECK(lp.find("30", 1, 2).value_or(999) == 3);
    // ...but "030" must not match it.
    CHECK(!lp.find("030", 1, 2).has_value());
}

void testRoundTripThroughBytes() {
    Listpack original;
    for (const char* v : {"alpha", "42", "-9999999999", "", "beta"}) original.append(v);

    auto restored = Listpack::fromBytes(original.bytes());
    CHECK(restored.has_value());
    if (restored) {
        CHECK_EQ(restored->toStrings(), original.toStrings());
        CHECK_EQ(restored->totalBytes(), original.totalBytes());
    }

    // Corruption must be rejected, not trusted: these bytes arrive from disk
    // and from replication peers.
    std::vector<std::uint8_t> corrupt = original.bytes();
    corrupt.back() = 0x00;  // clobber the EOF terminator
    CHECK(!Listpack::fromBytes(corrupt).has_value());

    std::vector<std::uint8_t> truncated = original.bytes();
    truncated.resize(truncated.size() / 2);
    CHECK(!Listpack::fromBytes(truncated).has_value());

    std::vector<std::uint8_t> bad_header = original.bytes();
    bad_header[0] = 0xFF;  // declared total-bytes no longer matches
    CHECK(!Listpack::fromBytes(bad_header).has_value());

    CHECK(!Listpack::fromBytes(std::vector<std::uint8_t>{}).has_value());
}

void testEmptyAndClear() {
    Listpack lp;
    CHECK_EQ(lp.numElements(), std::size_t{0});
    CHECK_EQ(lp.totalBytes(), std::size_t{7});  // 6 header + EOF

    for (int i = 0; i < 50; ++i) lp.append("v" + std::to_string(i));
    CHECK_EQ(lp.numElements(), std::size_t{50});

    lp.clear();
    CHECK_EQ(lp.numElements(), std::size_t{0});
    CHECK_EQ(lp.totalBytes(), std::size_t{7});
    CHECK(Listpack::fromBytes(lp.bytes()).has_value());
}

}  // namespace

int main() {
    testBacklenRoundTrip();
    testMatchesRealRedisBytes();
    testIntegerEncodingSelection();
    testStringEncodingSelection();
    testNumericStringsBecomeIntegers();
    testMutations();
    testFindWithStep();
    testRoundTripThroughBytes();
    testEmptyAndClear();
    return mnemos::test::summarise("listpack");
}
