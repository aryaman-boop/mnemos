#include "core/encoding.h"

#include <limits>

namespace mnemos::core {

std::string_view typeName(ObjType t) {
    switch (t) {
        case ObjType::String: return "string";
        case ObjType::List:   return "list";
        case ObjType::Set:    return "set";
        case ObjType::Hash:   return "hash";
        case ObjType::ZSet:   return "zset";
    }
    return "unknown";
}

std::string_view encodingName(ObjEncoding e) {
    switch (e) {
        case ObjEncoding::Int:       return "int";
        case ObjEncoding::EmbStr:    return "embstr";
        case ObjEncoding::Raw:       return "raw";
        case ObjEncoding::ListPack:  return "listpack";
        case ObjEncoding::QuickList: return "quicklist";
        case ObjEncoding::IntSet:    return "intset";
        case ObjEncoding::HashTable: return "hashtable";
        case ObjEncoding::SkipList:  return "skiplist";
    }
    return "unknown";
}

bool stringToInt64(std::string_view s, std::int64_t& out) {
    if (s.empty() || s.size() >= 21) return false;

    std::size_t i = 0;
    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        i = 1;
        if (s.size() == 1) return false;
    }

    // "0" is the only representation that may start with a zero digit; this is
    // what keeps "007" and "-0" out of the int encoding.
    if (s[i] == '0') {
        if (s.size() - i != 1) return false;
        if (negative) return false;
        out = 0;
        return true;
    }

    std::uint64_t acc = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (acc > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        acc = acc * 10 + digit;
    }

    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (acc > kMax + 1) return false;
        out = (acc == kMax + 1) ? std::numeric_limits<std::int64_t>::min()
                                : -static_cast<std::int64_t>(acc);
    } else {
        if (acc > kMax) return false;
        out = static_cast<std::int64_t>(acc);
    }
    return true;
}

}  // namespace mnemos::core
