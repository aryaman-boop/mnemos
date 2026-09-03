// The four collection value types, each owning its own encoding transitions.
//
// This is where the encoding thresholds actually live. A hash starts life as a
// listpack and becomes a real hash table once it grows past
// hash-max-listpack-entries (128) or any field/value exceeds
// hash-max-listpack-value (64 bytes). A set has three encodings rather than
// two: all-integer sets start as an intset, mixed sets as a listpack, and
// either converts to a hash table when it gets large.
//
// Every conversion here is one-way. Deleting members from a converted
// collection does not convert it back, because Redis does not pay to
// re-compact a structure that just demonstrated it can grow.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dict.h"
#include "core/intset.h"
#include "core/listpack.h"
#include "core/encoding.h"
#include "core/skiplist.h"

namespace mnemos::core {

// Conversion thresholds. These are the values Redis 8 actually runs with,
// confirmed by querying a live server rather than taken from documentation --
// hash-max-listpack-entries in particular is widely cited as 128 but is 512.
//
// Note that lists are the odd one out: list-max-listpack-size defaults to -2,
// which does not mean an element count at all. Negative values select a *size*
// limit, -2 being 8KB, so a list converts based on the bytes its listpack
// occupies. A single 9KB element is enough to force a quicklist on its own.
struct EncodingLimits {
    std::size_t hash_max_listpack_entries = 512;
    std::size_t hash_max_listpack_value   = 64;
    std::size_t list_max_listpack_bytes   = 8192;  // list-max-listpack-size -2
    std::size_t set_max_intset_entries    = 512;
    std::size_t set_max_listpack_entries  = 128;
    std::size_t set_max_listpack_value    = 64;
    std::size_t zset_max_listpack_entries = 128;
    std::size_t zset_max_listpack_value   = 64;
};

const EncodingLimits& defaultLimits();

// ---------------------------------------------------------------------------
// Hash
// ---------------------------------------------------------------------------

class HashValue {
public:
    HashValue() = default;
    HashValue(HashValue&&) noexcept = default;
    HashValue& operator=(HashValue&&) noexcept = default;
    // Deep copy, rebuilt from the logical contents -- RENAME and COPY need it,
    // and the underlying Dict cannot be copied directly.
    HashValue(const HashValue& other);
    HashValue& operator=(const HashValue& other);

    ObjEncoding encoding() const { return encoding_; }
    std::size_t size() const;

    std::optional<std::string> get(std::string_view field) const;
    // Returns true when the field was newly added rather than updated.
    bool set(std::string_view field, std::string_view value);
    bool erase(std::string_view field);
    bool contains(std::string_view field) const;

    // field, value, field, value... in storage order.
    std::vector<std::string> flatten() const;
    std::vector<std::string> fields() const;
    std::vector<std::string> values() const;

    // Exposed for the RDB writer, which needs the raw listpack when the hash is
    // small enough to be stored in its compact form.
    const Listpack& listpack() const { return listpack_; }
    // The inverse, for the RDB reader: take a listpack off disk as-is, then
    // apply the same limits a load applies, so a hash saved under one set of
    // thresholds is re-encoded under ours. False when the element count is odd,
    // i.e. the blob is not field/value pairs at all.
    bool adoptListpack(Listpack lp);

    void convertToHashTable();

private:
    bool shouldConvert(std::string_view field, std::string_view value) const;

    ObjEncoding       encoding_ = ObjEncoding::ListPack;
    Listpack          listpack_;   // [field, value, field, value, ...]
    Dict<std::string> table_;
};

// ---------------------------------------------------------------------------
// Set
// ---------------------------------------------------------------------------

class SetValue {
public:
    SetValue() = default;
    SetValue(SetValue&&) noexcept = default;
    SetValue& operator=(SetValue&&) noexcept = default;
    SetValue(const SetValue& other);
    SetValue& operator=(const SetValue& other);

    ObjEncoding encoding() const { return encoding_; }
    std::size_t size() const;

    bool contains(std::string_view member) const;
    bool add(std::string_view member);
    bool erase(std::string_view member);

    std::vector<std::string> members() const;
    // Uniform random member, for SRANDMEMBER and SPOP.
    std::optional<std::string> randomMember() const;

    const IntSet&   intset() const { return intset_; }
    const Listpack& listpack() const { return listpack_; }
    // RDB load: adopt a serialised encoding verbatim, then re-apply the limits.
    // Taking the intset bytes rather than re-adding the members is what keeps
    // its width sticky -- an 8-byte-wide set whose large member was deleted
    // before the save comes back 8 bytes wide, exactly as Redis does.
    void adoptIntSet(IntSet is);
    void adoptListpack(Listpack lp);

    void convertToListpack();
    void convertToHashTable();

private:
    ObjEncoding encoding_ = ObjEncoding::IntSet;
    IntSet      intset_;
    Listpack    listpack_;
    Dict<bool>  table_;
};

// ---------------------------------------------------------------------------
// Sorted set
// ---------------------------------------------------------------------------

class ZSetValue {
public:
    ZSetValue() = default;
    ZSetValue(ZSetValue&&) noexcept = default;
    ZSetValue& operator=(ZSetValue&&) noexcept = default;
    ZSetValue(const ZSetValue& other);
    ZSetValue& operator=(const ZSetValue& other);

    ObjEncoding encoding() const { return encoding_; }
    std::size_t size() const;

    std::optional<double> score(std::string_view member) const;
    // Returns true when the member was newly added rather than rescored.
    bool add(std::string_view member, double score);
    bool erase(std::string_view member);

    // 0-based rank in ascending score order.
    std::optional<std::size_t> rank(std::string_view member) const;
    // Ascending (member, score) pairs.
    std::vector<std::pair<std::string, double>> range(std::size_t start,
                                                      std::size_t stop) const;
    std::vector<std::pair<std::string, double>> all() const;
    std::vector<std::pair<std::string, double>> rangeByScore(const ScoreRange& range) const;
    std::vector<std::pair<std::string, double>> rangeByLex(const LexRange& range) const;

    const Listpack& listpack() const { return listpack_; }
    // RDB load. The blob is already ordered by (score, member); adopting it
    // keeps that order rather than paying to re-sort what Redis already sorted.
    bool adoptListpack(Listpack lp);

    void convertToSkipList();

private:
    ObjEncoding    encoding_ = ObjEncoding::ListPack;
    Listpack       listpack_;   // [member, score, member, score, ...]
    SkipList       skiplist_;
    Dict<double>   scores_;     // member -> score, for O(1) ZSCORE
};

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

// A quicklist is a doubly-linked list of listpack nodes: it keeps the memory
// win of listpacks for the bulk of the data while capping the cost of an
// insertion, which on a single huge listpack would be an O(N) memmove.
class ListValue {
public:
    ObjEncoding encoding() const { return encoding_; }
    std::size_t size() const;

    void pushFront(std::string_view value);
    void pushBack(std::string_view value);
    std::optional<std::string> popFront();
    std::optional<std::string> popBack();

    // Supports negative indices, where -1 is the last element.
    std::optional<std::string> at(std::int64_t index) const;
    bool set(std::int64_t index, std::string_view value);
    std::vector<std::string> range(std::int64_t start, std::int64_t stop) const;
    // Removes up to `count` occurrences; negative count searches from the tail,
    // zero removes every match.
    std::size_t removeValue(std::string_view value, std::int64_t count);
    void trim(std::int64_t start, std::int64_t stop);
    std::optional<std::size_t> indexOf(std::string_view value) const;

    const std::vector<Listpack>& nodes() const { return nodes_; }
    // RDB load: the quicklist's nodes, as they were written. False when there
    // is nothing in them -- an empty list is not a value Redis ever stores.
    bool adoptNodes(std::vector<Listpack> nodes);

private:
    void maybeConvert();
    // Normalises a possibly-negative index; returns npos when out of range.
    std::size_t resolveIndex(std::int64_t index) const;
    std::vector<std::string> toVector() const;
    void rebuildFrom(const std::vector<std::string>& items);

    ObjEncoding           encoding_ = ObjEncoding::ListPack;
    std::vector<Listpack> nodes_{Listpack{}};
};

}  // namespace mnemos::core
