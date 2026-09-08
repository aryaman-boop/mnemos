// Kafka wire and record-batch tests.
//
// This is the layer where a mistake is silent: a wrong varint or a CRC over the
// wrong span produces bytes that are structurally fine and that no client will
// explain to you. The record batch round trip below is the load-bearing case --
// it asserts against an independently written expectation, not just against
// this file's own encoder.
#include <string>
#include <vector>

#include "kafka/records.h"
#include "kafka/wire.h"
#include "test_harness.h"

using namespace mnemos::kafka;

namespace {

void testPrimitives() {
    Writer w;
    w.int8(-1);
    w.int16(-2);
    w.int32(-3);
    w.int64(-4);
    w.string("hello");
    w.nullString();
    w.bytes("ab");
    w.nullBytes();
    w.boolean(true);

    Reader r(w.str());
    CHECK_EQ(static_cast<int>(r.int8()), -1);
    CHECK_EQ(static_cast<int>(r.int16()), -2);
    CHECK_EQ(r.int32(), -3);
    CHECK_EQ(r.int64(), static_cast<std::int64_t>(-4));

    std::string s;
    CHECK(r.nullableString(s));
    CHECK_EQ(s, std::string("hello"));
    CHECK(!r.nullableString(s));  // the null one
    CHECK_EQ(s, std::string(""));

    std::string_view view;
    CHECK(r.nullableBytes(view));
    CHECK_EQ(std::string(view), std::string("ab"));
    CHECK(!r.nullableBytes(view));
    CHECK_EQ(r.boolean(), true);
    CHECK(r.ok());
    CHECK_EQ(r.remaining(), static_cast<std::size_t>(0));
}

// Big-endian is the whole protocol's byte order, so pin one value literally
// rather than only round-tripping it.
void testByteOrder() {
    Writer w;
    w.int32(0x01020304);
    CHECK_EQ(w.str(), std::string("\x01\x02\x03\x04", 4));

    Writer v;
    v.int16(1);
    CHECK_EQ(v.str(), std::string("\x00\x01", 2));
}

void testVarints() {
    // Zigzag: 0,-1,1,-2,2 encode as 0,1,2,3,4 -- one byte each, and the sign
    // does not cost five bytes the way a plain two's-complement varint would.
    const std::vector<std::int64_t> values = {0,        -1,      1,       -2,
                                              2,        63,      64,      -64,
                                              -65,      2147483647, -2147483648LL,
                                              9223372036854775807LL,
                                              -9223372036854775807LL - 1};
    for (const auto value : values) {
        Writer w;
        w.varlong(value);
        Reader r(w.str());
        CHECK_EQ(r.varlong(), value);
        CHECK(r.ok());
        CHECK_EQ(r.remaining(), static_cast<std::size_t>(0));
    }

    Writer one;
    one.varint(0);
    CHECK_EQ(one.str(), std::string("\x00", 1));
    Writer minus_one;
    minus_one.varint(-1);
    CHECK_EQ(minus_one.str(), std::string("\x01", 1));
    Writer plus_one;
    plus_one.varint(1);
    CHECK_EQ(plus_one.str(), std::string("\x02", 1));

    // A varint that never terminates must fail rather than run off the buffer.
    // The Reader borrows its buffer, so the bytes have to outlive it.
    const std::string all_continuations(12, '\xff');
    Reader            runaway(all_continuations);
    runaway.varlong();
    CHECK(!runaway.ok());
}

void testReaderBounds() {
    const std::string short_string("\x00\x05", 2);  // claims five bytes, supplies none
    Reader            r(short_string);
    std::string       s;
    CHECK(!r.nullableString(s));
    CHECK(!r.ok());

    // A bogus array count must not be believed: it is larger than what is left.
    const std::string huge_count("\x7f\xff\xff\xff", 4);
    Reader            array(huge_count);
    array.arrayLen();
    CHECK(!array.ok());
}

void testCrc32c() {
    // The check value every CRC-32C implementation agrees on.
    CHECK_EQ(crc32c(0, "123456789", 9), static_cast<std::uint32_t>(0xe3069283));
    CHECK_EQ(crc32c(0, "", 0), static_cast<std::uint32_t>(0));
    CHECK_EQ(crc32c(0, "a", 1), static_cast<std::uint32_t>(0xc1d04330));
}

Record makeRecord(const std::string& key, const std::string& value, std::int64_t ts) {
    Record r;
    r.timestamp  = ts;
    r.key        = key;
    r.key_null   = false;
    r.value      = value;
    r.value_null = false;
    return r;
}

void testRecordBatchRoundTrip() {
    std::vector<Record> in;
    in.push_back(makeRecord("k1", "v1", 1700000000000LL));
    in.push_back(makeRecord("k2", "v2", 1700000000005LL));
    in.back().headers.push_back({"h", "hv", false});
    in.push_back(makeRecord("", "", 1700000000009LL));

    Record null_key;
    null_key.timestamp = 1700000000010LL;
    null_key.value     = "tombstoned";
    null_key.value_null = false;
    in.push_back(null_key);

    const std::string encoded = encodeRecordBatch(100, in);

    // The header is fixed width, so the fields a consumer reads before any
    // record must sit where it looks for them.
    Reader header(encoded);
    CHECK_EQ(header.int64(), static_cast<std::int64_t>(100));  // base offset
    CHECK_EQ(header.int32(), static_cast<std::int32_t>(encoded.size() - 12));
    CHECK_EQ(header.int32(), 0);                               // leader epoch
    CHECK_EQ(static_cast<int>(header.int8()), 2);              // magic

    std::vector<Record> out;
    CHECK(decodeRecordBatches(encoded, out) == ErrorCode::None);
    CHECK_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < out.size() && i < in.size(); ++i) {
        CHECK_EQ(out[i].key, in[i].key);
        CHECK_EQ(out[i].key_null, in[i].key_null);
        CHECK_EQ(out[i].value, in[i].value);
        CHECK_EQ(out[i].value_null, in[i].value_null);
        CHECK_EQ(out[i].timestamp, in[i].timestamp);
        CHECK_EQ(out[i].headers.size(), in[i].headers.size());
    }
    CHECK_EQ(out[1].headers[0].key, std::string("h"));
    CHECK_EQ(out[1].headers[0].value, std::string("hv"));
    CHECK_EQ(out[3].key_null, true);
}

void testConcatenatedBatches() {
    // A Produce request may carry several batches back to back, and the batch
    // length -- not the record count -- is what separates them.
    std::vector<Record> first{makeRecord("a", "1", 10)};
    std::vector<Record> second{makeRecord("b", "2", 20), makeRecord("c", "3", 30)};

    const std::string blob = encodeRecordBatch(0, first) + encodeRecordBatch(1, second);
    std::vector<Record> out;
    CHECK(decodeRecordBatches(blob, out) == ErrorCode::None);
    CHECK_EQ(out.size(), static_cast<std::size_t>(3));
    CHECK_EQ(out[0].value, std::string("1"));
    CHECK_EQ(out[2].value, std::string("3"));
}

void testCorruptBatches() {
    std::vector<Record> records{makeRecord("k", "v", 7)};
    const std::string   good = encodeRecordBatch(0, records);
    std::vector<Record> out;

    // A flipped payload byte must be caught by the CRC, not by the parse.
    std::string flipped = good;
    flipped[flipped.size() - 1] = static_cast<char>(flipped.back() ^ 0xff);
    out.clear();
    CHECK(decodeRecordBatches(flipped, out) == ErrorCode::KafkaStorageError);

    // A compression codec in the low three bits of the attributes: declined
    // rather than misread. Attributes sit at offset 21, and the CRC covers
    // them, so it has to be recomputed for the batch to reach that check.
    std::string compressed = good;
    compressed[22] = static_cast<char>(compressed[22] | 0x02);  // snappy
    const std::uint32_t crc = crc32c(0, compressed.data() + 21, compressed.size() - 21);
    Writer patch;
    patch.uint32(crc);
    compressed.replace(17, 4, patch.str());
    out.clear();
    CHECK(decodeRecordBatches(compressed, out) == ErrorCode::UnsupportedCompression);

    // Truncation is a malformed request: batch_len names bytes that are absent.
    out.clear();
    CHECK(decodeRecordBatches(good.substr(0, good.size() - 3), out) == ErrorCode::InvalidRequest);

    // A magic byte we do not speak -- v0/v1 message sets -- is refused whole.
    std::string legacy = good;
    legacy[16] = 1;
    out.clear();
    CHECK(decodeRecordBatches(legacy, out) == ErrorCode::InvalidRequest);
}

void testStoredRecord() {
    Record in = makeRecord("key", std::string("bin\0ary", 7), 42);
    in.headers.push_back({"h1", "v1", false});
    in.headers.push_back({"h2", "", true});

    Record out;
    CHECK(decodeStoredRecord(encodeStoredRecord(in), out));
    CHECK_EQ(out.key, in.key);
    CHECK_EQ(out.value, in.value);   // NUL survives: the log is binary safe
    CHECK_EQ(out.value.size(), static_cast<std::size_t>(7));
    CHECK_EQ(out.timestamp, static_cast<std::int64_t>(42));
    CHECK_EQ(out.headers.size(), static_cast<std::size_t>(2));
    CHECK_EQ(out.headers[1].value_null, true);

    Record tomb;
    tomb.timestamp = 1;
    tomb.key       = "k";
    tomb.key_null  = false;
    Record back;
    CHECK(decodeStoredRecord(encodeStoredRecord(tomb), back));
    CHECK_EQ(back.value_null, true);
    CHECK_EQ(back.key_null, false);

    CHECK(!decodeStoredRecord("", back));
    const std::string bad_version("\x09", 1);
    CHECK(!decodeStoredRecord(bad_version, back));  // unknown version
}

void testRequestHeader() {
    Writer w;
    w.int16(static_cast<std::int16_t>(ApiKey::Fetch));
    w.int16(11);
    w.int32(1234);
    w.string("mnemos-test");

    Reader        r(w.str());
    RequestHeader header;
    CHECK(parseRequestHeader(r, header));
    CHECK_EQ(header.api_key, static_cast<std::int16_t>(1));
    CHECK_EQ(header.api_version, static_cast<std::int16_t>(11));
    CHECK_EQ(header.correlation_id, 1234);
    CHECK_EQ(header.client_id, std::string("mnemos-test"));

    // A null client_id is ordinary, not an error.
    Writer anon;
    anon.int16(18);
    anon.int16(0);
    anon.int32(1);
    anon.nullString();
    Reader        r2(anon.str());
    RequestHeader h2;
    CHECK(parseRequestHeader(r2, h2));
    CHECK_EQ(h2.client_id, std::string(""));

    const std::string short_header("\x00\x01", 2);
    Reader            truncated(short_header);
    RequestHeader     h3;
    CHECK(!parseRequestHeader(truncated, h3));
}

}  // namespace

int main() {
    testPrimitives();
    testByteOrder();
    testVarints();
    testReaderBounds();
    testCrc32c();
    testRecordBatchRoundTrip();
    testConcatenatedBatches();
    testCorruptBatches();
    testStoredRecord();
    testRequestHeader();
    return mnemos::test::summarise("kafka_wire");
}
