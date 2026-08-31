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

bool PubSub::subscribeChannel(const std::string& channel, int fd) {
    return addTo(channels_, channel, fd);
}

bool PubSub::unsubscribeChannel(const std::string& channel, int fd) {
    return removeFrom(channels_, channel, fd);
}

bool PubSub::subscribePattern(const std::string& pattern, int fd) {
    return addTo(patterns_, pattern, fd);
}

bool PubSub::unsubscribePattern(const std::string& pattern, int fd) {
    return removeFrom(patterns_, pattern, fd);
}

const std::set<int>* PubSub::channelSubscribers(const std::string& channel) const {
    auto it = channels_.find(channel);
    return it == channels_.end() ? nullptr : &it->second;
}

std::vector<std::string> PubSub::channelNames() const {
    std::vector<std::string> names;
    names.reserve(channels_.size());
    for (const auto& [name, subscribers] : channels_) names.push_back(name);
    return names;
}

std::size_t PubSub::channelSubscriberCount(const std::string& channel) const {
    const std::set<int>* subscribers = channelSubscribers(channel);
    return subscribers ? subscribers->size() : 0;
}

}  // namespace mnemos::server
