// The pub/sub registry: which connections are listening to what.
#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mnemos::server {

// Redis keeps two channel namespaces whose traffic never crosses. Global
// channels are what PUBLISH reaches; shard channels exist so a cluster can
// route a message by hash slot to one shard, and only SPUBLISH reaches those.
// Patterns apply to global channels alone -- a PSUBSCRIBE that happens to match
// a shard channel's name still hears nothing from SPUBLISH.
enum class ChannelKind { Global, Shard };

// Subscriptions, stored the way delivery needs to read them. An exact channel
// hashes straight to its subscriber set, so PUBLISH costs one lookup; patterns
// have no such index and must be walked in full on every publish. That
// asymmetry is real and is why Redis documents PUBLISH as O(N+M) and keeps
// NUMPAT as a separate figure worth watching.
//
// Subscribers are held as file descriptors rather than Client pointers: a
// connection can be torn down while a publish is in flight, and an fd can be
// re-checked against the client table instead of dangling.
class PubSub {
public:
    // Each returns true when the subscription actually changed.
    bool subscribeChannel(ChannelKind kind, const std::string& channel, int fd);
    bool unsubscribeChannel(ChannelKind kind, const std::string& channel, int fd);
    bool subscribePattern(const std::string& pattern, int fd);
    bool unsubscribePattern(const std::string& pattern, int fd);

    // Null when nobody is listening. The set is never left empty: the last
    // unsubscribe erases the entry, so PUBSUB CHANNELS can simply enumerate.
    const std::set<int>* channelSubscribers(ChannelKind kind,
                                            const std::string& channel) const;

    const std::map<std::string, std::set<int>>& patterns() const { return patterns_; }

    std::vector<std::string> channelNames(ChannelKind kind) const;
    std::size_t channelSubscriberCount(ChannelKind kind, const std::string& channel) const;
    std::size_t patternCount() const { return patterns_.size(); }

private:
    using ChannelMap = std::unordered_map<std::string, std::set<int>>;

    ChannelMap&       mapFor(ChannelKind kind);
    const ChannelMap& mapFor(ChannelKind kind) const;

    ChannelMap channels_;
    ChannelMap shard_channels_;
    // Ordered, so pattern delivery and NUMPAT are deterministic run to run.
    std::map<std::string, std::set<int>> patterns_;
};

}  // namespace mnemos::server
