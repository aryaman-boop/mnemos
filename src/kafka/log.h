// The partition log, expressed as mnemos keys.
//
// A partition is a list and an offset is an index into it. That equivalence is
// only exact because nothing ever pops: `RPUSH` returns the new length, so the
// base offset of an append is that length minus the number of records appended,
// and `LRANGE offset offset+n-1` answers a fetch with no index in between.
// Retention and compaction would both break it, which is the honest reason
// neither is implemented rather than half-implemented.
//
// When roadmap item 9 lands, a stream replaces the list here and nothing above
// this header changes -- the protocol layer only ever sees offsets and records.
//
// Keys, all under one prefix so a keyspace shared with ordinary data stays
// legible:
//   kafka:topics            SET   -- every known topic name
//   kafka:meta:<topic>      HASH  -- field `partitions`
//   kafka:log:<topic>:<p>   LIST  -- the records, one per element
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "client/resp_client.h"
#include "kafka/records.h"

namespace mnemos::kafka {

class PartitionLog {
public:
    PartitionLog(std::string host, int port, int db, int timeout_ms)
        : host_(std::move(host)), port_(port), db_(db), timeout_ms_(timeout_ms) {}

    // Connects if not already connected. Every entry point calls this, so a
    // server restart underneath the broker costs one failed request, not the
    // session -- the same reconnect policy mnemos-mcp uses.
    bool ensureConnected(std::string& error);
    void disconnect() { client_.close(); }

    bool createTopic(const std::string& topic, std::int32_t partitions, std::string& error);
    // Partition count, or 0 when the topic does not exist.
    bool topicPartitions(const std::string& topic, std::int32_t& out, std::string& error);
    bool listTopics(std::vector<std::string>& out, std::string& error);

    // Appends in one round trip and reports the offset the first record landed
    // at. Atomic because mnemos runs commands on one thread: no other producer
    // can interleave between the append and the length it returns.
    bool append(const std::string& topic, std::int32_t partition,
                const std::vector<Record>& records, std::int64_t& base_offset,
                std::string& error);

    // The offset one past the last record -- what a consumer compares its
    // position against, and what ListOffsets answers for `latest`.
    bool highWatermark(const std::string& topic, std::int32_t partition, std::int64_t& out,
                       std::string& error);

    // Records from `offset` onward, capped by count here and by bytes in the
    // caller. An offset at or past the high watermark reads back empty.
    bool read(const std::string& topic, std::int32_t partition, std::int64_t offset,
              std::int32_t max_records, std::vector<Record>& out, std::string& error);

    // The first offset whose record timestamp is >= `timestamp`, or -1 when no
    // record is that new. Binary search, which assumes timestamps do not go
    // backwards within a partition; a producer that back-dates a record can
    // therefore be missed, exactly as it can in a real broker's time index.
    bool offsetForTimestamp(const std::string& topic, std::int32_t partition,
                            std::int64_t timestamp, std::int64_t& out, std::string& error);

private:
    static std::string logKey(const std::string& topic, std::int32_t partition);
    static std::string metaKey(const std::string& topic);

    // Runs one command, treating a RESP error reply as a failure -- unlike the
    // client's own contract, since nothing here issues a command whose error
    // reply is a normal outcome.
    bool call(const std::vector<std::string>& argv, net::Reply& out, std::string& error);

    client::RespClient client_;
    std::string        host_;
    int                port_       = 6380;
    int                db_         = 0;
    int                timeout_ms_ = 5000;
};

}  // namespace mnemos::kafka
