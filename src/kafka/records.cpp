#include "kafka/records.h"

#include <array>

namespace mnemos::kafka {
namespace {

// The reflected Castagnoli table, built once at namespace scope. constexpr so
// it lands in .rodata rather than costing a guarded initialisation on a path
// that runs per batch.
constexpr std::array<std::uint32_t, 256> makeCrc32cTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x82f63b78u : crc >> 1;
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto kCrc32cTable = makeCrc32cTable();

constexpr std::int8_t  kMagicV2         = 2;
constexpr std::int16_t kCompressionMask = 0x07;

// A batch header is fixed-width up to the record count: 8 base offset, 4 length,
// 4 leader epoch, 1 magic, 4 crc, 2 attributes, 4 last offset delta, 8+8
// timestamps, 8 producer id, 2 producer epoch, 4 base sequence, 4 count.
constexpr std::size_t kBatchHeaderSize = 61;
// Offset of `attributes` -- the first byte the CRC covers.
constexpr std::size_t kCrcCoverageStart = 21;

}  // namespace

std::uint32_t crc32c(std::uint32_t crc, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    crc          = ~crc;
    while (len--) crc = kCrc32cTable[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

ErrorCode decodeRecordBatches(std::string_view blob, std::vector<Record>& out) {
    // A Produce request carries one or more batches concatenated, and the
    // batch length -- not the record count -- is what says where each one ends.
    std::size_t pos = 0;
    while (pos < blob.size()) {
        if (blob.size() - pos < kBatchHeaderSize) return ErrorCode::InvalidRequest;

        Reader r(blob.substr(pos));
        r.int64();  // base offset: the producer's guess, and the broker's call
        const std::int32_t batch_len = r.int32();
        if (!r.ok() || batch_len < 0) return ErrorCode::InvalidRequest;

        // batch_len counts every byte after itself.
        const std::size_t body_start = pos + r.position();
        if (blob.size() - body_start < static_cast<std::size_t>(batch_len)) {
            return ErrorCode::InvalidRequest;
        }
        const std::size_t batch_end = body_start + static_cast<std::size_t>(batch_len);

        r.int32();  // partition leader epoch
        if (r.int8() != kMagicV2) return ErrorCode::InvalidRequest;

        const std::uint32_t stated_crc = r.uint32();
        const std::size_t   crc_from   = pos + kCrcCoverageStart;
        if (!r.ok() || crc_from > batch_end) return ErrorCode::InvalidRequest;
        // A CRC mismatch is storage-shaped rather than request-shaped: the bytes
        // were meant to be these and arrived otherwise.
        if (crc32c(0, blob.data() + crc_from, batch_end - crc_from) != stated_crc) {
            return ErrorCode::KafkaStorageError;
        }

        const std::int16_t attributes = r.int16();
        if ((attributes & kCompressionMask) != 0) return ErrorCode::UnsupportedCompression;

        r.int32();                                     // last offset delta
        const std::int64_t base_timestamp = r.int64();
        r.int64();                                     // max timestamp
        r.int64();                                     // producer id
        r.int16();                                     // producer epoch
        r.int32();                                     // base sequence
        const std::int32_t count = r.int32();
        if (!r.ok() || count < 0) return ErrorCode::InvalidRequest;

        // Everything from here to batch_end is the record area. Bound the
        // sub-reader by it so a record length cannot read into the next batch.
        const std::size_t records_at = pos + r.position();
        if (records_at > batch_end) return ErrorCode::InvalidRequest;
        Reader area(blob.substr(records_at, batch_end - records_at));

        for (std::int32_t i = 0; i < count; ++i) {
            std::string_view rec_bytes;
            const std::int32_t rec_len = area.varint();
            if (!area.ok() || rec_len < 0 ||
                !area.takeView(static_cast<std::size_t>(rec_len), rec_bytes)) {
                return ErrorCode::InvalidRequest;
            }
            Reader rec(rec_bytes);

            Record r_out;
            rec.int8();  // per-record attributes: reserved by the format, unused
            r_out.timestamp = base_timestamp + rec.varlong();
            rec.varint();  // offset delta: the broker assigns offsets, not this

            std::string_view view;
            if (!rec.varintBytes(view, r_out.key_null)) return ErrorCode::InvalidRequest;
            r_out.key.assign(view);
            if (!rec.varintBytes(view, r_out.value_null)) return ErrorCode::InvalidRequest;
            r_out.value.assign(view);

            const std::int32_t header_count = rec.varint();
            if (!rec.ok() || header_count < 0 ||
                static_cast<std::size_t>(header_count) > rec.remaining()) {
                return ErrorCode::InvalidRequest;
            }
            for (std::int32_t h = 0; h < header_count; ++h) {
                RecordHeader hdr;
                bool         key_null = false;
                if (!rec.varintBytes(view, key_null) || key_null) {
                    return ErrorCode::InvalidRequest;  // a header key is not nullable
                }
                hdr.key.assign(view);
                if (!rec.varintBytes(view, hdr.value_null)) return ErrorCode::InvalidRequest;
                hdr.value.assign(view);
                r_out.headers.push_back(std::move(hdr));
            }
            if (!rec.ok()) return ErrorCode::InvalidRequest;
            out.push_back(std::move(r_out));
        }
        if (!area.ok()) return ErrorCode::InvalidRequest;

        pos = batch_end;
    }
    return ErrorCode::None;
}

std::string encodeRecordBatch(std::int64_t base_offset, const std::vector<Record>& records) {
    Writer w;
    w.int64(base_offset);
    const std::size_t len_at = w.reserveInt32();

    const std::size_t body_start = w.size();
    w.int32(0);             // partition leader epoch: single node, never advanced
    w.int8(kMagicV2);
    const std::size_t crc_at = w.reserveInt32();

    const std::size_t crc_start = w.size();
    w.int16(0);             // attributes: no compression, CreateTime, not control

    std::int64_t base_ts = records.empty() ? -1 : records.front().timestamp;
    std::int64_t max_ts  = base_ts;
    for (const auto& r : records) {
        if (r.timestamp > max_ts) max_ts = r.timestamp;
    }

    w.int32(records.empty() ? 0 : static_cast<std::int32_t>(records.size() - 1));
    w.int64(base_ts);
    w.int64(max_ts);
    w.int64(-1);            // producer id: idempotent produce is not supported
    w.int16(-1);            // producer epoch
    w.int32(-1);            // base sequence
    w.int32(static_cast<std::int32_t>(records.size()));

    for (std::size_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];

        // A record's length prefix is a varint, so it cannot be reserved and
        // patched at a fixed width -- build the body, then prefix it.
        Writer body;
        body.int8(0);
        body.varlong(r.timestamp - base_ts);
        body.varint(static_cast<std::int32_t>(i));
        if (r.key_null) {
            body.varint(-1);
        } else {
            body.varint(static_cast<std::int32_t>(r.key.size()));
            body.raw(r.key);
        }
        if (r.value_null) {
            body.varint(-1);
        } else {
            body.varint(static_cast<std::int32_t>(r.value.size()));
            body.raw(r.value);
        }
        body.varint(static_cast<std::int32_t>(r.headers.size()));
        for (const auto& h : r.headers) {
            body.varint(static_cast<std::int32_t>(h.key.size()));
            body.raw(h.key);
            if (h.value_null) {
                body.varint(-1);
            } else {
                body.varint(static_cast<std::int32_t>(h.value.size()));
                body.raw(h.value);
            }
        }
        w.varint(static_cast<std::int32_t>(body.size()));
        w.raw(body.str());
    }

    w.patchInt32(len_at, static_cast<std::int32_t>(w.size() - body_start));
    w.patchUint32(crc_at, crc32c(0, w.str().data() + crc_start, w.size() - crc_start));
    return w.take();
}

// ------------------------------------------------------- the stored form

std::string encodeStoredRecord(const Record& rec) {
    Writer w;
    w.int8(1);  // version, so a later field can be added without a migration
    w.int64(rec.timestamp);
    if (rec.key_null) {
        w.nullBytes();
    } else {
        w.bytes(rec.key);
    }
    if (rec.value_null) {
        w.nullBytes();
    } else {
        w.bytes(rec.value);
    }
    w.int32(static_cast<std::int32_t>(rec.headers.size()));
    for (const auto& h : rec.headers) {
        w.bytes(h.key);
        if (h.value_null) {
            w.nullBytes();
        } else {
            w.bytes(h.value);
        }
    }
    return w.take();
}

bool decodeStoredRecord(std::string_view blob, Record& out) {
    Reader r(blob);
    if (r.int8() != 1) return false;
    out.timestamp = r.int64();

    std::string_view view;
    out.key_null = !r.nullableBytes(view);
    out.key.assign(view);
    out.value_null = !r.nullableBytes(view);
    out.value.assign(view);

    const std::int32_t headers = r.int32();
    if (!r.ok() || headers < 0 || static_cast<std::size_t>(headers) > r.remaining()) return false;
    out.headers.clear();
    for (std::int32_t i = 0; i < headers; ++i) {
        RecordHeader h;
        if (!r.nullableBytes(view)) return false;
        h.key.assign(view);
        h.value_null = !r.nullableBytes(view);
        h.value.assign(view);
        out.headers.push_back(std::move(h));
    }
    return r.ok();
}

}  // namespace mnemos::kafka
