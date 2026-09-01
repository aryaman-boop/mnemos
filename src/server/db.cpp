#include "server/db.h"

namespace mnemos::server {

bool Database::expireIfNeeded(std::string_view key, std::int64_t now_ms) {
    const std::int64_t* when = expires_.find(key);
    if (!when) return false;
    if (*when > now_ms) return false;

    if (!expiration_enabled_) {
        // Replica path: report the key as logically gone, but leave the data in
        // place. Deleting here would let a replica diverge from its master if
        // their clocks disagree even slightly.
        return true;
    }

    const std::string owned(key);
    expires_.erase(owned);
    dict_.erase(owned);
    if (on_expired_) on_expired_(owned);
    return true;
}

Value* Database::lookupRead(std::string_view key, std::int64_t now_ms) {
    if (expireIfNeeded(key, now_ms)) return nullptr;
    Value* v = dict_.find(key);
    if (v) v->touch(now_ms);
    return v;
}

Value* Database::lookupWrite(std::string_view key, std::int64_t now_ms) {
    // Identical to the read path today, but kept separate because it is where
    // Redis hangs write-specific bookkeeping (dirty counters, WATCH touching).
    return lookupRead(key, now_ms);
}

void Database::setKey(std::string_view key, Value value) {
    const bool added = dict_.insert(key, std::move(value));
    expires_.erase(key);
    if (added && on_new_key_) on_new_key_(std::string(key));
}

void Database::overwriteValue(std::string_view key, Value value) {
    if (dict_.insert(key, std::move(value)) && on_new_key_) on_new_key_(std::string(key));
}

bool Database::erase(std::string_view key) {
    expires_.erase(key);
    return dict_.erase(key);
}

bool Database::exists(std::string_view key, std::int64_t now_ms) {
    return lookupRead(key, now_ms) != nullptr;
}

void Database::setExpireAt(std::string_view key, std::int64_t at_ms) {
    expires_.insert(key, at_ms);
}

bool Database::persist(std::string_view key) {
    return expires_.erase(key);
}

std::int64_t Database::expireAtMs(std::string_view key) const {
    const std::int64_t* when = expires_.find(key);
    return when ? *when : -1;
}

std::int64_t Database::ttlMs(std::string_view key, std::int64_t now_ms) {
    if (!lookupRead(key, now_ms)) return -2;
    const std::int64_t when = expireAtMs(key);
    if (when < 0) return -1;
    const std::int64_t remaining = when - now_ms;
    return remaining < 0 ? 0 : remaining;
}

void Database::flush() {
    dict_.clear();
    expires_.clear();
}

int Database::activeExpireCycle(std::int64_t now_ms) {
    if (!expiration_enabled_ || expires_.empty()) return 0;

    int total_deleted = 0;
    for (int iteration = 0; iteration < kActiveExpireMaxIterations; ++iteration) {
        const std::size_t remaining = expires_.size();
        if (remaining == 0) break;

        const int sample_target =
            static_cast<int>(std::min<std::size_t>(remaining, kActiveExpireSampleSize));

        int sampled = 0;
        int expired = 0;
        // Collect first, delete after: deleting while sampling would mutate the
        // table under randomEntry() and bias which keys we can still reach.
        std::vector<std::string> doomed;
        for (int i = 0; i < sample_target; ++i) {
            const auto* entry = expires_.randomEntry();
            if (!entry) break;
            ++sampled;
            if (entry->value <= now_ms) doomed.push_back(entry->key);
        }

        for (const std::string& key : doomed) {
            expires_.erase(key);
            dict_.erase(key);
            if (on_expired_) on_expired_(key);
            ++expired;
        }
        total_deleted += expired;

        if (sampled == 0) break;
        // The adaptive part: only keep going while the sample suggests there is
        // still a meaningful population of dead keys left to find.
        if (expired * 100 / sampled <= kActiveExpireThresholdPercent) break;
    }
    return total_deleted;
}

const std::string* Database::randomKey(std::int64_t now_ms) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto* entry = dict_.randomEntry();
        if (!entry) return nullptr;
        if (expireIfNeeded(entry->key, now_ms)) continue;  // landed on a dead key
        return &entry->key;
    }
    return nullptr;
}

std::vector<std::string> Database::keys() const {
    std::vector<std::string> out;
    out.reserve(dict_.size());
    dict_.forEach([&out](const std::string& k, const Value&) { out.push_back(k); });
    return out;
}

std::uint64_t Database::scan(std::uint64_t cursor, std::vector<std::string>& out) const {
    return dict_.scan(cursor, [&out](const std::string& k, const Value&) {
        out.push_back(k);
    });
}

}  // namespace mnemos::server
