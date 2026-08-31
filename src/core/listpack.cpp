#include "core/listpack.h"

#include <cstring>
#include <limits>

#include "core/object.h"

namespace mnemos::core {
namespace {

// Encoding-byte discriminators, straight from listpack.c.
constexpr std::uint8_t kEnc7BitUintMask  = 0x80, kEnc7BitUint  = 0x00;  // 0xxxxxxx
constexpr std::uint8_t kEnc6BitStrMask   = 0xC0, kEnc6BitStr   = 0x80;  // 10xxxxxx
constexpr std::uint8_t kEnc13BitIntMask  = 0xE0, kEnc13BitInt  = 0xC0;  // 110xxxxx
constexpr std::uint8_t kEnc12BitStrMask  = 0xF0, kEnc12BitStr  = 0xE0;  // 1110xxxx
constexpr std::uint8_t kEnc32BitStr      = 0xF0;
constexpr std::uint8_t kEnc16BitInt      = 0xF1;
constexpr std::uint8_t kEnc24BitInt      = 0xF2;
constexpr std::uint8_t kEnc32BitInt      = 0xF3;
constexpr std::uint8_t kEnc64BitInt      = 0xF4;

void writeLE16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}
void writeLE32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}
std::uint16_t readLE16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t readLE32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Serialises `value` into the smallest integer encoding that fits.
std::vector<std::uint8_t> encodeInteger(std::int64_t value) {
    std::vector<std::uint8_t> out;
    if (value >= 0 && value <= 127) {
        out.push_back(static_cast<std::uint8_t>(value));  // 0xxxxxxx
    } else if (value >= -4096 && value <= 4095) {
        // 13-bit two's complement split across the low 5 bits of byte 0 and all
        // of byte 1.
        const std::uint16_t raw = static_cast<std::uint16_t>(value) & 0x1FFF;
        out.push_back(static_cast<std::uint8_t>(kEnc13BitInt | ((raw >> 8) & 0x1F)));
        out.push_back(static_cast<std::uint8_t>(raw & 0xFF));
    } else if (value >= -32768 && value <= 32767) {
        out.resize(3);
        out[0] = kEnc16BitInt;
        writeLE16(out.data() + 1, static_cast<std::uint16_t>(value));
    } else if (value >= -8388608 && value <= 8388607) {
        out.resize(4);
        out[0] = kEnc24BitInt;
        const auto raw = static_cast<std::uint32_t>(value);
        out[1] = static_cast<std::uint8_t>(raw & 0xFF);
        out[2] = static_cast<std::uint8_t>((raw >> 8) & 0xFF);
        out[3] = static_cast<std::uint8_t>((raw >> 16) & 0xFF);
    } else if (value >= -2147483648LL && value <= 2147483647LL) {
        out.resize(5);
        out[0] = kEnc32BitInt;
        writeLE32(out.data() + 1, static_cast<std::uint32_t>(value));
    } else {
        out.resize(9);
        out[0] = kEnc64BitInt;
        const auto raw = static_cast<std::uint64_t>(value);
        for (int i = 0; i < 8; ++i) {
            out[1 + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((raw >> (8 * i)) & 0xFF);
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeString(std::string_view value) {
    std::vector<std::uint8_t> out;
    const std::size_t len = value.size();
    if (len < 64) {
        out.push_back(static_cast<std::uint8_t>(kEnc6BitStr | len));
    } else if (len < 4096) {
        out.push_back(static_cast<std::uint8_t>(kEnc12BitStr | ((len >> 8) & 0x0F)));
        out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    } else {
        out.resize(5);
        out[0] = kEnc32BitStr;
        writeLE32(out.data() + 1, static_cast<std::uint32_t>(len));
    }
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

// Builds the complete on-wire entry: encoding+data followed by its backlen.
std::vector<std::uint8_t> buildEntry(std::string_view value) {
    std::int64_t as_integer = 0;
    std::vector<std::uint8_t> payload = stringToInt64(value, as_integer)
                                            ? encodeInteger(as_integer)
                                            : encodeString(value);
    std::uint8_t backlen[5];
    const int backlen_size = encodeBacklen(payload.size(), backlen);
    payload.insert(payload.end(), backlen, backlen + backlen_size);
    return payload;
}

std::vector<std::uint8_t> buildIntegerEntry(std::int64_t value) {
    std::vector<std::uint8_t> payload = encodeInteger(value);
    std::uint8_t backlen[5];
    const int backlen_size = encodeBacklen(payload.size(), backlen);
    payload.insert(payload.end(), backlen, backlen + backlen_size);
    return payload;
}

}  // namespace

std::string Listpack::Element::toString() const {
    return is_integer ? std::to_string(integer) : std::string(string);
}

int encodeBacklen(std::uint64_t value, std::uint8_t* out) {
    // Most significant group first; every byte after the first carries bit 7 so
    // a backwards reader knows where to stop.
    if (value <= 127) {
        out[0] = static_cast<std::uint8_t>(value);
        return 1;
    }
    if (value < 16384) {
        out[0] = static_cast<std::uint8_t>(value >> 7);
        out[1] = static_cast<std::uint8_t>((value & 127) | 128);
        return 2;
    }
    if (value < 2097152) {
        out[0] = static_cast<std::uint8_t>(value >> 14);
        out[1] = static_cast<std::uint8_t>(((value >> 7) & 127) | 128);
        out[2] = static_cast<std::uint8_t>((value & 127) | 128);
        return 3;
    }
    if (value < 268435456) {
        out[0] = static_cast<std::uint8_t>(value >> 21);
        out[1] = static_cast<std::uint8_t>(((value >> 14) & 127) | 128);
        out[2] = static_cast<std::uint8_t>(((value >> 7) & 127) | 128);
        out[3] = static_cast<std::uint8_t>((value & 127) | 128);
        return 4;
    }
    out[0] = static_cast<std::uint8_t>(value >> 28);
    out[1] = static_cast<std::uint8_t>(((value >> 21) & 127) | 128);
    out[2] = static_cast<std::uint8_t>(((value >> 14) & 127) | 128);
    out[3] = static_cast<std::uint8_t>(((value >> 7) & 127) | 128);
    out[4] = static_cast<std::uint8_t>((value & 127) | 128);
    return 5;
}

std::uint64_t decodeBacklenBackwards(const std::uint8_t* last, int& bytes_read) {
    std::uint64_t value = 0;
    std::uint64_t shift = 0;
    bytes_read = 0;
    while (true) {
        value |= static_cast<std::uint64_t>(*last & 127) << shift;
        ++bytes_read;
        if (!(*last & 128)) break;  // first byte reached
        shift += 7;
        --last;
        if (shift > 28) {
            bytes_read = 0;
            return 0;
        }
    }
    return value;
}

Listpack::Listpack() {
    buffer_.resize(kListpackHeaderSize + 1);
    setTotalBytes(static_cast<std::uint32_t>(buffer_.size()));
    setNumElements(0);
    buffer_.back() = kListpackEof;
}

void Listpack::setTotalBytes(std::uint32_t value) { writeLE32(buffer_.data(), value); }

void Listpack::setNumElements(std::size_t value) {
    writeLE16(buffer_.data() + 4,
              static_cast<std::uint16_t>(std::min<std::size_t>(value, kListpackMaxInlineCount)));
}

std::uint16_t Listpack::storedCount() const { return readLE16(buffer_.data() + 4); }

std::size_t Listpack::numElements() const {
    const std::uint16_t stored = storedCount();
    if (stored < kListpackMaxInlineCount) return stored;
    // The header saturated, so the true count has to be recovered by walking.
    std::size_t count = 0;
    forEach([&count](const Element&) { ++count; });
    return count;
}

std::size_t Listpack::decodeAt(std::size_t offset, Element& out) const {
    if (offset >= buffer_.size()) return 0;
    const std::uint8_t* p = buffer_.data() + offset;
    const std::size_t remaining = buffer_.size() - offset;
    const std::uint8_t encoding = *p;

    std::size_t payload_size = 0;  // encoding bytes + data bytes

    if ((encoding & kEnc7BitUintMask) == kEnc7BitUint) {
        out.is_integer = true;
        out.integer    = encoding & 0x7F;
        payload_size   = 1;
    } else if ((encoding & kEnc6BitStrMask) == kEnc6BitStr) {
        const std::size_t len = encoding & 0x3F;
        if (remaining < 1 + len) return 0;
        out.is_integer = false;
        out.string     = std::string_view(reinterpret_cast<const char*>(p + 1), len);
        payload_size   = 1 + len;
    } else if ((encoding & kEnc13BitIntMask) == kEnc13BitInt) {
        if (remaining < 2) return 0;
        std::uint16_t raw = static_cast<std::uint16_t>(((encoding & 0x1F) << 8) | p[1]);
        // Sign-extend from 13 bits.
        std::int64_t value = raw;
        if (value >= 4096) value -= 8192;
        out.is_integer = true;
        out.integer    = value;
        payload_size   = 2;
    } else if ((encoding & kEnc12BitStrMask) == kEnc12BitStr) {
        if (remaining < 2) return 0;
        const std::size_t len = static_cast<std::size_t>(((encoding & 0x0F) << 8) | p[1]);
        if (remaining < 2 + len) return 0;
        out.is_integer = false;
        out.string     = std::string_view(reinterpret_cast<const char*>(p + 2), len);
        payload_size   = 2 + len;
    } else if (encoding == kEnc32BitStr) {
        if (remaining < 5) return 0;
        const std::size_t len = readLE32(p + 1);
        if (remaining < 5 + len) return 0;
        out.is_integer = false;
        out.string     = std::string_view(reinterpret_cast<const char*>(p + 5), len);
        payload_size   = 5 + len;
    } else if (encoding == kEnc16BitInt) {
        if (remaining < 3) return 0;
        out.is_integer = true;
        out.integer    = static_cast<std::int16_t>(readLE16(p + 1));
        payload_size   = 3;
    } else if (encoding == kEnc24BitInt) {
        if (remaining < 4) return 0;
        std::uint32_t raw = static_cast<std::uint32_t>(p[1]) |
                            (static_cast<std::uint32_t>(p[2]) << 8) |
                            (static_cast<std::uint32_t>(p[3]) << 16);
        std::int64_t value = raw;
        if (value >= 8388608) value -= 16777216;  // sign-extend from 24 bits
        out.is_integer = true;
        out.integer    = value;
        payload_size   = 4;
    } else if (encoding == kEnc32BitInt) {
        if (remaining < 5) return 0;
        out.is_integer = true;
        out.integer    = static_cast<std::int32_t>(readLE32(p + 1));
        payload_size   = 5;
    } else if (encoding == kEnc64BitInt) {
        if (remaining < 9) return 0;
        std::uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) {
            raw |= static_cast<std::uint64_t>(p[1 + static_cast<std::size_t>(i)]) << (8 * i);
        }
        out.is_integer = true;
        out.integer    = static_cast<std::int64_t>(raw);
        payload_size   = 9;
    } else {
        return 0;  // EOF byte or corruption
    }

    std::uint8_t backlen[5];
    const int backlen_size = encodeBacklen(payload_size, backlen);
    if (remaining < payload_size + static_cast<std::size_t>(backlen_size)) return 0;
    return payload_size + static_cast<std::size_t>(backlen_size);
}

std::size_t Listpack::offsetOf(std::size_t index) const {
    std::size_t offset = kListpackHeaderSize;
    std::size_t current = 0;
    while (offset < buffer_.size() && buffer_[offset] != kListpackEof) {
        if (current == index) return offset;
        Element scratch;
        const std::size_t size = decodeAt(offset, scratch);
        if (size == 0) return std::string::npos;
        offset += size;
        ++current;
    }
    return std::string::npos;
}

bool Listpack::get(std::size_t index, Element& out) const {
    const std::size_t offset = offsetOf(index);
    if (offset == std::string::npos) return false;
    return decodeAt(offset, out) != 0;
}

void Listpack::append(std::string_view value) {
    const std::vector<std::uint8_t> entry = buildEntry(value);
    // Splice in just before the EOF terminator.
    buffer_.insert(buffer_.end() - 1, entry.begin(), entry.end());
    setTotalBytes(static_cast<std::uint32_t>(buffer_.size()));
    const std::uint16_t stored = storedCount();
    if (stored < kListpackMaxInlineCount) setNumElements(stored + 1);
}

void Listpack::appendInteger(std::int64_t value) {
    const std::vector<std::uint8_t> entry = buildIntegerEntry(value);
    buffer_.insert(buffer_.end() - 1, entry.begin(), entry.end());
    setTotalBytes(static_cast<std::uint32_t>(buffer_.size()));
    const std::uint16_t stored = storedCount();
    if (stored < kListpackMaxInlineCount) setNumElements(stored + 1);
}

std::optional<std::size_t> Listpack::find(std::string_view value, std::size_t start,
                                          std::size_t step) const {
    std::int64_t needle_int = 0;
    const bool needle_is_int = stringToInt64(value, needle_int);

    std::size_t offset = kListpackHeaderSize;
    std::size_t index  = 0;

    while (offset < buffer_.size() && buffer_[offset] != kListpackEof) {
        Element element;
        const std::size_t size = decodeAt(offset, element);
        if (size == 0) return std::nullopt;

        if (index >= start && (index - start) % step == 0) {
            // An integer-encoded element and a numeric needle compare
            // numerically; otherwise fall back to byte comparison. Without this,
            // searching for "007" would wrongly match a stored 7.
            const bool matches = element.is_integer
                                     ? (needle_is_int && element.integer == needle_int)
                                     : (!needle_is_int && element.string == value);
            if (matches) return index;
        }
        offset += size;
        ++index;
    }
    return std::nullopt;
}

bool Listpack::eraseRange(std::size_t index, std::size_t count) {
    if (count == 0) return true;
    const std::size_t start_offset = offsetOf(index);
    if (start_offset == std::string::npos) return false;

    std::size_t end_offset = start_offset;
    for (std::size_t i = 0; i < count; ++i) {
        if (end_offset >= buffer_.size() || buffer_[end_offset] == kListpackEof) return false;
        Element scratch;
        const std::size_t size = decodeAt(end_offset, scratch);
        if (size == 0) return false;
        end_offset += size;
    }

    buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(start_offset),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(end_offset));
    setTotalBytes(static_cast<std::uint32_t>(buffer_.size()));

    const std::uint16_t stored = storedCount();
    if (stored < kListpackMaxInlineCount) {
        setNumElements(stored >= count ? stored - count : 0);
    } else {
        setNumElements(numElements());
    }
    return true;
}

bool Listpack::eraseAt(std::size_t index) { return eraseRange(index, 1); }

bool Listpack::replaceAt(std::size_t index, std::string_view value) {
    const std::size_t offset = offsetOf(index);
    if (offset == std::string::npos) return false;
    Element scratch;
    const std::size_t old_size = decodeAt(offset, scratch);
    if (old_size == 0) return false;

    const std::vector<std::uint8_t> entry = buildEntry(value);
    buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(offset),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset + old_size));
    buffer_.insert(buffer_.begin() + static_cast<std::ptrdiff_t>(offset), entry.begin(),
                   entry.end());
    setTotalBytes(static_cast<std::uint32_t>(buffer_.size()));
    return true;
}

bool Listpack::insertAt(std::size_t index, std::string_view value) {
    std::size_t offset;
    if (index == numElements()) {
        offset = buffer_.size() - 1;  // just before EOF
    } else {
        offset = offsetOf(index);
        if (offset == std::string::npos) return false;
    }

    const std::vector<std::uint8_t> entry = buildEntry(value);
    buffer_.insert(buffer_.begin() + static_cast<std::ptrdiff_t>(offset), entry.begin(),
                   entry.end());
    setTotalBytes(static_cast<std::uint32_t>(buffer_.size()));
    const std::uint16_t stored = storedCount();
    if (stored < kListpackMaxInlineCount) setNumElements(stored + 1);
    return true;
}

void Listpack::clear() {
    buffer_.assign(kListpackHeaderSize + 1, 0);
    buffer_.back() = kListpackEof;
    setTotalBytes(static_cast<std::uint32_t>(buffer_.size()));
    setNumElements(0);
}

std::vector<std::string> Listpack::toStrings() const {
    std::vector<std::string> out;
    forEach([&out](const Element& e) { out.push_back(e.toString()); });
    return out;
}

std::optional<Listpack> Listpack::fromBytes(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kListpackHeaderSize + 1) return std::nullopt;

    Listpack lp;
    lp.buffer_.assign(bytes.begin(), bytes.end());

    // The declared size must match reality, and the last byte must terminate.
    const std::uint32_t declared = readLE32(lp.buffer_.data());
    if (declared != lp.buffer_.size()) return std::nullopt;
    if (lp.buffer_.back() != kListpackEof) return std::nullopt;

    // Walk every entry so malformed input is rejected here rather than later,
    // deep inside a command handler.
    std::size_t offset = kListpackHeaderSize;
    std::size_t counted = 0;
    while (offset < lp.buffer_.size() && lp.buffer_[offset] != kListpackEof) {
        Element scratch;
        const std::size_t size = lp.decodeAt(offset, scratch);
        if (size == 0) return std::nullopt;
        offset += size;
        ++counted;
    }
    if (offset != lp.buffer_.size() - 1) return std::nullopt;

    const std::uint16_t stored = lp.storedCount();
    if (stored < kListpackMaxInlineCount && stored != counted) return std::nullopt;
    return lp;
}

}  // namespace mnemos::core
