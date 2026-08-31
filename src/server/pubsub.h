// The pub/sub registry: which connections are listening to what.
#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mnemos::server {

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
    bool subscribeChannel(const std::string& channel, int fd);
    bool unsubscribeChannel(const std::string& channel, int fd);
    bool subscribePattern(const std::string& pattern, int fd);
    bool unsubscribePattern(const std::string& pattern, int fd);

    // Null when nobody is listening. The set is never left empty: the last
    // unsubscribe erases the entry, so PUBSUB CHANNELS can simply enumerate.
    const std::set<int>* channelSubscribers(const std::string& channel) const;

    const std::map<std::string, std::set<int>>& patterns() const { return patterns_; }

    std::vector<std::string> channelNames() const;
    std::size_t channelSubscriberCount(const std::string& channel) const;
    std::size_t patternCount() const { return patterns_.size(); }

private:
    std::unordered_map<std::string, std::set<int>> channels_;
    // Ordered, so pattern delivery and NUMPAT are deterministic run to run.
    std::map<std::string, std::set<int>>           patterns_;
};

}  // namespace mnemos::server
