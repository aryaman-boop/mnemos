// Request dispatch: the five APIs that make up the produce/consume path.
//
// Deliberately absent: the consumer group coordinator (FindCoordinator,
// JoinGroup, SyncGroup, Heartbeat, OffsetCommit, OffsetFetch, LeaveGroup). That
// is a rebalance state machine with session and rebalance timers, and answering
// FindCoordinator without the rest would advertise a coordinator that then
// refuses every call -- a consumer would hang instead of failing. A client
// assigning partitions itself and tracking its own offsets works today; one
// using `subscribe()` gets a clean UNSUPPORTED_VERSION at the first group call.
//
// Also absent: replication and idempotent/transactional produce. One node is
// the leader and the sole in-sync replica of every partition, which is what the
// Metadata response says, so a client never asks for the rest.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "kafka/log.h"
#include "kafka/wire.h"

namespace mnemos::kafka {

struct BrokerConfig {
    // What Metadata hands back as this broker's address. A client connects to
    // whatever it finds here for every subsequent request, so on anything other
    // than loopback it must be an address the client can actually reach.
    std::string  advertised_host    = "127.0.0.1";
    std::int32_t advertised_port    = 9092;
    std::int32_t node_id            = 1;
    std::string  cluster_id         = "mnemos-kafka";
    std::int32_t default_partitions = 1;
    bool         auto_create_topics = true;
};

class Broker {
public:
    Broker(BrokerConfig config, PartitionLog& log)
        : config_(std::move(config)), log_(log) {}

    struct Outcome {
        enum class Kind {
            Respond,  // `response` is a complete length-prefixed frame
            Silence,  // a valid request that is answered with nothing (acks=0)
            Retry,    // no data yet, and the client asked to wait for some
            Close,    // unanswerable: the connection is the only thing to do
        };

        Kind         kind = Kind::Respond;
        std::string  response;
        std::int32_t retry_in_ms = 0;
        // On Retry, how long the client said it was willing to wait in total.
        // The caller turns that into a deadline once and then keeps it, so a
        // re-run of the same frame does not restart the clock.
        std::int32_t max_wait_ms = 0;
    };

    // `frame` is one request with its four-byte length prefix already removed.
    // `allow_wait` is false once a fetch's deadline has passed, which turns an
    // empty read from Retry into an empty Respond.
    Outcome handleRequest(std::string_view frame, bool allow_wait);

    const BrokerConfig& config() const { return config_; }

private:
    // Each of these writes only the response *body*; the header and the length
    // prefix are added once, by handleRequest.
    bool apiVersions(std::int16_t version, Writer& w);
    bool metadata(std::int16_t version, Reader& r, Writer& w);
    bool produce(std::int16_t version, Reader& r, Writer& w, bool& silent);
    bool fetch(std::int16_t version, Reader& r, Writer& w, bool allow_wait, bool& retry,
               std::int32_t& wait_for);
    bool listOffsets(std::int16_t version, Reader& r, Writer& w);

    // Resolves a topic to its partition count, creating it when the config and
    // the caller both allow it. Returns 0 for a topic that does not exist.
    std::int32_t partitionsFor(const std::string& topic, bool may_create);

    BrokerConfig  config_;
    PartitionLog& log_;
};

}  // namespace mnemos::kafka
