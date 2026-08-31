// The value object -- mnemos's equivalent of Redis's robj.
//
// The interesting part of Redis is not that it stores strings and lists; it is
// that each logical type has *several* physical representations and switches
// between them based on size and content. A short numeric string costs 8 bytes
// as an integer; the same value as a heap string costs far more. Getting these
// transitions right (and irreversible in the right direction) is most of what
// "knowing Redis internals" means.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace mnemos::core {

enum class ObjType : std::uint8_t { String, List, Set, Hash, ZSet };

enum class ObjEncoding : std::uint8_t {
    Int,        // string that is a valid int64, stored as an int64
    EmbStr,     // short immutable string (<= 44 bytes)
    Raw,        // modifiable heap string
    ListPack,   // compact single-allocation sequence
    QuickList,  // linked list of listpack nodes
    IntSet,     // sorted array of integers
    HashTable,  // open-hashed dict
    SkipList,   // skiplist + dict pair
};

std::string_view typeName(ObjType t);
std::string_view encodingName(ObjEncoding e);

// Redis's OBJ_ENCODING_EMBSTR_SIZE_LIMIT. A 44-byte payload is the largest that
// still lets the object header and the string data share a single 64-byte
// allocation -- i.e. one cache line, one malloc, no pointer chase.
inline constexpr std::size_t kEmbStrSizeLimit = 44;

class Value {
public:
    Value() = default;

    // Chooses the encoding the way Redis's tryObjectEncoding does: parse as an
    // int64 first, then fall back to embstr / raw on length.
    static Value makeString(std::string_view s);
    // Forces a Raw string -- used by APPEND/SETRANGE, whose results are mutable
    // and so must never be left in the immutable embstr representation.
    static Value makeRawString(std::string_view s);
    static Value makeInt(std::int64_t n);

    ObjType     type() const { return type_; }
    ObjEncoding encoding() const { return encoding_; }

    bool isString() const { return type_ == ObjType::String; }

    // Materialises the string form regardless of the underlying encoding.
    std::string stringValue() const;

    // Borrows the stored bytes without copying. Only valid when the encoding is
    // EmbStr or Raw -- an Int-encoded value has no stored bytes to point at.
    std::string_view stringRef() const;

    // Reads the value as an integer without materialising a string when the
    // encoding is already Int. Returns false if the content is not an integer.
    bool asInt(std::int64_t& out) const;

    // Promotes an Int/EmbStr value to Raw so it can be mutated in place.
    void makeMutable();
    // Appends to a string value, promoting to Raw first if needed.
    void appendString(std::string_view suffix);
    // Direct access for in-place mutation. Caller must have called makeMutable().
    std::string& mutableString();

    std::size_t stringLength() const;

    // Approximate heap cost in bytes, for OBJECT/MEMORY reporting.
    std::size_t memoryUsage() const;

    // Access clock, used by the LRU/idle-time reporting.
    std::int64_t lastAccess() const { return last_access_ms_; }
    void touch(std::int64_t now_ms) { last_access_ms_ = now_ms; }

private:
    ObjType     type_     = ObjType::String;
    ObjEncoding encoding_ = ObjEncoding::EmbStr;
    // Int-encoded values live in the int64 arm; every other string encoding uses
    // the std::string arm. Collection types are added alongside these.
    std::variant<std::int64_t, std::string> data_{std::string{}};
    std::int64_t last_access_ms_ = 0;
};

// Parses a string as a strict int64 exactly as Redis's string2ll does: no
// leading '+', no leading zeros (except "0" itself), no whitespace, no overflow.
// This strictness is why SET k 007 stays a raw string but SET k 7 becomes an int.
bool stringToInt64(std::string_view s, std::int64_t& out);

}  // namespace mnemos::core
