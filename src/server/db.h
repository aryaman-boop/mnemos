// One numbered keyspace (SELECT 0..15) plus its expiry table.
//
// Expiry is the part worth reading closely. Redis never scans the keyspace to
// find expired keys -- that would be O(N) and would stall the loop. Instead it
// combines two mechanisms:
//
//   * lazy   -- every lookup checks the expiry and deletes on the spot;
//   * active -- 10x/second, sample 20 random keys with a TTL, delete the dead
//               ones, and if more than 25% were dead, immediately go again.
//
// The active cycle is a probabilistic bound: it keeps the expected fraction of
// expired-but-still-resident keys under ~25% without ever touching a key it
// didn't sample.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dict.h"
#include "core/object.h"

namespace mnemos::server {

using core::Value;

// Redis's ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP / threshold.
inline constexpr int kActiveExpireSampleSize = 20;
inline constexpr int kActiveExpireThresholdPercent = 25;
inline constexpr int kActiveExpireMaxIterations = 16;

class Database {
public:
    explicit Database(int index) : index_(index) {}

    int index() const { return index_; }

    // Called whenever a key is deleted because its TTL passed, so the caller can
    // propagate an explicit DEL to replicas and the AOF. Redis does exactly this:
    // expiry must be driven by the master and replicated as a real deletion,
    // never re-derived independently on each replica.
    void setExpiredKeyCallback(std::function<void(const std::string&)> cb) {
        on_expired_ = std::move(cb);
    }

    // Called when a key that was not there before is added, whatever put it
    // there. Redis reports this as the `new` event, and it always precedes the
    // event for the command that did the creating.
    void setNewKeyCallback(std::function<void(const std::string&)> cb) {
        on_new_key_ = std::move(cb);
    }

    // When false (i.e. we are a replica of someone else), a logically-expired
    // key reads as missing but is *not* deleted -- we wait for the master's DEL.
    void setExpirationEnabled(bool enabled) { expiration_enabled_ = enabled; }

    Value* lookupRead(std::string_view key, std::int64_t now_ms);
    Value* lookupWrite(std::string_view key, std::int64_t now_ms);

    // Stores `value` under `key`, clearing any existing TTL -- the semantics of
    // a plain SET, which is why `SET k v` cancels a previously set EXPIRE.
    void setKey(std::string_view key, Value value);
    // Stores without touching the TTL, for in-place mutations like APPEND/INCR.
    void overwriteValue(std::string_view key, Value value);

    bool erase(std::string_view key);
    bool exists(std::string_view key, std::int64_t now_ms);

    std::size_t size() const { return dict_.size(); }
    std::size_t expiresSize() const { return expires_.size(); }
    std::size_t bucketCount() const { return dict_.bucketCount(); }
    bool isRehashing() const { return dict_.isRehashing(); }

    void setExpireAt(std::string_view key, std::int64_t at_ms);
    bool persist(std::string_view key);
    // Absolute expiry in ms, or -1 when the key has no TTL.
    std::int64_t expireAtMs(std::string_view key) const;
    // Remaining ms, -1 if no TTL, -2 if the key does not exist.
    std::int64_t ttlMs(std::string_view key, std::int64_t now_ms);

    void flush();

    // Returns how many keys it deleted. `now_ms` is the loop's cached clock.
    int activeExpireCycle(std::int64_t now_ms);

    const std::string* randomKey(std::int64_t now_ms);

    std::vector<std::string> keys() const;

    // SCAN over the keyspace. Returns the next cursor (0 when complete).
    std::uint64_t scan(std::uint64_t cursor, std::vector<std::string>& out) const;

    // Runs a slice of background rehashing when the server is otherwise idle.
    bool incrementalRehash(int buckets) { return dict_.rehashSteps(buckets); }

    core::Dict<Value>& raw() { return dict_; }

private:
    // Deletes the key if its TTL has passed. Returns true when the key is gone
    // (or should be treated as gone, on a replica).
    bool expireIfNeeded(std::string_view key, std::int64_t now_ms);

    int                                     index_;
    core::Dict<Value>                       dict_;
    core::Dict<std::int64_t>                expires_;
    std::function<void(const std::string&)> on_expired_;
    std::function<void(const std::string&)> on_new_key_;
    bool                                    expiration_enabled_ = true;
};

}  // namespace mnemos::server
