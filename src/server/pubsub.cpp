#include "server/pubsub.h"

namespace mnemos::server {

namespace {

template <typename Map>
bool addTo(Map& map, const std::string& key, int fd) {
    return map[key].insert(fd).second;
}

template <typename Map>
bool removeFrom(Map& map, const std::string& key, int fd) {
    auto it = map.find(key);
    if (it == map.end()) return false;
    const bool erased = it->second.erase(fd) > 0;
    // Dropping the empty set keeps "has an entry" and "has a subscriber" the
    // same question, which PUBSUB CHANNELS and NUMPAT both rely on.
    if (it->second.empty()) map.erase(it);
    return erased;
}

}  // namespace

PubSub::ChannelMap& PubSub::mapFor(ChannelKind kind) {
    return kind == ChannelKind::Shard ? shard_channels_ : channels_;
}

const PubSub::ChannelMap& PubSub::mapFor(ChannelKind kind) const {
    return kind == ChannelKind::Shard ? shard_channels_ : channels_;
}

bool PubSub::subscribeChannel(ChannelKind kind, const std::string& channel, int fd) {
    return addTo(mapFor(kind), channel, fd);
}

bool PubSub::unsubscribeChannel(ChannelKind kind, const std::string& channel, int fd) {
    return removeFrom(mapFor(kind), channel, fd);
}

bool PubSub::subscribePattern(const std::string& pattern, int fd) {
    return addTo(patterns_, pattern, fd);
}

bool PubSub::unsubscribePattern(const std::string& pattern, int fd) {
    return removeFrom(patterns_, pattern, fd);
}

const std::set<int>* PubSub::channelSubscribers(ChannelKind kind,
                                                const std::string& channel) const {
    const ChannelMap& map = mapFor(kind);
    auto it = map.find(channel);
    return it == map.end() ? nullptr : &it->second;
}

std::vector<std::string> PubSub::channelNames(ChannelKind kind) const {
    const ChannelMap& map = mapFor(kind);
    std::vector<std::string> names;
    names.reserve(map.size());
    for (const auto& [name, subscribers] : map) names.push_back(name);
    return names;
}

std::size_t PubSub::channelSubscriberCount(ChannelKind kind,
                                           const std::string& channel) const {
    const std::set<int>* subscribers = channelSubscribers(kind, channel);
    return subscribers ? subscribers->size() : 0;
}

}  // namespace mnemos::server
