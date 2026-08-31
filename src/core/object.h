// The value object -- mnemos's equivalent of Redis's robj.
//
// The interesting part of Redis is not that it stores strings and lists; it is
// that each logical type has *several* physical representations and switches
// between them based on size and content. Getting those transitions right (and
// one-way, in the right direction) is most of what "knowing Redis internals"
// means, so Value delegates encoding decisions to the type-specific classes in
// collections.h rather than deciding centrally.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "core/collections.h"
#include "core/encoding.h"

namespace mnemos::core {

class Value {
public:
    Value() = default;
    ~Value() = default;

    // Collections are held behind unique_ptr so Value stays small (a keyspace
    // of millions of short strings should not pay for the largest arm), which
    // means copying has to be written out rather than defaulted.
    Value(const Value& other);
    Value& operator=(const Value& other);
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;

    // --- construction -------------------------------------------------------

    // Chooses the encoding the way Redis's tryObjectEncoding does: parse as an
    // int64 first, then fall back to embstr / raw on length.
    static Value makeString(std::string_view s);
    // Forces a Raw string -- used by APPEND/SETRANGE, whose results are mutable
    // and so must never be left in the immutable embstr representation.
    static Value makeRawString(std::string_view s);
    static Value makeInt(std::int64_t n);

    static Value makeList();
    static Value makeHash();
    static Value makeSet();
    static Value makeZSet();

    // --- identity -----------------------------------------------------------

    ObjType     type() const { return type_; }
    ObjEncoding encoding() const;

    bool isString() const { return type_ == ObjType::String; }
    bool isList() const { return type_ == ObjType::List; }
    bool isHash() const { return type_ == ObjType::Hash; }
    bool isSet() const { return type_ == ObjType::Set; }
    bool isZSet() const { return type_ == ObjType::ZSet; }

    // Number of elements for a collection; 1 for a string.
    std::size_t elementCount() const;

    // --- string access ------------------------------------------------------

    std::string stringValue() const;
    // Borrows the stored bytes without copying. Only valid for EmbStr/Raw -- an
    // Int-encoded value has no stored bytes to point at.
    std::string_view stringRef() const;
    // Reads as an integer without materialising a string when already Int.
    bool asInt(std::int64_t& out) const;

    void makeMutable();
    void appendString(std::string_view suffix);
    std::string& mutableString();
    std::size_t stringLength() const;

    // --- collection access --------------------------------------------------
    // Return nullptr when the value is not of that type, so callers can use the
    // result directly as the WRONGTYPE check.

    ListValue*  list();
    HashValue*  hash();
    SetValue*   set();
    ZSetValue*  zset();
    const ListValue* list() const;
    const HashValue* hash() const;
    const SetValue*  set() const;
    const ZSetValue* zset() const;

    // --- bookkeeping --------------------------------------------------------

    std::size_t memoryUsage() const;
    std::int64_t lastAccess() const { return last_access_ms_; }
    void touch(std::int64_t now_ms) { last_access_ms_ = now_ms; }

private:
    ObjType     type_     = ObjType::String;
    ObjEncoding encoding_ = ObjEncoding::EmbStr;  // strings only; collections self-report

    std::variant<std::int64_t,
                 std::string,
                 std::unique_ptr<ListValue>,
                 std::unique_ptr<HashValue>,
                 std::unique_ptr<SetValue>,
                 std::unique_ptr<ZSetValue>>
        data_{std::string{}};

    std::int64_t last_access_ms_ = 0;
};

}  // namespace mnemos::core
