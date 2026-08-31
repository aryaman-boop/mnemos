// intset: the encoding Redis uses for a set whose members are all integers.
//
//   <encoding:u32 LE> <length:u32 LE> <contents[]>
//
// `contents` is a sorted array of fixed-width little-endian integers, where the
// width is 2, 4 or 8 bytes as given by `encoding`. Two consequences follow, and
// both are visible from the outside:
//
//   * Membership is a binary search -- O(log N) with no hashing and no pointer
//     chasing, on an array that is usually one cache line or two.
//   * The width *upgrades* when a value no longer fits, and never downgrades.
//     Adding one 64-bit member to a set of small integers rewrites the whole
//     array to 8-byte slots, and removing it again does not shrink it back.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mnemos::core {

class IntSet {
public:
    enum class Width : std::uint32_t { Int16 = 2, Int32 = 4, Int64 = 8 };

    IntSet() = default;

    static std::optional<IntSet> fromBytes(std::span<const std::uint8_t> bytes);

    // Returns false when the value was already present.
    bool add(std::int64_t value);
    bool remove(std::int64_t value);
    bool contains(std::int64_t value) const;

    std::size_t size() const { return values_.size(); }
    bool empty() const { return values_.empty(); }

    // Members in ascending order -- intsets are always sorted, which is why
    // SMEMBERS on an intset-encoded set comes back sorted while on a listpack-
    // or hashtable-encoded set it does not.
    const std::vector<std::int64_t>& values() const { return values_; }
    std::int64_t at(std::size_t index) const { return values_[index]; }

    Width width() const { return width_; }

    // Serialised form, for RDB.
    std::vector<std::uint8_t> toBytes() const;
    std::size_t byteSize() const { return 8 + values_.size() * static_cast<std::size_t>(width_); }

    void clear();

private:
    // Smallest width that can hold `value`.
    static Width widthFor(std::int64_t value);
    void upgradeTo(Width width);

    std::vector<std::int64_t> values_;  // kept sorted ascending
    Width                     width_ = Width::Int16;
};

}  // namespace mnemos::core
