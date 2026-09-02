#include "persist/rdb.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>

#include "core/encoding.h"

namespace mnemos::persist {
namespace {

// --- opcodes and type bytes -------------------------------------------------

constexpr std::uint8_t kOpIdle        = 0xF8;
constexpr std::uint8_t kOpFreq        = 0xF9;
constexpr std::uint8_t kOpAux         = 0xFA;
constexpr std::uint8_t kOpResizeDb    = 0xFB;
constexpr std::uint8_t kOpExpireMs    = 0xFC;
constexpr std::uint8_t kOpExpireSec   = 0xFD;
constexpr std::uint8_t kOpSelectDb    = 0xFE;
constexpr std::uint8_t kOpEof         = 0xFF;

constexpr std::uint8_t kTypeString      = 0;
constexpr std::uint8_t kTypeSet         = 2;
constexpr std::uint8_t kTypeZSet        = 3;   // scores as decimal text
constexpr std::uint8_t kTypeHash        = 4;
constexpr std::uint8_t kTypeZSet2       = 5;   // scores as binary doubles
constexpr std::uint8_t kTypeSetIntset   = 11;
constexpr std::uint8_t kTypeHashListpack = 16;
constexpr std::uint8_t kTypeZSetListpack = 17;
constexpr std::uint8_t kTypeListQuicklist2 = 18;
constexpr std::uint8_t kTypeSetListpack = 20;

// Quicklist node containers, as RDB_TYPE_LIST_QUICKLIST_2 spells them.
constexpr std::uint64_t kNodePlain  = 1;
constexpr std::uint64_t kNodePacked = 2;

// Redis's isLargeElement() at list-max-listpack-size -2: an element over the
// safety limit never goes into a listpack at all.
constexpr std::size_t kPlainNodeThreshold = 8192;

// Length prefixes. The top two bits of the first byte choose the shape.
constexpr std::uint8_t k6BitLen  = 0;
constexpr std::uint8_t k14BitLen = 1;
constexpr std::uint8_t kEncVal   = 3;
constexpr std::uint8_t k32BitLen = 0x80;
constexpr std::uint8_t k64BitLen = 0x81;

constexpr std::uint8_t kEncInt8  = 0;
constexpr std::uint8_t kEncInt16 = 1;
constexpr std::uint8_t kEncInt32 = 2;
constexpr std::uint8_t kEncLzf   = 3;

// --- CRC-64/Jones -----------------------------------------------------------

// The Jones polynomial, reflected. Redis's crc64 is a reflected CRC, so the
// table is built by shifting right against the reversed polynomial rather than
// left against the original.
constexpr std::uint64_t kCrcPolyReflected = 0x95ac9329ac4bc9b5ULL;

struct CrcTable {
    std::array<std::uint64_t, 256> entries{};
    constexpr CrcTable() {
        for (int i = 0; i < 256; ++i) {
            std::uint64_t crc = static_cast<std::uint64_t>(i);
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (kCrcPolyReflected & ~((crc & 1) - 1));
            }
            entries[static_cast<std::size_t>(i)] = crc;
        }
    }
};

constexpr CrcTable kCrcTable{};

// --- LZF --------------------------------------------------------------------
//
// A faithful port of liblzf's lzf_c.c/lzf_d.c as Redis configures it: HLOG 16,
// VERY_FAST on, ULTRA_FAST off, and INIT_HTAB on -- that last one is Redis's
// change, and it is what makes compression deterministic. Without it the hash
// table starts as whatever was on the stack, and two servers compressing the
// same string could disagree on the bytes.

constexpr int         kHashLog  = 16;
constexpr std::size_t kHashSize = 1u << kHashLog;
constexpr std::size_t kMaxLit   = 1u << 5;
constexpr std::size_t kMaxOff   = 1u << 13;
constexpr std::size_t kMaxRef   = (1u << 8) + (1u << 3);

inline std::uint32_t hashFirst(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
}
inline std::uint32_t hashNext(std::uint32_t v, const std::uint8_t* p) {
    return (v << 8) | p[2];
}
inline std::size_t hashIndex(std::uint32_t h) {
    // liblzf's VERY_FAST index, which is what Redis compiles. The `* 5` is not
    // decoration: with `- h` instead (liblzf's ULTRA_FAST) the hash table lands
    // matches in different slots and the compressed bytes stop matching a real
    // server's, one or two bytes at a time.
    return ((h >> (3 * 8 - kHashLog)) - h * 5) & (kHashSize - 1);
}

}  // namespace

std::uint64_t crc64(std::uint64_t crc, const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        crc = kCrcTable.entries[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

std::size_t lzfCompress(const std::uint8_t* in, std::size_t in_len,
                        std::uint8_t* out, std::size_t out_len) {
    if (in_len == 0 || out_len == 0) return 0;

    std::vector<std::uint32_t> htab(kHashSize, 0);

    const std::uint8_t* ip      = in;
    const std::uint8_t* in_end  = in + in_len;
    std::uint8_t*       op      = out;
    std::uint8_t* const out_end = out + out_len;

    std::size_t lit = 0;
    ++op;  // reserve the control byte of the first literal run

    if (in_len <= 2) {
        // Too short for the match loop, which needs a three-byte window.
        while (ip < in_end) {
            if (op >= out_end) return 0;
            ++lit;
            *op++ = *ip++;
        }
        op[-static_cast<std::ptrdiff_t>(lit) - 1] = static_cast<std::uint8_t>(lit - 1);
        return static_cast<std::size_t>(op - out);
    }

    std::uint32_t hval = hashFirst(ip);
    while (ip < in_end - 2) {
        hval = hashNext(hval, ip);
        std::uint32_t& slot = htab[hashIndex(hval)];
        const std::uint8_t* ref = in + slot;
        slot = static_cast<std::uint32_t>(ip - in);

        std::size_t off = 0;
        if (ref < ip && (off = static_cast<std::size_t>(ip - ref - 1)) < kMaxOff &&
            ref > in && ref[2] == ip[2] && ref[0] == ip[0] && ref[1] == ip[1]) {
            std::size_t len = 2;
            std::size_t maxlen = static_cast<std::size_t>(in_end - ip) - len;
            maxlen = maxlen > kMaxRef ? kMaxRef : maxlen;

            // Conservative test first, then the exact one: the run being closed
            // costs a byte only when it was non-empty.
            if (op + 3 + 1 >= out_end) {
                if (op - (lit ? 0 : 1) + 3 + 1 >= out_end) return 0;
            }

            op[-static_cast<std::ptrdiff_t>(lit) - 1] = static_cast<std::uint8_t>(lit - 1);
            if (lit == 0) --op;  // the run was empty; take its control byte back

            // liblzf unrolls the first sixteen comparisons and *leaves the loop*
            // from inside them, without consulting maxlen. When a match runs to
            // exactly the end of a 17- or 18-byte window that gives a length the
            // guarded loop below would have clipped -- a difference of one byte
            // in the output, so the unrolled form has to be reproduced and not
            // simplified away.
            bool unrolled_break = false;
            if (maxlen > 16) {
                for (int i = 0; i < 16; ++i) {
                    ++len;
                    if (ref[len] != ip[len]) { unrolled_break = true; break; }
                }
            }
            if (!unrolled_break) {
                do {
                    ++len;
                } while (len < maxlen && ref[len] == ip[len]);
            }

            len -= 2;  // the encoded length counts octets beyond the first two
            ++ip;

            if (len < 7) {
                *op++ = static_cast<std::uint8_t>((off >> 8) + (len << 5));
            } else {
                *op++ = static_cast<std::uint8_t>((off >> 8) + (7 << 5));
                *op++ = static_cast<std::uint8_t>(len - 7);
            }
            *op++ = static_cast<std::uint8_t>(off);

            lit = 0;
            ++op;  // start the next literal run

            ip += len + 1;
            if (ip >= in_end - 2) break;

            // VERY_FAST: re-index the two positions the match consumed, no more.
            ip -= 2;
            hval = hashFirst(ip);
            hval = hashNext(hval, ip);
            htab[hashIndex(hval)] = static_cast<std::uint32_t>(ip - in);
            ++ip;
            hval = hashNext(hval, ip);
            htab[hashIndex(hval)] = static_cast<std::uint32_t>(ip - in);
            ++ip;
        } else {
            if (op >= out_end) return 0;
            ++lit;
            *op++ = *ip++;
            if (lit == kMaxLit) {
                op[-static_cast<std::ptrdiff_t>(lit) - 1] =
                    static_cast<std::uint8_t>(lit - 1);
                lit = 0;
                ++op;
            }
        }
    }

    if (op + 3 > out_end) return 0;  // at most three bytes can still be missing

    while (ip < in_end) {
        ++lit;
        *op++ = *ip++;
        if (lit == kMaxLit) {
            op[-static_cast<std::ptrdiff_t>(lit) - 1] = static_cast<std::uint8_t>(lit - 1);
            lit = 0;
            ++op;
        }
    }

    op[-static_cast<std::ptrdiff_t>(lit) - 1] = static_cast<std::uint8_t>(lit - 1);
    if (lit == 0) --op;
    return static_cast<std::size_t>(op - out);
}

std::size_t lzfDecompress(const std::uint8_t* in, std::size_t in_len,
                          std::uint8_t* out, std::size_t out_len) {
    const std::uint8_t* ip     = in;
    const std::uint8_t* in_end = in + in_len;
    std::uint8_t*       op     = out;
    std::uint8_t* const out_end = out + out_len;

    while (ip < in_end) {
        std::size_t ctrl = *ip++;

        if (ctrl < (1u << 5)) {  // literal run of ctrl + 1 bytes
            ++ctrl;
            if (op + ctrl > out_end) return 0;
            if (ip + ctrl > in_end) return 0;
            do {
                *op++ = *ip++;
            } while (--ctrl);
        } else {  // back reference
            std::size_t len = ctrl >> 5;
            std::uint8_t* ref = op - ((ctrl & 0x1F) << 8) - 1;

            if (ip >= in_end) return 0;
            if (len == 7) {
                len += *ip++;
                if (ip >= in_end) return 0;
            }
            ref -= *ip++;

            if (op + len + 2 > out_end) return 0;
            if (ref < out) return 0;

            *op++ = *ref++;
            *op++ = *ref++;
            do {
                *op++ = *ref++;
            } while (--len);
        }
    }
    return static_cast<std::size_t>(op - out);
}

// --- length and string encoding ---------------------------------------------

void saveLen(std::string& out, std::uint64_t len) {
    if (len < (1u << 6)) {
        out.push_back(static_cast<char>((k6BitLen << 6) | len));
    } else if (len < (1u << 14)) {
        out.push_back(static_cast<char>((k14BitLen << 6) | ((len >> 8) & 0x3F)));
        out.push_back(static_cast<char>(len & 0xFF));
    } else if (len <= 0xFFFFFFFFULL) {
        out.push_back(static_cast<char>(k32BitLen));
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((len >> shift) & 0xFF));
        }
    } else {
        out.push_back(static_cast<char>(k64BitLen));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((len >> shift) & 0xFF));
        }
    }
}

namespace {

// Redis's rdbTryIntegerEncoding. Returns false when the text is not the
// canonical spelling of an integer that fits in 32 bits -- the round-trip check
// matters, because "007" and "12345" must not both come back as 12345.
bool saveIntegerEncoded(std::string& out, std::string_view s) {
    if (s.empty() || s.size() > 11) return false;
    std::int64_t value = 0;
    if (!core::stringToInt64(s, value)) return false;
    if (std::to_string(value) != s) return false;

    if (value >= -128 && value <= 127) {
        out.push_back(static_cast<char>((kEncVal << 6) | kEncInt8));
        out.push_back(static_cast<char>(value & 0xFF));
    } else if (value >= -32768 && value <= 32767) {
        out.push_back(static_cast<char>((kEncVal << 6) | kEncInt16));
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    } else if (value >= -2147483648LL && value <= 2147483647LL) {
        out.push_back(static_cast<char>((kEncVal << 6) | kEncInt32));
        for (int shift = 0; shift <= 24; shift += 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xFF));
        }
    } else {
        return false;  // an int64 that will not fit; goes down as text
    }
    return true;
}

// Compresses only when the result is at least four bytes shorter, which is the
// margin Redis demands before paying for a decompression on every load.
bool saveLzfEncoded(std::string& out, std::string_view s) {
    if (s.size() <= 4) return false;
    std::vector<std::uint8_t> compressed(s.size() - 4);
    const std::size_t n = lzfCompress(reinterpret_cast<const std::uint8_t*>(s.data()),
                                      s.size(), compressed.data(), compressed.size());
    if (n == 0) return false;

    out.push_back(static_cast<char>((kEncVal << 6) | kEncLzf));
    saveLen(out, n);
    saveLen(out, s.size());
    out.append(reinterpret_cast<const char*>(compressed.data()), n);
    return true;
}

}  // namespace

void saveString(std::string& out, std::string_view s) {
    if (s.size() <= 11 && saveIntegerEncoded(out, s)) return;
    // Under 20 bytes LZF cannot win even on a run of identical characters, so
    // Redis does not spend the attempt.
    if (s.size() > 20 && saveLzfEncoded(out, s)) return;
    saveLen(out, s.size());
    out.append(s);
}

// --- Reader -----------------------------------------------------------------

bool Reader::readByte(std::uint8_t& out) {
    if (pos_ >= bytes_.size()) return false;
    out = static_cast<std::uint8_t>(bytes_[pos_++]);
    return true;
}

bool Reader::peek(std::uint8_t& out) const {
    if (pos_ >= bytes_.size()) return false;
    out = static_cast<std::uint8_t>(bytes_[pos_]);
    return true;
}

bool Reader::readRaw(std::size_t n, std::string_view& out) {
    if (bytes_.size() - pos_ < n) return false;
    out = bytes_.substr(pos_, n);
    pos_ += n;
    return true;
}

bool Reader::readLen(std::uint64_t& out, bool* encoded) {
    if (encoded) *encoded = false;
    std::uint8_t first = 0;
    if (!readByte(first)) return false;

    const std::uint8_t kind = (first & 0xC0) >> 6;
    if (kind == k6BitLen) {
        out = first & 0x3F;
        return true;
    }
    if (kind == kEncVal) {
        if (!encoded) return false;  // an encoding tag where a length was required
        *encoded = true;
        out = first & 0x3F;
        return true;
    }
    if (kind == k14BitLen) {
        std::uint8_t second = 0;
        if (!readByte(second)) return false;
        out = (static_cast<std::uint64_t>(first & 0x3F) << 8) | second;
        return true;
    }
    // kind == 2: the byte itself says how wide the length is.
    std::string_view raw;
    if (first == k32BitLen) {
        if (!readRaw(4, raw)) return false;
        out = 0;
        for (char c : raw) out = (out << 8) | static_cast<std::uint8_t>(c);
        return true;
    }
    if (first == k64BitLen) {
        if (!readRaw(8, raw)) return false;
        out = 0;
        for (char c : raw) out = (out << 8) | static_cast<std::uint8_t>(c);
        return true;
    }
    return false;
}

bool Reader::readString(std::string& out) {
    bool encoded = false;
    std::uint64_t len = 0;
    if (!readLen(len, &encoded)) return false;

    if (!encoded) {
        std::string_view raw;
        if (!readRaw(static_cast<std::size_t>(len), raw)) return false;
        out.assign(raw);
        return true;
    }

    switch (static_cast<std::uint8_t>(len)) {
        case kEncInt8: {
            std::uint8_t b = 0;
            if (!readByte(b)) return false;
            out = std::to_string(static_cast<std::int8_t>(b));
            return true;
        }
        case kEncInt16: {
            std::string_view raw;
            if (!readRaw(2, raw)) return false;
            const std::uint16_t v = static_cast<std::uint8_t>(raw[0]) |
                                    (static_cast<std::uint16_t>(
                                         static_cast<std::uint8_t>(raw[1])) << 8);
            out = std::to_string(static_cast<std::int16_t>(v));
            return true;
        }
        case kEncInt32: {
            std::string_view raw;
            if (!readRaw(4, raw)) return false;
            std::uint32_t v = 0;
            for (int i = 3; i >= 0; --i) {
                v = (v << 8) | static_cast<std::uint8_t>(raw[static_cast<std::size_t>(i)]);
            }
            out = std::to_string(static_cast<std::int32_t>(v));
            return true;
        }
        case kEncLzf: {
            std::uint64_t clen = 0, ulen = 0;
            if (!readLen(clen) || !readLen(ulen)) return false;
            std::string_view raw;
            if (!readRaw(static_cast<std::size_t>(clen), raw)) return false;
            out.assign(static_cast<std::size_t>(ulen), '\0');
            if (ulen == 0) return false;
            const std::size_t n = lzfDecompress(
                reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size(),
                reinterpret_cast<std::uint8_t*>(out.data()), out.size());
            return n == ulen;
        }
        default:
            return false;
    }
}

// --- objects ----------------------------------------------------------------

namespace {

void saveBinaryDouble(std::string& out, double value) {
    // Little-endian on the wire, whatever the host is.
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 0; shift <= 56; shift += 8) {
        out.push_back(static_cast<char>((bits >> shift) & 0xFF));
    }
}

bool loadBinaryDouble(Reader& in, double& out) {
    std::string_view raw;
    if (!in.readRaw(8, raw)) return false;
    std::uint64_t bits = 0;
    for (int i = 7; i >= 0; --i) {
        bits = (bits << 8) |
               static_cast<std::uint8_t>(raw[static_cast<std::size_t>(i)]);
    }
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

// The pre-ZSET_2 score encoding: one length byte, three of whose values are
// reserved for the numbers decimal text cannot spell.
bool loadTextDouble(Reader& in, double& out) {
    std::uint8_t len = 0;
    if (!in.readByte(len)) return false;
    if (len == 255) { out = -HUGE_VAL; return true; }
    if (len == 254) { out = HUGE_VAL; return true; }
    if (len == 253) { out = std::nan(""); return true; }
    std::string_view raw;
    if (!in.readRaw(len, raw)) return false;
    const std::string text(raw);
    try {
        out = std::stod(text);
    } catch (...) {
        return false;
    }
    return true;
}

void appendListpackBytes(std::string& out, const core::Listpack& lp) {
    saveString(out, std::string_view(reinterpret_cast<const char*>(lp.bytes().data()),
                                     lp.bytes().size()));
}

bool readListpack(Reader& in, core::Listpack& out) {
    std::string blob;
    if (!in.readString(blob)) return false;
    auto lp = core::Listpack::fromBytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()));
    if (!lp) return false;
    out = std::move(*lp);
    return true;
}

}  // namespace

std::uint8_t objectType(const core::Value& value) {
    using core::ObjEncoding;
    switch (value.type()) {
        case core::ObjType::String:
            return kTypeString;
        case core::ObjType::List:
            // Both list encodings go out as a quicklist; a listpack-encoded list
            // is written as a quicklist that happens to have one node.
            return kTypeListQuicklist2;
        case core::ObjType::Set:
            switch (value.set()->encoding()) {
                case ObjEncoding::IntSet:   return kTypeSetIntset;
                case ObjEncoding::ListPack: return kTypeSetListpack;
                default:                    return kTypeSet;
            }
        case core::ObjType::Hash:
            return value.hash()->encoding() == ObjEncoding::ListPack ? kTypeHashListpack
                                                                     : kTypeHash;
        case core::ObjType::ZSet:
            return value.zset()->encoding() == ObjEncoding::ListPack ? kTypeZSetListpack
                                                                     : kTypeZSet2;
    }
    return kTypeString;
}

void saveObjectBody(std::string& out, const core::Value& value) {
    using core::ObjEncoding;
    switch (value.type()) {
        case core::ObjType::String:
            saveString(out, value.stringValue());
            return;

        case core::ObjType::List: {
            const core::ListValue* list = value.list();
            saveLen(out, list->nodes().size());
            for (const core::Listpack& node : list->nodes()) {
                // An element too big to share a node with anything is written as
                // itself, with no listpack around it. mnemos has no separate
                // plain node -- a node holding one oversized element *is* the
                // plain node, and this is where the two representations meet.
                core::Listpack::Element only;
                if (node.numElements() == 1 && node.get(0, only) && !only.is_integer &&
                    only.string.size() > kPlainNodeThreshold) {
                    saveLen(out, kNodePlain);
                    saveString(out, only.string);
                    continue;
                }
                saveLen(out, kNodePacked);
                appendListpackBytes(out, node);
            }
            return;
        }

        case core::ObjType::Set: {
            const core::SetValue* set = value.set();
            if (set->encoding() == ObjEncoding::IntSet) {
                const std::vector<std::uint8_t> blob = set->intset().toBytes();
                saveString(out, std::string_view(
                                    reinterpret_cast<const char*>(blob.data()), blob.size()));
                return;
            }
            if (set->encoding() == ObjEncoding::ListPack) {
                appendListpackBytes(out, set->listpack());
                return;
            }
            const std::vector<std::string> members = set->members();
            saveLen(out, members.size());
            for (const std::string& m : members) saveString(out, m);
            return;
        }

        case core::ObjType::Hash: {
            const core::HashValue* hash = value.hash();
            if (hash->encoding() == ObjEncoding::ListPack) {
                appendListpackBytes(out, hash->listpack());
                return;
            }
            const std::vector<std::string> flat = hash->flatten();
            saveLen(out, flat.size() / 2);
            for (const std::string& s : flat) saveString(out, s);
            return;
        }

        case core::ObjType::ZSet: {
            const core::ZSetValue* zset = value.zset();
            if (zset->encoding() == ObjEncoding::ListPack) {
                appendListpackBytes(out, zset->listpack());
                return;
            }
            // Descending, which is Redis's order: a loader that appends to a
            // skiplist then finds every insertion is at the head.
            const std::vector<std::pair<std::string, double>> all = zset->all();
            saveLen(out, all.size());
            for (auto it = all.rbegin(); it != all.rend(); ++it) {
                saveString(out, it->first);
                saveBinaryDouble(out, it->second);
            }
            return;
        }
    }
}

bool loadObjectBody(Reader& in, std::uint8_t type, core::Value& out) {
    switch (type) {
        case kTypeString: {
            std::string s;
            if (!in.readString(s)) return false;
            out = core::Value::makeString(s);
            return true;
        }

        case kTypeListQuicklist2: {
            std::uint64_t nodes = 0;
            if (!in.readLen(nodes) || nodes == 0) return false;
            std::vector<core::Listpack> packs;
            packs.reserve(static_cast<std::size_t>(nodes));
            for (std::uint64_t i = 0; i < nodes; ++i) {
                std::uint64_t container = 0;
                if (!in.readLen(container)) return false;
                if (container == kNodePlain) {
                    // One element too large for a listpack. mnemos has no plain
                    // node, so it becomes a node holding that single element --
                    // the same list, one representation less exotic.
                    std::string element;
                    if (!in.readString(element)) return false;
                    core::Listpack node;
                    node.append(element);
                    packs.push_back(std::move(node));
                    continue;
                }
                if (container != kNodePacked) return false;
                core::Listpack node;
                if (!readListpack(in, node)) return false;
                if (node.numElements() == 0) return false;
                packs.push_back(std::move(node));
            }
            out = core::Value::makeList();
            return out.list()->adoptNodes(std::move(packs));
        }

        case kTypeSetIntset: {
            std::string blob;
            if (!in.readString(blob)) return false;
            auto is = core::IntSet::fromBytes(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()));
            if (!is || is->empty()) return false;
            out = core::Value::makeSet();
            out.set()->adoptIntSet(std::move(*is));
            return true;
        }

        case kTypeSetListpack: {
            core::Listpack lp;
            if (!readListpack(in, lp) || lp.numElements() == 0) return false;
            out = core::Value::makeSet();
            out.set()->adoptListpack(std::move(lp));
            return true;
        }

        case kTypeSet: {
            std::uint64_t len = 0;
            if (!in.readLen(len) || len == 0) return false;
            out = core::Value::makeSet();
            for (std::uint64_t i = 0; i < len; ++i) {
                std::string member;
                if (!in.readString(member)) return false;
                out.set()->add(member);
            }
            return out.set()->size() > 0;
        }

        case kTypeHashListpack: {
            core::Listpack lp;
            if (!readListpack(in, lp) || lp.numElements() == 0) return false;
            out = core::Value::makeHash();
            return out.hash()->adoptListpack(std::move(lp));
        }

        case kTypeHash: {
            std::uint64_t len = 0;
            if (!in.readLen(len) || len == 0) return false;
            out = core::Value::makeHash();
            for (std::uint64_t i = 0; i < len; ++i) {
                std::string field, value;
                if (!in.readString(field) || !in.readString(value)) return false;
                out.hash()->set(field, value);
            }
            return out.hash()->size() > 0;
        }

        case kTypeZSetListpack: {
            core::Listpack lp;
            if (!readListpack(in, lp) || lp.numElements() == 0) return false;
            out = core::Value::makeZSet();
            return out.zset()->adoptListpack(std::move(lp));
        }

        case kTypeZSet:
        case kTypeZSet2: {
            std::uint64_t len = 0;
            if (!in.readLen(len) || len == 0) return false;
            out = core::Value::makeZSet();
            for (std::uint64_t i = 0; i < len; ++i) {
                std::string member;
                if (!in.readString(member)) return false;
                double score = 0;
                const bool ok = type == kTypeZSet2 ? loadBinaryDouble(in, score)
                                                   : loadTextDouble(in, score);
                if (!ok) return false;
                out.zset()->add(member, score);
            }
            return out.zset()->size() > 0;
        }

        default:
            return false;  // a pre-listpack encoding, or not a type at all
    }
}

// --- DUMP / RESTORE ---------------------------------------------------------

std::string dumpPayload(const core::Value& value) {
    std::string out;
    out.push_back(static_cast<char>(objectType(value)));
    saveObjectBody(out, value);

    out.push_back(static_cast<char>(kRdbVersion & 0xFF));
    out.push_back(static_cast<char>((kRdbVersion >> 8) & 0xFF));
    const std::uint64_t crc = crc64(0, out.data(), out.size());
    for (int shift = 0; shift <= 56; shift += 8) {
        out.push_back(static_cast<char>((crc >> shift) & 0xFF));
    }
    return out;
}

bool payloadFooterOk(std::string_view payload) {
    if (payload.size() < 11) return false;  // a type byte, a version, a checksum
    const std::size_t footer = payload.size() - 10;
    const auto byte = [&](std::size_t i) {
        return static_cast<std::uint64_t>(static_cast<std::uint8_t>(payload[i]));
    };
    const std::uint16_t version =
        static_cast<std::uint16_t>(byte(footer) | (byte(footer + 1) << 8));
    if (version > kRdbVersion) return false;

    std::uint64_t stored = 0;
    for (int i = 7; i >= 0; --i) {
        stored = (stored << 8) | byte(footer + 2 + static_cast<std::size_t>(i));
    }
    return stored == crc64(0, payload.data(), footer + 2);
}

bool loadPayload(std::string_view payload, core::Value& out) {
    if (payload.size() < 11) return false;
    Reader in(payload.substr(0, payload.size() - 10));
    std::uint8_t type = 0;
    if (!in.readByte(type)) return false;
    if (!loadObjectBody(in, type, out)) return false;
    return in.exhausted();
}

// --- files ------------------------------------------------------------------

FileWriter::FileWriter(int fd, std::string tmp_path, std::string path)
    : fd_(fd), tmp_path_(std::move(tmp_path)), path_(std::move(path)) {
    buffer_ = "REDIS";
    char version[8];
    std::snprintf(version, sizeof(version), "%04d", kRdbVersion);
    buffer_ += version;

    const auto aux = [this](std::string_view key, std::string_view value) {
        buffer_.push_back(static_cast<char>(kOpAux));
        saveString(buffer_, key);
        saveString(buffer_, value);
    };
    aux("redis-ver", "7.4.0");
    aux("redis-bits", "64");
}

FileWriter::FileWriter(FileWriter&& other) noexcept
    : fd_(other.fd_),
      tmp_path_(std::move(other.tmp_path_)),
      path_(std::move(other.path_)),
      buffer_(std::move(other.buffer_)),
      crc_(other.crc_),
      failed_(other.failed_) {
    other.fd_ = -1;
}

FileWriter::~FileWriter() {
    // Reached only when finish() was never called, i.e. the save was abandoned.
    if (fd_ >= 0) {
        ::close(fd_);
        ::unlink(tmp_path_.c_str());
    }
}

std::optional<FileWriter> FileWriter::create(const std::string& path) {
    const std::string tmp = path + ".tmp-" + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return std::nullopt;
    return FileWriter(fd, tmp, path);
}

void FileWriter::append(std::string_view bytes) {
    buffer_.append(bytes);
    if (buffer_.size() >= 64 * 1024) flush();
}

bool FileWriter::flush() {
    if (failed_) return false;
    if (buffer_.empty()) return true;
    crc_ = crc64(crc_, buffer_.data(), buffer_.size());

    const char* p = buffer_.data();
    std::size_t left = buffer_.size();
    while (left > 0) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            failed_ = true;
            buffer_.clear();
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    buffer_.clear();
    return true;
}

void FileWriter::selectDb(int index, std::uint64_t size, std::uint64_t expires) {
    std::string frame;
    frame.push_back(static_cast<char>(kOpSelectDb));
    saveLen(frame, static_cast<std::uint64_t>(index));
    frame.push_back(static_cast<char>(kOpResizeDb));
    saveLen(frame, size);
    saveLen(frame, expires);
    append(frame);
}

void FileWriter::writeEntry(const std::string& key, const core::Value& value,
                            std::int64_t expire_at_ms) {
    std::string frame;
    if (expire_at_ms >= 0) {
        frame.push_back(static_cast<char>(kOpExpireMs));
        const auto ms = static_cast<std::uint64_t>(expire_at_ms);
        for (int shift = 0; shift <= 56; shift += 8) {
            frame.push_back(static_cast<char>((ms >> shift) & 0xFF));
        }
    }
    frame.push_back(static_cast<char>(objectType(value)));
    saveString(frame, key);
    saveObjectBody(frame, value);
    append(frame);
}

bool FileWriter::finish() {
    buffer_.push_back(static_cast<char>(kOpEof));
    // The checksum covers the EOF byte, so it has to be folded in before the
    // eight bytes that carry it are appended.
    crc_ = crc64(crc_, buffer_.data(), buffer_.size());
    std::string trailer;
    for (int shift = 0; shift <= 56; shift += 8) {
        trailer.push_back(static_cast<char>((crc_ >> shift) & 0xFF));
    }
    const std::uint64_t saved_crc = crc_;
    buffer_ += trailer;
    // flush() would fold the trailer into the checksum again; write it directly.
    crc_ = saved_crc;
    const std::string payload = std::move(buffer_);
    buffer_.clear();

    const char* p = payload.data();
    std::size_t left = payload.size();
    while (left > 0 && !failed_) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            failed_ = true;
            break;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }

    if (!failed_) ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;

    if (failed_ || ::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
        ::unlink(tmp_path_.c_str());
        return false;
    }
    return true;
}

bool loadFile(const std::string& path,
              const std::function<void(int, std::string, core::Value, std::int64_t)>& emit,
              bool& existed, std::string& error) {
    existed = false;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        if (errno == ENOENT) return true;
        error = std::strerror(errno);
        return false;
    }
    existed = true;

    std::string contents;
    char chunk[65536];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), fp)) > 0) contents.append(chunk, n);
    const bool read_failed = std::ferror(fp) != 0;
    std::fclose(fp);
    if (read_failed) {
        error = "read error";
        return false;
    }

    if (contents.size() < 9 || contents.compare(0, 5, "REDIS") != 0) {
        error = "wrong signature";
        return false;
    }
    const int version = std::atoi(contents.substr(5, 4).c_str());
    if (version < 1 || version > kRdbVersion) {
        error = "unsupported RDB version " + contents.substr(5, 4);
        return false;
    }

    Reader in(std::string_view(contents).substr(9));
    int          db_index     = 0;
    std::int64_t expire_at_ms = -1;

    while (true) {
        std::uint8_t opcode = 0;
        if (!in.readByte(opcode)) {
            error = "truncated file";
            return false;
        }

        if (opcode == kOpEof) {
            // Everything up to and including the EOF byte is covered by the
            // checksum. A stored zero means the writer disabled checksumming.
            const std::size_t covered = 9 + in.pos();
            std::string_view trailer;
            if (!in.readRaw(8, trailer)) {
                error = "missing checksum";
                return false;
            }
            std::uint64_t stored = 0;
            for (int i = 7; i >= 0; --i) {
                stored = (stored << 8) |
                         static_cast<std::uint8_t>(trailer[static_cast<std::size_t>(i)]);
            }
            if (stored != 0 && stored != crc64(0, contents.data(), covered)) {
                error = "checksum mismatch";
                return false;
            }
            return true;
        }

        if (opcode == kOpSelectDb) {
            std::uint64_t index = 0;
            if (!in.readLen(index)) { error = "bad SELECTDB"; return false; }
            db_index = static_cast<int>(index);
            continue;
        }
        if (opcode == kOpResizeDb) {
            std::uint64_t size = 0, expires = 0;
            if (!in.readLen(size) || !in.readLen(expires)) {
                error = "bad RESIZEDB";
                return false;
            }
            continue;
        }
        if (opcode == kOpAux) {
            std::string key, value;
            if (!in.readString(key) || !in.readString(value)) {
                error = "bad auxiliary field";
                return false;
            }
            continue;
        }
        if (opcode == kOpExpireMs) {
            std::string_view raw;
            if (!in.readRaw(8, raw)) { error = "bad expiry"; return false; }
            std::uint64_t ms = 0;
            for (int i = 7; i >= 0; --i) {
                ms = (ms << 8) |
                     static_cast<std::uint8_t>(raw[static_cast<std::size_t>(i)]);
            }
            expire_at_ms = static_cast<std::int64_t>(ms);
            continue;
        }
        if (opcode == kOpExpireSec) {
            std::string_view raw;
            if (!in.readRaw(4, raw)) { error = "bad expiry"; return false; }
            std::uint64_t seconds = 0;
            for (int i = 3; i >= 0; --i) {
                seconds = (seconds << 8) |
                          static_cast<std::uint8_t>(raw[static_cast<std::size_t>(i)]);
            }
            expire_at_ms = static_cast<std::int64_t>(seconds) * 1000;
            continue;
        }
        if (opcode == kOpIdle) {
            std::uint64_t idle = 0;
            if (!in.readLen(idle)) { error = "bad IDLE"; return false; }
            continue;
        }
        if (opcode == kOpFreq) {
            std::uint8_t freq = 0;
            if (!in.readByte(freq)) { error = "bad FREQ"; return false; }
            continue;
        }
        if (opcode >= 0xF0) {
            error = "unsupported opcode " + std::to_string(opcode);
            return false;
        }

        std::string key;
        if (!in.readString(key)) { error = "bad key"; return false; }
        core::Value value;
        if (!loadObjectBody(in, opcode, value)) {
            error = "bad value for key '" + key + "'";
            return false;
        }
        emit(db_index, std::move(key), std::move(value), expire_at_ms);
        expire_at_ms = -1;
    }
}

}  // namespace mnemos::persist
