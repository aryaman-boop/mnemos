#include "core/object.h"

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

Value Value::makeString(std::string_view s) {
    Value v;
    v.type_ = ObjType::String;

    std::int64_t n = 0;
    if (stringToInt64(s, n)) {
        v.encoding_ = ObjEncoding::Int;
        v.data_     = n;
        return v;
    }

    v.encoding_ = (s.size() <= kEmbStrSizeLimit) ? ObjEncoding::EmbStr : ObjEncoding::Raw;
    v.data_     = std::string(s);
    return v;
}

Value Value::makeRawString(std::string_view s) {
    Value v;
    v.type_     = ObjType::String;
    v.encoding_ = ObjEncoding::Raw;
    v.data_     = std::string(s);
    return v;
}

Value Value::makeInt(std::int64_t n) {
    Value v;
    v.type_     = ObjType::String;
    v.encoding_ = ObjEncoding::Int;
    v.data_     = n;
    return v;
}

std::string Value::stringValue() const {
    if (encoding_ == ObjEncoding::Int) {
        return std::to_string(std::get<std::int64_t>(data_));
    }
    return std::get<std::string>(data_);
}

std::string_view Value::stringRef() const {
    if (encoding_ == ObjEncoding::Int) return {};
    return std::get<std::string>(data_);
}

bool Value::asInt(std::int64_t& out) const {
    if (encoding_ == ObjEncoding::Int) {
        out = std::get<std::int64_t>(data_);
        return true;
    }
    return stringToInt64(std::get<std::string>(data_), out);
}

void Value::makeMutable() {
    if (encoding_ == ObjEncoding::Raw) return;
    std::string materialised = stringValue();
    data_     = std::move(materialised);
    encoding_ = ObjEncoding::Raw;
}

void Value::appendString(std::string_view suffix) {
    makeMutable();
    std::get<std::string>(data_).append(suffix);
}

std::string& Value::mutableString() {
    makeMutable();
    return std::get<std::string>(data_);
}

std::size_t Value::stringLength() const {
    if (encoding_ == ObjEncoding::Int) {
        // Cheaper than materialising: count the decimal digits directly.
        std::int64_t n = std::get<std::int64_t>(data_);
        std::size_t len = (n < 0) ? 1 : 0;
        std::uint64_t magnitude = (n < 0) ? -static_cast<std::uint64_t>(n)
                                          :  static_cast<std::uint64_t>(n);
        do { ++len; magnitude /= 10; } while (magnitude > 0);
        return len;
    }
    return std::get<std::string>(data_).size();
}

std::size_t Value::memoryUsage() const {
    constexpr std::size_t kObjectHeader = 16;
    if (encoding_ == ObjEncoding::Int) return kObjectHeader;
    const std::string& s = std::get<std::string>(data_);
    // embstr shares one allocation with the header; raw pays for a second.
    return kObjectHeader + s.capacity() + (encoding_ == ObjEncoding::Raw ? 16 : 0);
}

}  // namespace mnemos::core
