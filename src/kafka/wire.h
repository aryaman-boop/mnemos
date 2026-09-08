// The Kafka wire protocol's primitive types, and the framing around a request.
//
// Kafka's encoding is big-endian and length-prefixed, with two families of
// every composite type: the classic one (int16 string lengths, int32 array
// counts) and the "flexible" one introduced alongside tagged fields (unsigned
// varint lengths, biased by one, plus a tag section on every struct).
//
// mnemos-kafka speaks only the classic family. That is not a shortcut around a
// hard part -- it is the whole reason the broker fits in one file per concern.
// A client discovers what a broker supports through ApiVersions and negotiates
// down, so advertising a maximum below each API's first flexible version means
// no tagged fields and no compact types ever reach the wire. `kApis` in
// broker.cpp is where those maxima live, and every one of them is one below the
// version that turned flexible.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mnemos::kafka {

// The API keys we answer. A request naming anything else cannot be answered at
// all -- the response schema is what we do not know -- so the broker closes the
// connection, which is what a real broker does with an unparseable request.
enum class ApiKey : std::int16_t {
    Produce     = 0,
    Fetch       = 1,
    ListOffsets = 2,
    Metadata    = 3,
    ApiVersions = 18,
};

// Error codes are protocol constants: a client switches on the number, so these
// must be Kafka's own values and not a private numbering.
enum class ErrorCode : std::int16_t {
    None                      = 0,
    OffsetOutOfRange          = 1,
    UnknownTopicOrPartition   = 3,
    UnsupportedVersion        = 35,
    InvalidRequest            = 42,
    KafkaStorageError         = 56,
    UnsupportedCompression    = 76,
};

// Bounds-checked big-endian reader. Every read that would run past the end sets
// the failure flag and returns a zero value, so a caller may decode a whole
// request and check `ok()` once at the end rather than after each field.
class Reader {
public:
    explicit Reader(std::string_view buf) : buf_(buf) {}

    bool        ok() const { return ok_; }
    std::size_t remaining() const { return ok_ ? buf_.size() - pos_ : 0; }
    std::size_t position() const { return pos_; }

    std::int8_t  int8();
    std::int16_t int16();
    std::int32_t int32();
    std::int64_t int64();
    std::uint32_t uint32();
    bool         boolean() { return int8() != 0; }

    // Zigzag varint, as used inside a record batch (and nowhere outside one).
    std::int32_t varint();
    std::int64_t varlong();

    // int16-prefixed. A length of -1 is null, which `string()` renders as empty
    // -- use `nullableString` where the difference carries meaning.
    std::string string();
    bool        nullableString(std::string& out);  // false => null

    // int32-prefixed. Returns a view into the reader's buffer, so it stays
    // valid only as long as the buffer does.
    bool nullableBytes(std::string_view& out);  // false => null

    // int32 count. -1 (a null array) reads back as 0 with `was_null` set, since
    // every array we consume treats the two the same.
    std::int32_t arrayLen(bool* was_null = nullptr);

    // Skips `n` bytes, failing rather than clamping.
    void skip(std::size_t n);

    // Consumes `n` bytes and hands back a view of them. The view aliases the
    // reader's buffer, which is what keeps record decoding copy-free: a
    // sub-Reader over the result parses one record without owning its bytes.
    bool takeView(std::size_t n, std::string_view& out);

    // A zigzag-varint length followed by that many bytes -- how a record's key,
    // value and header values are framed. A negative length is null, reported
    // through `is_null` rather than as a failure.
    bool varintBytes(std::string_view& out, bool& is_null);

private:
    bool need(std::size_t n);

    std::string_view buf_;
    std::size_t      pos_ = 0;
    bool             ok_  = true;
};

// Big-endian writer over a growing buffer. Length-prefixed structures are built
// with `reserveInt32` and `patchInt32`: Kafka puts several lengths (the request
// size, a record batch's length, its CRC) before content whose size is not
// known until it has been written.
class Writer {
public:
    void int8(std::int8_t v);
    void int16(std::int16_t v);
    void int32(std::int32_t v);
    void int64(std::int64_t v);
    void uint32(std::uint32_t v);
    void boolean(bool v) { int8(v ? 1 : 0); }

    void varint(std::int32_t v);
    void varlong(std::int64_t v);

    void string(std::string_view v);
    void nullString();
    void nullableString(const std::string& v, bool is_null);
    void bytes(std::string_view v);
    void nullBytes();
    void raw(std::string_view v) { buf_.append(v); }

    void arrayLen(std::int32_t n) { int32(n); }
    void errorCode(ErrorCode e) { int16(static_cast<std::int16_t>(e)); }

    // Writes a placeholder int32 and returns its offset, for patchInt32 later.
    std::size_t reserveInt32();
    void        patchInt32(std::size_t at, std::int32_t v);
    void        patchUint32(std::size_t at, std::uint32_t v);

    std::size_t        size() const { return buf_.size(); }
    const std::string& str() const { return buf_; }
    std::string        take() { return std::move(buf_); }
    std::string_view   view(std::size_t from) const {
        return std::string_view(buf_).substr(from);
    }

private:
    std::string buf_;
};

// A decoded request header. Header v1 is what every API we support uses on the
// request side: v2 is the flexible one, and we never advertise a flexible
// version. `client_id` is nullable and we keep it only for logging.
struct RequestHeader {
    std::int16_t api_key        = 0;
    std::int16_t api_version    = 0;
    std::int32_t correlation_id = 0;
    std::string  client_id;
};

// Parses the header off the front of one framed request body (the four-byte
// length prefix already removed). Leaves the reader positioned at the payload.
bool parseRequestHeader(Reader& r, RequestHeader& out);

}  // namespace mnemos::kafka
