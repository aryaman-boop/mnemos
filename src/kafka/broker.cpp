#include "kafka/broker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace mnemos::kafka {
namespace {

// The versions advertised through ApiVersions, and the whole reason no flexible
// encoding exists in this codebase: every maximum here is one below the version
// where that API grew tagged fields. Raising one means implementing them.
//
//   Produce      v9  flexible   -- and v0-v2 carry legacy message sets, so min 3
//   Fetch        v12 flexible   -- v4 is the first with isolation_level, so min 4
//   ListOffsets  v6  flexible   -- v0 answered with an offset array, so min 1
//   Metadata     v9  flexible
//   ApiVersions  v3  flexible
struct ApiSupport {
    ApiKey       key;
    std::int16_t min_version;
    std::int16_t max_version;
};

constexpr ApiSupport kApis[] = {
    {ApiKey::Produce,     3, 7},
    {ApiKey::Fetch,       4, 11},
    {ApiKey::ListOffsets, 1, 5},
    {ApiKey::Metadata,    0, 8},
    {ApiKey::ApiVersions, 0, 2},
};

// Kafka's own sentinel for "authorized operations were not requested".
constexpr std::int32_t kNoAuthorizedOperations = INT32_MIN;

// How many records one fetch will read from the log before byte-capping. A
// bound is needed because LRANGE would otherwise materialise a whole partition.
constexpr std::int32_t kMaxFetchRecords = 4096;

// How long a waiting fetch sleeps before looking again. This is a poll, not a
// signal: roadmap item 8's ready-key registry is what replaces it, at which
// point the tick disappears rather than shrinks.
constexpr std::int32_t kFetchPollMs = 25;

// The longest a fetch is parked, whatever the client asked for.
constexpr std::int32_t kMaxFetchWaitMs = 30000;

const ApiSupport* findApi(std::int16_t key) {
    for (const auto& api : kApis) {
        if (static_cast<std::int16_t>(api.key) == key) return &api;
    }
    return nullptr;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A rough encoded size for one record, used to honour a fetch's byte budget
// without encoding the batch once per candidate prefix. Kafka's max_bytes is a
// soft limit -- a broker may exceed it to return a first record -- so an
// estimate within a few bytes per record is within spec.
std::size_t estimateRecordSize(const Record& rec) {
    std::size_t size = 16;  // attributes, two varint deltas, three varint lengths
    size += rec.key.size() + rec.value.size();
    for (const auto& h : rec.headers) size += h.key.size() + h.value.size() + 4;
    return size;
}

}  // namespace

std::int32_t Broker::partitionsFor(const std::string& topic, bool may_create) {
    std::string  error;
    std::int32_t partitions = 0;
    if (!log_.topicPartitions(topic, partitions, error)) {
        std::fprintf(stderr, "mnemos-kafka: %s\n", error.c_str());
        log_.disconnect();
        return 0;
    }
    if (partitions == 0 && may_create && config_.auto_create_topics) {
        if (!log_.createTopic(topic, config_.default_partitions, error)) {
            std::fprintf(stderr, "mnemos-kafka: %s\n", error.c_str());
            log_.disconnect();
            return 0;
        }
        partitions = config_.default_partitions;
    }
    return partitions;
}

Broker::Outcome Broker::handleRequest(std::string_view frame, bool allow_wait) {
    Outcome outcome;

    Reader        r(frame);
    RequestHeader header;
    if (!parseRequestHeader(r, header)) {
        outcome.kind = Outcome::Kind::Close;
        return outcome;
    }

    const ApiSupport* api = findApi(header.api_key);

    // An unknown API key cannot be answered: the response schema is exactly
    // what we do not have. Closing is what a broker does with a frame it cannot
    // parse, and it is the only thing that leaves the client's stream coherent.
    if (api == nullptr) {
        std::fprintf(stderr, "mnemos-kafka: unsupported api key %d from '%s'\n", header.api_key,
                     header.client_id.c_str());
        outcome.kind = Outcome::Kind::Close;
        return outcome;
    }

    const bool version_ok =
        header.api_version >= api->min_version && header.api_version <= api->max_version;

    // ApiVersions is the one API that answers an unsupported version rather
    // than closing, and it answers using version 0 of the response. That is how
    // a client that opened with a newer version learns what to downgrade to, so
    // it is load-bearing rather than a nicety.
    if (!version_ok && api->key != ApiKey::ApiVersions) {
        std::fprintf(stderr, "mnemos-kafka: api %d version %d outside [%d, %d]\n", header.api_key,
                     header.api_version, api->min_version, api->max_version);
        outcome.kind = Outcome::Kind::Close;
        return outcome;
    }

    Writer body;
    bool         ok       = true;
    bool         silent   = false;
    bool         retry    = false;
    std::int32_t wait_for = 0;

    switch (api->key) {
        case ApiKey::ApiVersions:
            // A rejected version still gets the api_keys array, and the header
            // it is framed with is v0 either way -- ApiVersions never carries a
            // flexible response header, even at v3 and above.
            ok = apiVersions(version_ok ? header.api_version : 0, body);
            if (!version_ok) {
                Writer replacement;
                replacement.errorCode(ErrorCode::UnsupportedVersion);
                replacement.arrayLen(static_cast<std::int32_t>(std::size(kApis)));
                for (const auto& entry : kApis) {
                    replacement.int16(static_cast<std::int16_t>(entry.key));
                    replacement.int16(entry.min_version);
                    replacement.int16(entry.max_version);
                }
                body = std::move(replacement);
            }
            break;
        case ApiKey::Metadata:    ok = metadata(header.api_version, r, body); break;
        case ApiKey::Produce:     ok = produce(header.api_version, r, body, silent); break;
        case ApiKey::Fetch:
            ok = fetch(header.api_version, r, body, allow_wait, retry, wait_for);
            break;
        case ApiKey::ListOffsets: ok = listOffsets(header.api_version, r, body); break;
    }

    if (!ok) {
        outcome.kind = Outcome::Kind::Close;
        return outcome;
    }
    if (retry) {
        outcome.kind        = Outcome::Kind::Retry;
        outcome.retry_in_ms = kFetchPollMs;
        outcome.max_wait_ms = wait_for;
        return outcome;
    }
    if (silent) {
        outcome.kind = Outcome::Kind::Silence;
        return outcome;
    }

    // Frame it: size, then response header v0 (a correlation id and nothing
    // else), then the body. No API we advertise uses response header v1.
    Writer            out;
    const std::size_t size_at = out.reserveInt32();
    out.int32(header.correlation_id);
    out.raw(body.str());
    out.patchInt32(size_at, static_cast<std::int32_t>(out.size() - size_at - 4));

    outcome.response = out.take();
    return outcome;
}

// --------------------------------------------------------------- ApiVersions

bool Broker::apiVersions(std::int16_t version, Writer& w) {
    w.errorCode(ErrorCode::None);
    w.arrayLen(static_cast<std::int32_t>(std::size(kApis)));
    for (const auto& api : kApis) {
        w.int16(static_cast<std::int16_t>(api.key));
        w.int16(api.min_version);
        w.int16(api.max_version);
    }
    if (version >= 1) w.int32(0);  // throttle_time_ms
    return true;
}

// ------------------------------------------------------------------ Metadata

bool Broker::metadata(std::int16_t version, Reader& r, Writer& w) {
    bool                     all_topics = false;
    std::vector<std::string> requested;
    {
        bool               was_null = false;
        const std::int32_t count    = r.arrayLen(&was_null);
        for (std::int32_t i = 0; i < count; ++i) requested.push_back(r.string());
        // A null topic array means "every topic". At v0 there is no null, and
        // an empty array carries that meaning instead; from v1 an empty array
        // means literally no topics, so the two cannot be collapsed.
        all_topics = was_null || (version == 0 && count == 0);
    }
    bool may_create = config_.auto_create_topics;
    if (version >= 4) may_create = r.boolean() && config_.auto_create_topics;
    if (version >= 8) {
        r.boolean();  // include_cluster_authorized_operations
        r.boolean();  // include_topic_authorized_operations
    }
    if (!r.ok()) return false;

    if (all_topics) {
        std::string error;
        if (!log_.listTopics(requested, error)) {
            std::fprintf(stderr, "mnemos-kafka: %s\n", error.c_str());
            log_.disconnect();
            return false;
        }
        may_create = false;  // nothing to create: this is the list of what is
    }

    if (version >= 3) w.int32(0);  // throttle_time_ms

    w.arrayLen(1);
    w.int32(config_.node_id);
    w.string(config_.advertised_host);
    w.int32(config_.advertised_port);
    if (version >= 1) w.nullString();  // rack

    if (version >= 2) w.string(config_.cluster_id);
    if (version >= 1) w.int32(config_.node_id);  // controller

    w.arrayLen(static_cast<std::int32_t>(requested.size()));
    for (const auto& topic : requested) {
        const std::int32_t partitions = partitionsFor(topic, may_create);

        w.errorCode(partitions > 0 ? ErrorCode::None : ErrorCode::UnknownTopicOrPartition);
        w.string(topic);
        if (version >= 1) w.boolean(false);  // is_internal

        w.arrayLen(partitions);
        for (std::int32_t p = 0; p < partitions; ++p) {
            w.errorCode(ErrorCode::None);
            w.int32(p);
            w.int32(config_.node_id);            // leader
            if (version >= 7) w.int32(0);        // leader epoch: never advances
            w.arrayLen(1);                       // replicas
            w.int32(config_.node_id);
            w.arrayLen(1);                       // in-sync replicas
            w.int32(config_.node_id);
            if (version >= 5) w.arrayLen(0);     // offline replicas
        }
        if (version >= 8) w.int32(kNoAuthorizedOperations);
    }
    if (version >= 8) w.int32(kNoAuthorizedOperations);
    return true;
}

// ------------------------------------------------------------------- Produce

bool Broker::produce(std::int16_t version, Reader& r, Writer& w, bool& silent) {
    if (version >= 3) {
        std::string transactional_id;
        r.nullableString(transactional_id);
    }
    const std::int16_t acks = r.int16();
    r.int32();  // timeout_ms: every write here is already durable-or-failed
    if (!r.ok()) return false;

    // acks=0 is fire and forget, and the client is not reading a reply for it.
    // Sending one would be interpreted as the answer to whatever it asks next.
    silent = (acks == 0);

    const std::int32_t topic_count = r.arrayLen();
    if (!r.ok()) return false;
    w.arrayLen(topic_count);

    for (std::int32_t t = 0; t < topic_count; ++t) {
        const std::string  topic           = r.string();
        const std::int32_t partition_count = r.arrayLen();
        if (!r.ok()) return false;

        const std::int32_t partitions = partitionsFor(topic, true);

        w.string(topic);
        w.arrayLen(partition_count);
        for (std::int32_t p = 0; p < partition_count; ++p) {
            const std::int32_t index = r.int32();
            std::string_view   blob;
            const bool         has_records = r.nullableBytes(blob);
            if (!r.ok()) return false;

            ErrorCode    error       = ErrorCode::None;
            std::int64_t base_offset = -1;

            if (index < 0 || index >= partitions) {
                error = ErrorCode::UnknownTopicOrPartition;
            } else {
                std::vector<Record> records;
                if (has_records) error = decodeRecordBatches(blob, records);
                if (error == ErrorCode::None) {
                    // The broker stamps LogAppendTime on a record the producer
                    // left unset (-1), which is what makes offsetForTimestamp's
                    // ordering assumption hold for the common case.
                    const std::int64_t now = nowMs();
                    for (auto& rec : records) {
                        if (rec.timestamp < 0) rec.timestamp = now;
                    }
                    std::string message;
                    if (!log_.append(topic, index, records, base_offset, message)) {
                        std::fprintf(stderr, "mnemos-kafka: %s\n", message.c_str());
                        log_.disconnect();
                        error       = ErrorCode::KafkaStorageError;
                        base_offset = -1;
                    }
                }
            }

            w.int32(index);
            w.errorCode(error);
            w.int64(base_offset);
            if (version >= 2) w.int64(-1);  // log_append_time: records keep CreateTime
            if (version >= 5) w.int64(0);   // log_start_offset: nothing is ever trimmed
        }
    }
    if (version >= 1) w.int32(0);  // throttle_time_ms
    return true;
}

// --------------------------------------------------------------------- Fetch

bool Broker::fetch(std::int16_t version, Reader& r, Writer& w, bool allow_wait, bool& retry,
                   std::int32_t& wait_for) {
    r.int32();  // replica_id: only a consumer ever reaches this broker
    const std::int32_t max_wait_ms = r.int32();
    const std::int32_t min_bytes   = r.int32();
    if (version >= 3) r.int32();  // max_bytes: the per-partition cap is enough
    if (version >= 4) r.int8();   // isolation_level: no transactions to isolate
    std::int32_t session_id = 0;
    if (version >= 7) {
        session_id = r.int32();
        r.int32();  // session_epoch
    }
    if (!r.ok()) return false;

    struct FetchedPartition {
        std::int32_t index      = 0;
        ErrorCode    error      = ErrorCode::None;
        std::int64_t high_water = 0;
        std::string  records;
    };
    struct FetchedTopic {
        std::string                   name;
        std::vector<FetchedPartition> partitions;
    };

    std::vector<FetchedTopic> results;
    std::size_t               total_bytes = 0;

    const std::int32_t topic_count = r.arrayLen();
    if (!r.ok()) return false;
    for (std::int32_t t = 0; t < topic_count; ++t) {
        FetchedTopic out;
        out.name                           = r.string();
        const std::int32_t partition_count = r.arrayLen();
        if (!r.ok()) return false;

        const std::int32_t partitions = partitionsFor(out.name, false);

        for (std::int32_t p = 0; p < partition_count; ++p) {
            FetchedPartition fetched;
            fetched.index = r.int32();
            if (version >= 9) r.int32();  // current_leader_epoch
            const std::int64_t fetch_offset = r.int64();
            if (version >= 5) r.int64();  // log_start_offset, a replica's concern
            const std::int32_t partition_max_bytes = r.int32();
            if (!r.ok()) return false;

            if (fetched.index < 0 || fetched.index >= partitions) {
                fetched.error = ErrorCode::UnknownTopicOrPartition;
                out.partitions.push_back(std::move(fetched));
                continue;
            }

            std::string error;
            if (!log_.highWatermark(out.name, fetched.index, fetched.high_water, error)) {
                std::fprintf(stderr, "mnemos-kafka: %s\n", error.c_str());
                log_.disconnect();
                fetched.error = ErrorCode::KafkaStorageError;
                out.partitions.push_back(std::move(fetched));
                continue;
            }
            // Past the end is out of range; exactly at it is a caught-up
            // consumer, which is the single most common fetch there is.
            if (fetch_offset < 0 || fetch_offset > fetched.high_water) {
                fetched.error = ErrorCode::OffsetOutOfRange;
                out.partitions.push_back(std::move(fetched));
                continue;
            }

            std::vector<Record> records;
            if (!log_.read(out.name, fetched.index, fetch_offset, kMaxFetchRecords, records,
                           error)) {
                std::fprintf(stderr, "mnemos-kafka: %s\n", error.c_str());
                log_.disconnect();
                fetched.error = ErrorCode::KafkaStorageError;
                out.partitions.push_back(std::move(fetched));
                continue;
            }

            // Trim to the partition's byte budget, but never to nothing: a
            // record larger than the budget is still returned, or a consumer
            // whose max.partition.fetch.bytes is below it would stall forever.
            std::size_t budget = 0;
            std::size_t kept   = 0;
            for (; kept < records.size(); ++kept) {
                budget += estimateRecordSize(records[kept]);
                if (kept > 0 && partition_max_bytes > 0 &&
                    budget > static_cast<std::size_t>(partition_max_bytes)) {
                    break;
                }
            }
            records.resize(kept);

            if (!records.empty()) {
                fetched.records = encodeRecordBatch(fetch_offset, records);
                total_bytes += fetched.records.size();
            }
            out.partitions.push_back(std::move(fetched));
        }
        results.push_back(std::move(out));
    }
    if (version >= 7) {
        const std::int32_t forgotten = r.arrayLen();
        for (std::int32_t i = 0; i < forgotten; ++i) {
            r.string();
            const std::int32_t count = r.arrayLen();
            for (std::int32_t j = 0; j < count; ++j) r.int32();
        }
    }
    if (version >= 11) r.string();  // rack_id
    if (!r.ok()) return false;

    // The client asked to wait for min_bytes and got less. Rather than answer
    // empty and have it come straight back, park the request -- see kFetchPollMs
    // for why this is a poll and what replaces it.
    if (allow_wait && max_wait_ms > 0 &&
        total_bytes < static_cast<std::size_t>(std::max(min_bytes, 1))) {
        retry = true;
        // Capped so one client cannot pin a connection indefinitely by asking
        // for an hour of patience; Kafka's own default is 500ms.
        wait_for = std::min(max_wait_ms, kMaxFetchWaitMs);
        return true;
    }

    if (version >= 1) w.int32(0);  // throttle_time_ms
    if (version >= 7) {
        w.errorCode(ErrorCode::None);
        w.int32(session_id);
    }
    w.arrayLen(static_cast<std::int32_t>(results.size()));
    for (const auto& topic : results) {
        w.string(topic.name);
        w.arrayLen(static_cast<std::int32_t>(topic.partitions.size()));
        for (const auto& p : topic.partitions) {
            w.int32(p.index);
            w.errorCode(p.error);
            w.int64(p.high_water);
            if (version >= 4) w.int64(p.high_water);  // last stable offset
            if (version >= 5) w.int64(0);             // log start offset
            if (version >= 4) w.int32(-1);            // aborted transactions: null
            if (version >= 11) w.int32(-1);           // preferred read replica
            w.bytes(p.records);
        }
    }
    return true;
}

// --------------------------------------------------------------- ListOffsets

bool Broker::listOffsets(std::int16_t version, Reader& r, Writer& w) {
    r.int32();                    // replica_id
    if (version >= 2) r.int8();   // isolation_level
    if (!r.ok()) return false;

    const std::int32_t topic_count = r.arrayLen();
    if (!r.ok()) return false;

    if (version >= 2) w.int32(0);  // throttle_time_ms
    w.arrayLen(topic_count);

    for (std::int32_t t = 0; t < topic_count; ++t) {
        const std::string  topic           = r.string();
        const std::int32_t partition_count = r.arrayLen();
        if (!r.ok()) return false;

        const std::int32_t partitions = partitionsFor(topic, false);

        w.string(topic);
        w.arrayLen(partition_count);
        for (std::int32_t p = 0; p < partition_count; ++p) {
            const std::int32_t index = r.int32();
            if (version >= 4) r.int32();  // current_leader_epoch
            const std::int64_t timestamp = r.int64();
            if (!r.ok()) return false;

            ErrorCode    error  = ErrorCode::None;
            std::int64_t offset = -1;

            if (index < 0 || index >= partitions) {
                error = ErrorCode::UnknownTopicOrPartition;
            } else {
                std::string error_text;
                bool        ok = true;
                if (timestamp == -2) {          // earliest
                    offset = 0;
                } else if (timestamp == -1) {   // latest
                    ok = log_.highWatermark(topic, index, offset, error_text);
                } else {
                    ok = log_.offsetForTimestamp(topic, index, timestamp, offset, error_text);
                }
                if (!ok) {
                    std::fprintf(stderr, "mnemos-kafka: %s\n", error_text.c_str());
                    log_.disconnect();
                    error  = ErrorCode::KafkaStorageError;
                    offset = -1;
                }
            }

            w.int32(index);
            w.errorCode(error);
            // The timestamp field answers a timestamp query; for the earliest
            // and latest sentinels Kafka reports -1, not the record's own time.
            w.int64(-1);
            w.int64(offset);
            if (version >= 4) w.int32(-1);  // leader epoch
        }
    }
    return true;
}

}  // namespace mnemos::kafka
