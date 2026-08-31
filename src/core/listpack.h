// listpack: Redis's compact, single-allocation sequence encoding.
//
// This is the representation behind *every* small collection in modern Redis --
// small hashes, small sorted sets, small sets, and the nodes of a quicklist.
// The reason it exists is memory and cache behaviour: a 10-field hash stored as
// a real hash table costs several hundred bytes of buckets, entry structs and
// pointers, and every lookup chases pointers into cold memory. The same hash as
// a listpack is one flat allocation of a few dozen bytes, and scanning it
// linearly beats hashing because it never leaves L1.
//
// Layout:
//
//   <total-bytes:u32 LE> <num-elements:u16 LE> <element>... <0xFF>
//
// Each element is <encoding+data><backlen>, where `backlen` stores the length of
// the encoding+data portion in a variable-length form designed to be decoded
// *backwards*. That is what makes the structure traversable right-to-left --
// which is how RPOP and negative indices work without a second pointer.
//
// Integers are stored as integers, not as their decimal text: the member "12345"
// occupies 3 bytes here rather than 5, and comes back out as an int64.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mnemos::core {

// Header is u32 total-bytes + u16 element-count.
inline constexpr std::size_t kListpackHeaderSize = 6;
inline constexpr std::uint8_t kListpackEof = 0xFF;
// The count field saturates: beyond this the header stops being authoritative
// and the elements have to be walked to be counted.
inline constexpr std::uint16_t kListpackMaxInlineCount = 65535;

class Listpack {
public:
    struct Element {
        bool             is_integer = false;
        std::int64_t     integer    = 0;
        std::string_view string;  // valid only while the Listpack is unmodified

        // Materialises the element as text regardless of how it was stored.
        std::string toString() const;
    };

    Listpack();

    // Adopts an existing serialised listpack, validating its structure. Returns
    // nullopt if the bytes are malformed -- important because these come off
    // disk (RDB) and off the network (replication), i.e. from outside.
    static std::optional<Listpack> fromBytes(std::span<const std::uint8_t> bytes);

    // Appends, choosing the integer encoding automatically when `value` parses
    // as a strict int64 -- the same rule that makes SET k 123 int-encoded.
    void append(std::string_view value);
    void appendInteger(std::int64_t value);

    std::size_t numElements() const;
    std::size_t totalBytes() const { return buffer_.size(); }

    bool get(std::size_t index, Element& out) const;
    // Index of the first element equal to `value`, comparing numerically when
    // both sides are integers. `step` skips over interleaved values, so a hash
    // stored as [field, value, field, value] can search fields alone with step 2.
    std::optional<std::size_t> find(std::string_view value, std::size_t start = 0,
                                    std::size_t step = 1) const;

    bool eraseAt(std::size_t index);
    // Erases `count` consecutive elements -- used to drop a hash's field and
    // value together, so the pair can never be left half-removed.
    bool eraseRange(std::size_t index, std::size_t count);
    bool replaceAt(std::size_t index, std::string_view value);
    // Inserts before `index`; index == numElements() appends.
    bool insertAt(std::size_t index, std::string_view value);

    void clear();

    const std::vector<std::uint8_t>& bytes() const { return buffer_; }
    std::vector<std::string> toStrings() const;

    template <typename Fn>
    void forEach(Fn&& fn) const {
        std::size_t offset = kListpackHeaderSize;
        while (offset < buffer_.size() && buffer_[offset] != kListpackEof) {
            Element element;
            const std::size_t entry_size = decodeAt(offset, element);
            if (entry_size == 0) return;  // corrupt; stop rather than run off the end
            fn(element);
            offset += entry_size;
        }
    }

private:
    // Decodes the element at byte `offset`, returning its total size on the wire
    // (encoding + data + backlen), or 0 if the bytes are malformed.
    std::size_t decodeAt(std::size_t offset, Element& out) const;
    // Byte offset of element `index`, or npos.
    std::size_t offsetOf(std::size_t index) const;

    void setTotalBytes(std::uint32_t value);
    void setNumElements(std::size_t value);
    std::uint16_t storedCount() const;

    std::vector<std::uint8_t> buffer_;
};

// --- encoding primitives, exposed for the RDB writer and for testing ---------

// Encodes `value` (an element's encoding+data length) into `out`, most
// significant group first, with the continuation bit set on every byte except
// the first. Returns the number of bytes written (1..5).
int encodeBacklen(std::uint64_t value, std::uint8_t* out);
// Decodes a backlen by walking *backwards* from `last`, the final byte of the
// field. Returns the value and sets `bytes_read`.
std::uint64_t decodeBacklenBackwards(const std::uint8_t* last, int& bytes_read);

}  // namespace mnemos::core
