// Record batch v2 -- the container Kafka has framed messages in since 0.11, and
// the only one mnemos-kafka reads or writes.
//
// The v0/v1 message sets that preceded it are why Produce is advertised from
// v3 and Fetch from v4: below those versions a client may send the legacy
// framing, and half-supporting a second container is worse than not offering it.
//
// Compression is declined rather than approximated. The codec lives in the low
// three bits of the batch attributes, and a non-zero one is answered with
// UNSUPPORTED_COMPRESSION_TYPE -- a producer configured with the default
// `compression.type=none` is unaffected, and one that is not gets a protocol
// error it knows how to report instead of corrupt records.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kafka/wire.h"

namespace mnemos::kafka {

// CRC-32/ISCSI (Castagnoli), reflected, init and final xor 0xffffffff -- the
// variant Kafka puts in a record batch header. Distinct from the CRC-64/Jones
// in src/persist/, which is Redis's and unrelated.
// crc32c("123456789") == 0xe3069283.
std::uint32_t crc32c(std::uint32_t crc, const void* data, std::size_t len);

struct RecordHeader {
    std::string key;
    std::string value;
    bool        value_null = false;
};

// One record, with its offset and timestamp made absolute. On the wire both are
// deltas against the batch header; nothing above this layer should have to know
// that, so decode resolves them and encode re-derives them.
struct Record {
    std::int64_t              offset    = 0;
    std::int64_t              timestamp = 0;
    std::string               key;
    bool                      key_null   = true;
    std::string               value;
    bool                      value_null = true;
    std::vector<RecordHeader> headers;
};

// Decodes every batch in `blob` -- a Produce request carries one or more,
// concatenated -- appending their records. Returns None, or the error code a
// partition response should carry.
ErrorCode decodeRecordBatches(std::string_view blob, std::vector<Record>& out);

// Encodes one uncompressed batch whose first record sits at `base_offset`. The
// records' own `offset` fields are ignored: a broker assigns offsets, and this
// is where that assignment becomes wire bytes.
std::string encodeRecordBatch(std::int64_t base_offset, const std::vector<Record>& records);

// The internal, non-Kafka encoding of a single record as one value in the log.
// Storing records rather than whole batches is what makes an offset a list
// index: a batch is a variable number of records, so keeping batches would put
// an index between the offset a client asks for and the element that answers it.
std::string encodeStoredRecord(const Record& rec);
bool        decodeStoredRecord(std::string_view blob, Record& out);

}  // namespace mnemos::kafka
