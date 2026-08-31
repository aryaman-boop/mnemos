#include "core/intset.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace mnemos::core {
namespace {

std::uint32_t readLE32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void appendLE(std::vector<std::uint8_t>& out, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

std::int64_t readSigned(const std::uint8_t* p, std::size_t width) {
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < width; ++i) {
        raw |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    switch (width) {
        case 2: return static_cast<std::int16_t>(raw);
        case 4: return static_cast<std::int32_t>(raw);
        default: return static_cast<std::int64_t>(raw);
    }
}

}  // namespace

IntSet::Width IntSet::widthFor(std::int64_t value) {
    if (value >= std::numeric_limits<std::int16_t>::min() &&
        value <= std::numeric_limits<std::int16_t>::max()) {
        return Width::Int16;
    }
    if (value >= std::numeric_limits<std::int32_t>::min() &&
        value <= std::numeric_limits<std::int32_t>::max()) {
        return Width::Int32;
    }
    return Width::Int64;
}

void IntSet::upgradeTo(Width width) {
    // One-way: an intset never narrows, because doing so on every removal would
    // mean rewriting the array to save bytes that will likely be needed again.
    if (static_cast<std::uint32_t>(width) > static_cast<std::uint32_t>(width_)) {
        width_ = width;
    }
}

bool IntSet::contains(std::int64_t value) const {
    return std::binary_search(values_.begin(), values_.end(), value);
}

bool IntSet::add(std::int64_t value) {
    const auto it = std::lower_bound(values_.begin(), values_.end(), value);
    if (it != values_.end() && *it == value) return false;
    upgradeTo(widthFor(value));
    values_.insert(it, value);
    return true;
}

bool IntSet::remove(std::int64_t value) {
    const auto it = std::lower_bound(values_.begin(), values_.end(), value);
    if (it == values_.end() || *it != value) return false;
    values_.erase(it);
    return true;
}

void IntSet::clear() {
    values_.clear();
    width_ = Width::Int16;
}

std::vector<std::uint8_t> IntSet::toBytes() const {
    std::vector<std::uint8_t> out;
    out.reserve(byteSize());
    appendLE(out, static_cast<std::uint32_t>(width_), 4);
    appendLE(out, static_cast<std::uint32_t>(values_.size()), 4);
    for (std::int64_t v : values_) {
        appendLE(out, static_cast<std::uint64_t>(v), static_cast<std::size_t>(width_));
    }
    return out;
}

std::optional<IntSet> IntSet::fromBytes(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8) return std::nullopt;

    const std::uint32_t encoding = readLE32(bytes.data());
    if (encoding != 2 && encoding != 4 && encoding != 8) return std::nullopt;
    const std::uint32_t count = readLE32(bytes.data() + 4);

    const std::size_t width = encoding;
    if (bytes.size() != 8 + static_cast<std::size_t>(count) * width) return std::nullopt;

    IntSet set;
    set.width_ = static_cast<Width>(encoding);
    set.values_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        set.values_.push_back(readSigned(bytes.data() + 8 + i * width, width));
    }

    // The invariant the rest of the class relies on: strictly ascending, no
    // duplicates. Worth verifying because these bytes come off disk.
    for (std::size_t i = 1; i < set.values_.size(); ++i) {
        if (set.values_[i] <= set.values_[i - 1]) return std::nullopt;
    }
    return set;
}

}  // namespace mnemos::core
