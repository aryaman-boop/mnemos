// The RDB codec. The checks that matter here are the ones with an external
// answer -- CRC-64's published check value, the exact number of bytes liblzf
// produces -- because agreeing with ourselves proves nothing about a format
// whose whole purpose is to be read by someone else.
#include "persist/rdb.h"

#include <string>
#include <vector>

#include "core/collections.h"
#include "core/object.h"
#include "test_harness.h"

using namespace mnemos;
using persist::Reader;

namespace {

std::string hex(std::string_view bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

void testCrc64() {
    // The check value every CRC-64/Jones implementation is expected to produce.
    CHECK(persist::crc64(0, "123456789", 9) == 0xe9c6d914c4b8d9caULL);
    CHECK(persist::crc64(0, "", 0) == 0);
    // Streaming in two pieces must equal one pass, or a chunked file writer
    // would not be able to accumulate the checksum as it goes.
    const std::string s = "the quick brown fox jumps over the lazy dog";
    const std::uint64_t whole = persist::crc64(0, s.data(), s.size());
    const std::uint64_t part = persist::crc64(0, s.data(), 10);
    CHECK(persist::crc64(part, s.data() + 10, s.size() - 10) == whole);
}

void testLzf() {
    const std::string runs(300, 'a');
    std::vector<std::uint8_t> out(runs.size() - 4);
    const std::size_t n =
        persist::lzfCompress(reinterpret_cast<const std::uint8_t*>(runs.data()),
                             runs.size(), out.data(), out.size());
    // Redis writes exactly twelve bytes for 300 'a's; anything else means the
    // port drifted from liblzf and the payloads would stop matching.
    CHECK_EQ(n, static_cast<std::size_t>(12));

    std::vector<std::uint8_t> back(runs.size());
    CHECK_EQ(persist::lzfDecompress(out.data(), n, back.data(), back.size()), runs.size());
    CHECK(std::string(back.begin(), back.end()) == runs);

    // Incompressible input has to be refused rather than expanded, since the
    // caller offers a buffer four bytes shorter than the input.
    std::string   random;
    std::uint32_t x = 2463534242u;  // xorshift32: no runs for LZF to find
    for (int i = 0; i < 64; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        random.push_back(static_cast<char>(x & 0xFF));
    }
    std::vector<std::uint8_t> tight(random.size() - 4);
    CHECK_EQ(persist::lzfCompress(reinterpret_cast<const std::uint8_t*>(random.data()),
                                  random.size(), tight.data(), tight.size()),
             static_cast<std::size_t>(0));

    // A truncated back-reference must not read behind the output buffer.
    const std::uint8_t bad[] = {0xE0, 0xFF, 0xFF};
    std::vector<std::uint8_t> scratch(64);
    CHECK_EQ(persist::lzfDecompress(bad, sizeof(bad), scratch.data(), scratch.size()),
             static_cast<std::size_t>(0));
}

void testLengths() {
    const std::uint64_t values[] = {0, 63, 64, 16383, 16384, 70000, 0xFFFFFFFFULL,
                                    0x1FFFFFFFFULL};
    for (std::uint64_t v : values) {
        std::string encoded;
        persist::saveLen(encoded, v);
        Reader reader(encoded);
        std::uint64_t back = 0;
        CHECK(reader.readLen(back));
        CHECK_EQ(back, v);
        CHECK(reader.exhausted());
    }

    std::string encoded;
    persist::saveLen(encoded, 300);
    CHECK_EQ(hex(encoded), std::string("412c"));

    // An encoding tag where a plain length was required is an error, not a 3.
    const std::string tag(1, static_cast<char>(0xC0));
    Reader reader(tag);
    std::uint64_t out = 0;
    CHECK(!reader.readLen(out));
}

void testStrings() {
    struct Case {
        std::string_view input;
        std::string      expected;
    };
    const Case cases[] = {
        {"hello", "0568656c6c6f"},
        {"", "00"},
        {"100", "c064"},
        {"-1", "c0ff"},
        {"12345", "c13930"},
        {"123456789", "c215cd5b07"},
        // Sixteen digits: too long for the integer encodings, too short for LZF.
        {"123456789012345", "0f313233343536373839303132333435"},
        // Leading zeros are not the canonical spelling, so the text is kept.
        {"007", "03303037"},
    };
    for (const Case& c : cases) {
        std::string encoded;
        persist::saveString(encoded, c.input);
        CHECK_EQ(hex(encoded), c.expected);

        Reader reader(encoded);
        std::string back;
        CHECK(reader.readString(back));
        CHECK_EQ(back, std::string(c.input));
    }

    // Over twenty bytes and compressible: LZF, with the two lengths behind it.
    std::string encoded;
    persist::saveString(encoded, std::string(300, 'a'));
    CHECK_EQ(hex(encoded).substr(0, 8), std::string("c30c412c"));
    Reader reader(encoded);
    std::string back;
    CHECK(reader.readString(back));
    CHECK_EQ(back, std::string(300, 'a'));

    // Truncated input must fail rather than read past the end.
    Reader truncated(std::string_view(encoded).substr(0, encoded.size() - 3));
    std::string ignored;
    CHECK(!truncated.readString(ignored));
}

// Round-trips a value through DUMP and back, checking that the encoding
// survives -- which is the whole reason collections are stored in their
// in-memory form rather than as a flat list of elements.
core::Value roundTrip(const core::Value& value) {
    const std::string payload = persist::dumpPayload(value);
    CHECK(persist::payloadFooterOk(payload));
    core::Value loaded;
    CHECK(persist::loadPayload(payload, loaded));
    return loaded;
}

void testStringObjects() {
    for (const char* text : {"hello", "12345", "123456789012345678", ""}) {
        const core::Value value = core::Value::makeString(text);
        const core::Value back = roundTrip(value);
        CHECK_EQ(back.stringValue(), std::string(text));
        CHECK(back.encoding() == value.encoding());
    }

    std::string big(4096, 'z');
    CHECK_EQ(roundTrip(core::Value::makeString(big)).stringValue(), big);
}

void testCollectionObjects() {
    // Hash, both encodings.
    core::Value small_hash = core::Value::makeHash();
    small_hash.hash()->set("a", "1");
    small_hash.hash()->set("b", "2");
    CHECK(small_hash.hash()->encoding() == core::ObjEncoding::ListPack);
    core::Value back = roundTrip(small_hash);
    CHECK(back.hash()->encoding() == core::ObjEncoding::ListPack);
    CHECK_EQ(back.hash()->flatten(), small_hash.hash()->flatten());

    core::Value big_hash = core::Value::makeHash();
    for (int i = 0; i < 600; ++i) {
        big_hash.hash()->set("field" + std::to_string(i), std::to_string(i));
    }
    CHECK(big_hash.hash()->encoding() == core::ObjEncoding::HashTable);
    back = roundTrip(big_hash);
    CHECK(back.hash()->encoding() == core::ObjEncoding::HashTable);
    CHECK_EQ(back.hash()->size(), big_hash.hash()->size());
    CHECK_EQ(back.hash()->get("field599").value_or(""), std::string("599"));

    // Set: all three encodings.
    core::Value intset = core::Value::makeSet();
    for (int i = 0; i < 5; ++i) intset.set()->add(std::to_string(i * 1000));
    CHECK(intset.set()->encoding() == core::ObjEncoding::IntSet);
    back = roundTrip(intset);
    CHECK(back.set()->encoding() == core::ObjEncoding::IntSet);
    CHECK_EQ(back.set()->members(), intset.set()->members());

    core::Value lp_set = core::Value::makeSet();
    lp_set.set()->add("alpha");
    lp_set.set()->add("beta");
    CHECK(lp_set.set()->encoding() == core::ObjEncoding::ListPack);
    back = roundTrip(lp_set);
    CHECK(back.set()->encoding() == core::ObjEncoding::ListPack);
    CHECK_EQ(back.set()->members(), lp_set.set()->members());

    core::Value ht_set = core::Value::makeSet();
    for (int i = 0; i < 200; ++i) ht_set.set()->add("member" + std::to_string(i));
    CHECK(ht_set.set()->encoding() == core::ObjEncoding::HashTable);
    back = roundTrip(ht_set);
    CHECK(back.set()->encoding() == core::ObjEncoding::HashTable);
    CHECK_EQ(back.set()->size(), ht_set.set()->size());
    CHECK(back.set()->contains("member199"));

    // Sorted set, both encodings. The skiplist form is written backwards, so
    // an ascending read of the reloaded value is the real test.
    core::Value small_zset = core::Value::makeZSet();
    small_zset.zset()->add("one", 1);
    small_zset.zset()->add("two", 2.5);
    small_zset.zset()->add("three", -3);
    back = roundTrip(small_zset);
    CHECK(back.zset()->encoding() == core::ObjEncoding::ListPack);
    CHECK(back.zset()->all() == small_zset.zset()->all());

    core::Value big_zset = core::Value::makeZSet();
    for (int i = 0; i < 200; ++i) big_zset.zset()->add("m" + std::to_string(i), i * 1.5);
    CHECK(big_zset.zset()->encoding() == core::ObjEncoding::SkipList);
    back = roundTrip(big_zset);
    CHECK(back.zset()->encoding() == core::ObjEncoding::SkipList);
    CHECK(back.zset()->all() == big_zset.zset()->all());
    CHECK_EQ(back.zset()->score("m199").value_or(0), 199 * 1.5);

    // List, both encodings. The quicklist keeps its node boundaries, because
    // the nodes are what is written.
    core::Value small_list = core::Value::makeList();
    for (int i = 0; i < 4; ++i) small_list.list()->pushBack("item" + std::to_string(i));
    back = roundTrip(small_list);
    CHECK(back.list()->encoding() == core::ObjEncoding::ListPack);
    CHECK_EQ(back.list()->range(0, -1), small_list.list()->range(0, -1));

    core::Value big_list = core::Value::makeList();
    for (int i = 0; i < 500; ++i) big_list.list()->pushBack(std::string(100, 'x') +
                                                            std::to_string(i));
    CHECK(big_list.list()->encoding() == core::ObjEncoding::QuickList);
    CHECK(big_list.list()->nodes().size() > 1);
    back = roundTrip(big_list);
    CHECK(back.list()->encoding() == core::ObjEncoding::QuickList);
    CHECK_EQ(back.list()->nodes().size(), big_list.list()->nodes().size());
    CHECK_EQ(back.list()->range(0, -1), big_list.list()->range(0, -1));
}

void testBadPayloads() {
    const core::Value value = core::Value::makeString("hello");
    std::string payload = persist::dumpPayload(value);

    // A flipped byte anywhere before the checksum must be caught.
    std::string corrupt = payload;
    corrupt[1] ^= 0x01;
    CHECK(!persist::payloadFooterOk(corrupt));

    // A version from the future is refused even with a valid checksum.
    std::string future = payload;
    future[future.size() - 10] = static_cast<char>(persist::kRdbVersion + 1);
    CHECK(!persist::payloadFooterOk(future));

    CHECK(!persist::payloadFooterOk(""));
    CHECK(!persist::payloadFooterOk(std::string(10, '\0')));

    // A type byte mnemos does not write is rejected rather than half-read.
    core::Value out;
    std::string ziplist = payload;
    ziplist[0] = 10;  // RDB_TYPE_LIST_ZIPLIST
    CHECK(!persist::loadPayload(ziplist, out));

    // Trailing bytes inside the payload are a malformed payload, not a value
    // with something after it.
    std::string extra = persist::dumpPayload(value);
    extra.insert(extra.size() - 10, "junk");
    CHECK(!persist::loadPayload(extra, out));
}

void testFileRoundTrip() {
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                             "/mnemos_test_rdb.rdb";
    std::remove(path.c_str());

    {
        auto writer = persist::FileWriter::create(path);
        CHECK(writer.has_value());
        if (!writer) return;
        writer->selectDb(0, 2, 1);
        writer->writeEntry("plain", core::Value::makeString("value"), -1);
        writer->writeEntry("mortal", core::Value::makeString("dies"), 1700000000000LL);
        core::Value hash = core::Value::makeHash();
        hash.hash()->set("f", "v");
        writer->selectDb(3, 1, 0);
        writer->writeEntry("h", hash, -1);
        CHECK(writer->finish());
    }

    struct Entry {
        int          db;
        std::string  key;
        core::Value  value;
        std::int64_t expire;
    };
    std::vector<Entry> loaded;
    bool existed = false;
    std::string error;
    const bool ok = persist::loadFile(
        path,
        [&loaded](int db, std::string key, core::Value value, std::int64_t expire) {
            loaded.push_back({db, std::move(key), std::move(value), expire});
        },
        existed, error);
    CHECK(ok);
    CHECK(existed);
    CHECK_EQ(error, std::string());
    CHECK_EQ(loaded.size(), static_cast<std::size_t>(3));
    if (loaded.size() == 3) {
        CHECK_EQ(loaded[0].db, 0);
        CHECK_EQ(loaded[0].key, std::string("plain"));
        CHECK_EQ(loaded[0].value.stringValue(), std::string("value"));
        CHECK_EQ(loaded[0].expire, static_cast<std::int64_t>(-1));
        CHECK_EQ(loaded[1].key, std::string("mortal"));
        CHECK_EQ(loaded[1].expire, static_cast<std::int64_t>(1700000000000LL));
        CHECK_EQ(loaded[2].db, 3);
        CHECK(loaded[2].value.hash()->encoding() == core::ObjEncoding::ListPack);
    }

    // A single flipped byte in the body has to be caught by the trailing CRC.
    std::string contents;
    {
        std::FILE* fp = std::fopen(path.c_str(), "rb");
        CHECK(fp != nullptr);
        char chunk[4096];
        std::size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof(chunk), fp)) > 0) contents.append(chunk, n);
        std::fclose(fp);
    }
    contents[contents.size() - 12] ^= 0x40;
    {
        std::FILE* fp = std::fopen(path.c_str(), "wb");
        CHECK(fp != nullptr);
        std::fwrite(contents.data(), 1, contents.size(), fp);
        std::fclose(fp);
    }
    loaded.clear();
    error.clear();
    CHECK(!persist::loadFile(
        path,
        [&loaded](int db, std::string key, core::Value value, std::int64_t expire) {
            loaded.push_back({db, std::move(key), std::move(value), expire});
        },
        existed, error));
    CHECK(!error.empty());

    std::remove(path.c_str());

    // A file that is not there is not an error: there is simply no snapshot.
    existed = true;
    error.clear();
    CHECK(persist::loadFile(
        path, [](int, std::string, core::Value, std::int64_t) {}, existed, error));
    CHECK(!existed);
    CHECK(error.empty());
}

}  // namespace

int main() {
    testCrc64();
    testLzf();
    testLengths();
    testStrings();
    testStringObjects();
    testCollectionObjects();
    testBadPayloads();
    testFileRoundTrip();
    return mnemos::test::summarise("rdb");
}
