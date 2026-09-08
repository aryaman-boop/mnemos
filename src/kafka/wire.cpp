#include "kafka/wire.h"

#include <cstring>

namespace mnemos::kafka {

// ---------------------------------------------------------------------- Reader

bool Reader::need(std::size_t n) {
    if (!ok_) return false;
    if (buf_.size() - pos_ < n) {
        ok_ = false;
        return false;
    }
    return true;
}

std::int8_t Reader::int8() {
    if (!need(1)) return 0;
    return static_cast<std::int8_t>(static_cast<unsigned char>(buf_[pos_++]));
}

std::int16_t Reader::int16() {
    if (!need(2)) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(buf_.data() + pos_);
    pos_ += 2;
    return static_cast<std::int16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::int32_t Reader::int32() {
    return static_cast<std::int32_t>(uint32());
}

std::uint32_t Reader::uint32() {
    if (!need(4)) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(buf_.data() + pos_);
    pos_ += 4;
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::int64_t Reader::int64() {
    if (!need(8)) return 0;
    const auto*   p = reinterpret_cast<const unsigned char*>(buf_.data() + pos_);
    pos_ += 8;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return static_cast<std::int64_t>(v);
}

std::int64_t Reader::varlong() {
    std::uint64_t raw   = 0;
    int           shift = 0;
    while (true) {
        if (!need(1)) return 0;
        const auto byte = static_cast<unsigned char>(buf_[pos_++]);
        raw |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        // Ten groups of seven bits is the most a 64-bit value can occupy. An
        // eleventh continuation byte is a malformed stream, not a big number.
        if (shift > 63) {
            ok_ = false;
            return 0;
        }
    }
    return static_cast<std::int64_t>(raw >> 1) ^ -static_cast<std::int64_t>(raw & 1);
}

std::int32_t Reader::varint() {
    const std::int64_t v = varlong();
    if (v < INT32_MIN || v > INT32_MAX) {
        ok_ = false;
        return 0;
    }
    return static_cast<std::int32_t>(v);
}

std::string Reader::string() {
    std::string out;
    nullableString(out);
    return out;
}

bool Reader::nullableString(std::string& out) {
    out.clear();
    const std::int16_t len = int16();
    if (!ok_ || len < 0) return false;
    if (!need(static_cast<std::size_t>(len))) return false;
    out.assign(buf_.data() + pos_, static_cast<std::size_t>(len));
    pos_ += static_cast<std::size_t>(len);
    return true;
}

bool Reader::nullableBytes(std::string_view& out) {
    out = {};
    const std::int32_t len = int32();
    if (!ok_ || len < 0) return false;
    if (!need(static_cast<std::size_t>(len))) return false;
    out = buf_.substr(pos_, static_cast<std::size_t>(len));
    pos_ += static_cast<std::size_t>(len);
    return true;
}

std::int32_t Reader::arrayLen(bool* was_null) {
    const std::int32_t n = int32();
    if (was_null) *was_null = (n < 0);
    if (n < 0) return 0;
    // One element cannot be smaller than a byte, so a count larger than what is
    // left is a malformed frame -- and rejecting it here stops a bogus length
    // from driving a huge reserve in the caller's loop.
    if (static_cast<std::size_t>(n) > remaining()) {
        ok_ = false;
        return 0;
    }
    return n;
}

void Reader::skip(std::size_t n) {
    if (!need(n)) return;
    pos_ += n;
}

bool Reader::takeView(std::size_t n, std::string_view& out) {
    out = {};
    if (!need(n)) return false;
    out = buf_.substr(pos_, n);
    pos_ += n;
    return true;
}

bool Reader::varintBytes(std::string_view& out, bool& is_null) {
    out     = {};
    is_null = false;
    const std::int32_t len = varint();
    if (!ok_) return false;
    if (len < 0) {
        is_null = true;
        return true;
    }
    return takeView(static_cast<std::size_t>(len), out);
}

// ---------------------------------------------------------------------- Writer

void Writer::int8(std::int8_t v) {
    buf_.push_back(static_cast<char>(v));
}

void Writer::int16(std::int16_t v) {
    const auto u = static_cast<std::uint16_t>(v);
    buf_.push_back(static_cast<char>((u >> 8) & 0xff));
    buf_.push_back(static_cast<char>(u & 0xff));
}

void Writer::int32(std::int32_t v) {
    uint32(static_cast<std::uint32_t>(v));
}

void Writer::uint32(std::uint32_t u) {
    buf_.push_back(static_cast<char>((u >> 24) & 0xff));
    buf_.push_back(static_cast<char>((u >> 16) & 0xff));
    buf_.push_back(static_cast<char>((u >> 8) & 0xff));
    buf_.push_back(static_cast<char>(u & 0xff));
}

void Writer::int64(std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    for (int i = 7; i >= 0; --i) buf_.push_back(static_cast<char>((u >> (i * 8)) & 0xff));
}

void Writer::varlong(std::int64_t v) {
    std::uint64_t raw = (static_cast<std::uint64_t>(v) << 1) ^
                        static_cast<std::uint64_t>(v >> 63);
    while (raw >= 0x80) {
        buf_.push_back(static_cast<char>((raw & 0x7f) | 0x80));
        raw >>= 7;
    }
    buf_.push_back(static_cast<char>(raw));
}

void Writer::varint(std::int32_t v) {
    varlong(v);
}

void Writer::string(std::string_view v) {
    int16(static_cast<std::int16_t>(v.size()));
    buf_.append(v);
}

void Writer::nullString() {
    int16(-1);
}

void Writer::nullableString(const std::string& v, bool is_null) {
    if (is_null) {
        nullString();
        return;
    }
    string(v);
}

void Writer::bytes(std::string_view v) {
    int32(static_cast<std::int32_t>(v.size()));
    buf_.append(v);
}

void Writer::nullBytes() {
    int32(-1);
}

std::size_t Writer::reserveInt32() {
    const std::size_t at = buf_.size();
    int32(0);
    return at;
}

void Writer::patchInt32(std::size_t at, std::int32_t v) {
    patchUint32(at, static_cast<std::uint32_t>(v));
}

void Writer::patchUint32(std::size_t at, std::uint32_t u) {
    buf_[at]     = static_cast<char>((u >> 24) & 0xff);
    buf_[at + 1] = static_cast<char>((u >> 16) & 0xff);
    buf_[at + 2] = static_cast<char>((u >> 8) & 0xff);
    buf_[at + 3] = static_cast<char>(u & 0xff);
}

// ---------------------------------------------------------------------- header

bool parseRequestHeader(Reader& r, RequestHeader& out) {
    out.api_key        = r.int16();
    out.api_version    = r.int16();
    out.correlation_id = r.int32();
    // Header v1 adds a nullable client_id, and every version of every API we
    // support uses v1 on the request side. A null one is not an error.
    r.nullableString(out.client_id);
    return r.ok();
}

}  // namespace mnemos::kafka
