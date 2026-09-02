// The RDB format: Redis's on-disk snapshot, and the DUMP/RESTORE payload that
// is the same codec with a version and a checksum stapled on.
//
// Everything here is byte-format work, so the rules are Redis's and not ours:
//
//   * Lengths are variable-width. The top two bits of the first byte say which
//     of four shapes follows -- 6-bit inline, 14-bit, a 32/64-bit length in its
//     own bytes, or "this is not a length at all but an encoding tag".
//   * Strings get three chances to be small: an integer that fits in 8, 16 or
//     32 bits is stored as that integer; anything over 20 bytes is offered to
//     LZF and kept compressed only if that saved at least four bytes; otherwise
//     the raw bytes go down behind their length.
//   * A collection is written in whatever representation it is *actually* in.
//     A small hash goes out as its listpack, verbatim; a large one as a flat
//     run of field/value strings. So the encoding survives a save and a load,
//     and OBJECT ENCODING after DEBUG RELOAD still tells the truth.
//
// The one deliberate omission is the pre-listpack encodings (ziplist, zipmap,
// the RDB_TYPE_LIST of individually-encoded elements). Redis 8 never writes
// them; a file old enough to contain them is rejected rather than half-read.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/object.h"

namespace mnemos::persist {

// The version stamped into the file header and into every DUMP footer. Redis
// refuses a payload whose version is *newer* than its own, so this number is
// what decides which real servers will accept what mnemos hands them.
inline constexpr int kRdbVersion = 15;

// CRC-64/Jones, reflected, initialised to zero -- the variant Redis uses.
// crc64(0, "123456789", 9) == 0xe9c6d914c4b8d9ca.
std::uint64_t crc64(std::uint64_t crc, const void* data, std::size_t len);

// liblzf, ported. `lzfCompress` returns 0 when the result would not fit in
// `out_len` (which is how Redis asks "did this compress at all?" -- it passes
// an output buffer four bytes shorter than the input). `lzfDecompress` returns
// 0 on malformed input or an output overrun.
std::size_t lzfCompress(const std::uint8_t* in, std::size_t in_len,
                        std::uint8_t* out, std::size_t out_len);
std::size_t lzfDecompress(const std::uint8_t* in, std::size_t in_len,
                          std::uint8_t* out, std::size_t out_len);

// --- primitives -------------------------------------------------------------

void saveLen(std::string& out, std::uint64_t len);
// The full string encoder: integer, then LZF, then raw.
void saveString(std::string& out, std::string_view s);

// A cursor over a serialised buffer. Every read is bounds-checked and returns
// false rather than throwing, because these bytes come off disk and off the
// network -- from outside, where nothing can be assumed about them.
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    bool readByte(std::uint8_t& out);
    bool readRaw(std::size_t n, std::string_view& out);
    // `encoded` reports that the first byte carried an encoding tag rather than
    // a length; `out` is then the tag, 0..3.
    bool readLen(std::uint64_t& out, bool* encoded = nullptr);
    bool readString(std::string& out);

    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return bytes_.size() - pos_; }
    bool exhausted() const { return pos_ >= bytes_.size(); }
    // Looks at the next byte without consuming it.
    bool peek(std::uint8_t& out) const;

private:
    std::string_view bytes_;
    std::size_t      pos_ = 0;
};

// --- objects ----------------------------------------------------------------

// The type byte for a value, which depends on its encoding and not only on its
// type: a small hash is 16 (HASH_LISTPACK) and a large one is 4 (HASH).
std::uint8_t objectType(const core::Value& value);
// The body only -- `objectType` supplies the byte in front of it.
void saveObjectBody(std::string& out, const core::Value& value);
// Reads a body of the given type. False on malformed or unsupported input.
bool loadObjectBody(Reader& in, std::uint8_t type, core::Value& out);

// --- DUMP / RESTORE ---------------------------------------------------------

// Type byte, body, two little-endian version bytes, eight-byte CRC64.
std::string dumpPayload(const core::Value& value);
// Checks the footer alone -- the version and the checksum, which is what Redis
// reports as "DUMP payload version or checksum are wrong".
bool payloadFooterOk(std::string_view payload);
// Deserialises a payload whose footer has already been checked. False means the
// bytes are structurally wrong, which Redis reports as "Bad data format".
bool loadPayload(std::string_view payload, core::Value& out);

// --- whole files ------------------------------------------------------------

// Streams a snapshot to `path.tmp` and renames it into place on `finish`, so a
// reader never sees a half-written file and a failed save never destroys the
// previous one.
class FileWriter {
public:
    // Returns nullopt when the temporary file cannot be created.
    static std::optional<FileWriter> create(const std::string& path);

    FileWriter(FileWriter&&) noexcept;
    FileWriter& operator=(FileWriter&&) = delete;
    FileWriter(const FileWriter&) = delete;
    ~FileWriter();

    void selectDb(int index, std::uint64_t size, std::uint64_t expires);
    void writeEntry(const std::string& key, const core::Value& value,
                    std::int64_t expire_at_ms);
    // Writes the EOF marker and the trailing checksum, then renames. False on
    // any write error, in which case the temporary file is removed.
    bool finish();

private:
    FileWriter(int fd, std::string tmp_path, std::string path);
    void append(std::string_view bytes);
    bool flush();

    int           fd_ = -1;
    std::string   tmp_path_;
    std::string   path_;
    std::string   buffer_;
    std::uint64_t crc_ = 0;
    bool          failed_ = false;
};

// Reads a snapshot, handing every key to `emit`. `expire_at_ms` is -1 for a key
// with no TTL. Returns false and sets `error` on a malformed or unreadable
// file; a file that does not exist is *not* an error (there is simply no
// snapshot yet), and `existed` reports which of the two happened.
bool loadFile(const std::string& path,
              const std::function<void(int db, std::string key, core::Value value,
                                       std::int64_t expire_at_ms)>& emit,
              bool& existed, std::string& error);

}  // namespace mnemos::persist
