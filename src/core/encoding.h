// Object types, physical encodings, and the strict integer parse that decides
// between them.
//
// Split out from object.h so that the collection types can name an encoding
// without depending on Value, while Value is free to contain them.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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
// allocation -- one cache line, one malloc, no pointer chase.
inline constexpr std::size_t kEmbStrSizeLimit = 44;

// Parses a string as a strict int64 exactly as Redis's string2ll does: no
// leading '+', no leading zeros (except "0" itself), no whitespace, no
// overflow. This strictness is why SET k 007 stays a raw string but SET k 7
// becomes an int, and why a listpack stores "007" as text but 7 as an integer.
bool stringToInt64(std::string_view s, std::int64_t& out);

}  // namespace mnemos::core
