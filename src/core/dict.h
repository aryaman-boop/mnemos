// An open-hashed table with incremental rehashing -- our dict.c.
//
// We do not use std::unordered_map for the keyspace, for two reasons that are
// both central to how Redis behaves:
//
//   1. Rehashing must be *incremental*. Growing a 10M-key table in one step
//      would stall the single-threaded event loop for seconds. Instead we keep
//      two tables live and migrate one bucket at a time on each operation, so
//      the cost is amortised and no single command pays for the whole resize.
//
//   2. SCAN's guarantees depend on the bucket layout. The reverse-binary cursor
//      below is what lets SCAN promise that every element present for the whole
//      iteration is returned at least once, *even if the table is resized
//      mid-iteration*. That is not expressible over an opaque std::unordered_map.
#pragma once

#include <cstdint>
#include <functional>
#include <random>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace mnemos::core {

// Reverses the bits of a 64-bit word. The SCAN cursor is incremented in
// reverse-binary order so that when the table size changes, the buckets a
// cursor still has to visit remain a superset of the ones it would have
// visited at the old size -- which is precisely the "no element missed" promise.
inline std::uint64_t reverseBits(std::uint64_t v) {
    v = ((v >> 1)  & 0x5555555555555555ULL) | ((v & 0x5555555555555555ULL) << 1);
    v = ((v >> 2)  & 0x3333333333333333ULL) | ((v & 0x3333333333333333ULL) << 2);
    v = ((v >> 4)  & 0x0F0F0F0F0F0F0F0FULL) | ((v & 0x0F0F0F0F0F0F0F0FULL) << 4);
    v = ((v >> 8)  & 0x00FF00FF00FF00FFULL) | ((v & 0x00FF00FF00FF00FFULL) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFULL) | ((v & 0x0000FFFF0000FFFFULL) << 16);
    return (v >> 32) | (v << 32);
}

template <typename T>
class Dict {
public:
    struct Entry {
        std::string key;
        T           value;
        Entry*      next = nullptr;
    };

    Dict() { tables_[0].resize(kInitialSize); }

    ~Dict() { clear(); }

    // Copying would have to duplicate every chained entry; callers that need a
    // copy rebuild from the logical contents instead. Moving is cheap and is
    // required because the declared destructor suppresses the implicit move.
    Dict(const Dict&)            = delete;
    Dict& operator=(const Dict&) = delete;

    Dict(Dict&& other) noexcept { swap(other); }
    Dict& operator=(Dict&& other) noexcept {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }

    void swap(Dict& other) noexcept {
        std::swap(tables_[0], other.tables_[0]);
        std::swap(tables_[1], other.tables_[1]);
        std::swap(used_[0], other.used_[0]);
        std::swap(used_[1], other.used_[1]);
        std::swap(rehash_index_, other.rehash_index_);
    }

    std::size_t size() const { return used_[0] + used_[1]; }
    bool empty() const { return size() == 0; }
    bool isRehashing() const { return rehash_index_ >= 0; }

    // Total bucket slots across both tables -- what INFO reports.
    std::size_t bucketCount() const { return tables_[0].size() + tables_[1].size(); }

    T* find(std::string_view key) {
        if (empty()) return nullptr;
        rehashStep();
        const std::uint64_t h = hash(key);
        for (int t = 0; t <= (isRehashing() ? 1 : 0); ++t) {
            if (tables_[t].empty()) continue;
            Entry* e = tables_[t][h & mask(t)];
            while (e) {
                if (e->key == key) return &e->value;
                e = e->next;
            }
            // While rehashing, everything below rehash_index_ has already moved
            // out of table 0, so there is no point looking there for those.
            if (!isRehashing()) break;
        }
        return nullptr;
    }

    const T* find(std::string_view key) const {
        return const_cast<Dict*>(this)->find(key);
    }

    // Inserts or overwrites. Returns true when a new key was added.
    bool insert(std::string_view key, T value) {
        rehashStep();
        if (T* existing = find(key)) {
            *existing = std::move(value);
            return false;
        }
        expandIfNeeded();

        // New keys always go into table 1 during a rehash, so table 0 only ever
        // shrinks and the migration is guaranteed to terminate.
        const int t = isRehashing() ? 1 : 0;
        const std::uint64_t h = hash(key);
        Entry* e = new Entry{std::string(key), std::move(value), tables_[t][h & mask(t)]};
        tables_[t][h & mask(t)] = e;
        ++used_[t];
        return true;
    }

    bool erase(std::string_view key) {
        if (empty()) return false;
        rehashStep();
        const std::uint64_t h = hash(key);
        for (int t = 0; t <= (isRehashing() ? 1 : 0); ++t) {
            if (tables_[t].empty()) continue;
            const std::size_t idx = h & mask(t);
            Entry* prev = nullptr;
            Entry* e    = tables_[t][idx];
            while (e) {
                if (e->key == key) {
                    if (prev) prev->next = e->next;
                    else      tables_[t][idx] = e->next;
                    delete e;
                    --used_[t];
                    shrinkIfNeeded();
                    return true;
                }
                prev = e;
                e    = e->next;
            }
            if (!isRehashing()) break;
        }
        return false;
    }

    void clear() {
        for (int t = 0; t < 2; ++t) {
            for (Entry*& head : tables_[t]) {
                while (head) {
                    Entry* next = head->next;
                    delete head;
                    head = next;
                }
            }
            tables_[t].clear();
            used_[t] = 0;
        }
        rehash_index_ = -1;
        tables_[0].resize(kInitialSize);
    }

    // Visits every entry. Used by KEYS, DEBUG, and persistence -- all of which
    // accept an O(N) scan because they are explicitly not hot-path operations.
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (int t = 0; t < 2; ++t) {
            for (Entry* head : tables_[t]) {
                for (Entry* e = head; e; e = e->next) fn(e->key, e->value);
            }
        }
    }

    // One step of the SCAN cursor. Emits every entry in one bucket (and its
    // rehash counterpart) and returns the next cursor; 0 means iteration is done.
    template <typename Fn>
    std::uint64_t scan(std::uint64_t cursor, Fn&& fn) const {
        if (empty()) return 0;

        if (!isRehashing()) {
            const std::uint64_t m0 = mask(0);
            emitBucket(0, cursor & m0, fn);
            // Set every bit above the mask, reverse, increment, reverse back:
            // this walks the significant bits from the *top* down.
            cursor |= ~m0;
            cursor = reverseBits(cursor);
            ++cursor;
            cursor = reverseBits(cursor);
            return cursor;
        }

        // Mid-rehash: visit the smaller table's bucket, then every bucket in the
        // larger table that maps onto it, so nothing is skipped either way.
        const int small = tables_[0].size() <= tables_[1].size() ? 0 : 1;
        const int large = 1 - small;
        const std::uint64_t ms = mask(small);
        const std::uint64_t ml = mask(large);

        emitBucket(small, cursor & ms, fn);
        do {
            emitBucket(large, cursor & ml, fn);
            cursor |= ~ml;
            cursor = reverseBits(cursor);
            ++cursor;
            cursor = reverseBits(cursor);
        } while (cursor & (ms ^ ml));
        return cursor;
    }

    // Uniform-ish random entry, used by RANDOMKEY and the active expire cycle.
    // Picks a random non-empty bucket, then a random link in its chain -- so it
    // is biased toward keys in short chains, exactly as Redis's dictGetRandomKey is.
    const Entry* randomEntry() const {
        if (empty()) return nullptr;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int t = (isRehashing() && (rng() & 1)) ? 1 : 0;
            if (tables_[t].empty() || used_[t] == 0) continue;
            const std::size_t idx = rng() % tables_[t].size();
            Entry* head = tables_[t][idx];
            if (!head) continue;
            std::size_t chain_len = 0;
            for (Entry* e = head; e; e = e->next) ++chain_len;
            std::size_t step = rng() % chain_len;
            Entry* e = head;
            while (step-- > 0) e = e->next;
            return e;
        }
        // Degenerate fallback: linear scan for the first entry we can find.
        for (int t = 0; t < 2; ++t) {
            for (Entry* head : tables_[t]) if (head) return head;
        }
        return nullptr;
    }

    // Performs up to `buckets` migration steps; returns true while more remain.
    bool rehashSteps(int buckets) {
        if (!isRehashing()) return false;
        int empty_visits = buckets * 10;  // bound work on a sparse table
        while (buckets-- > 0 && used_[0] > 0) {
            while (tables_[0][static_cast<std::size_t>(rehash_index_)] == nullptr) {
                ++rehash_index_;
                if (--empty_visits == 0) return true;
                if (static_cast<std::size_t>(rehash_index_) >= tables_[0].size()) break;
            }
            if (static_cast<std::size_t>(rehash_index_) >= tables_[0].size()) break;

            Entry* e = tables_[0][static_cast<std::size_t>(rehash_index_)];
            while (e) {
                Entry* next = e->next;
                const std::size_t idx = hash(e->key) & mask(1);
                e->next          = tables_[1][idx];
                tables_[1][idx]  = e;
                --used_[0];
                ++used_[1];
                e = next;
            }
            tables_[0][static_cast<std::size_t>(rehash_index_)] = nullptr;
            ++rehash_index_;
        }

        if (used_[0] == 0) {
            tables_[0] = std::move(tables_[1]);
            used_[0]   = used_[1];
            tables_[1].clear();
            used_[1]      = 0;
            rehash_index_ = -1;
            return false;
        }
        return true;
    }

    static std::uint64_t hash(std::string_view key) {
        // FNV-1a. Redis uses SipHash for DoS resistance on untrusted keys; FNV
        // is faster and adequate here, and swapping it is a one-function change.
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : key) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

private:
    static constexpr std::size_t kInitialSize = 4;

    std::size_t mask(int t) const { return tables_[t].size() - 1; }

    template <typename Fn>
    void emitBucket(int t, std::size_t idx, Fn& fn) const {
        if (idx >= tables_[t].size()) return;
        for (Entry* e = tables_[t][idx]; e; e = e->next) fn(e->key, e->value);
    }

    void rehashStep() {
        if (isRehashing()) rehashSteps(1);
    }

    void expandIfNeeded() {
        if (isRehashing()) return;
        // Load factor 1: Redis grows once used == size, keeping chains short.
        if (used_[0] < tables_[0].size()) return;
        resizeTo(tables_[0].size() * 2);
    }

    void shrinkIfNeeded() {
        if (isRehashing()) return;
        if (tables_[0].size() <= kInitialSize) return;
        // Below 10% occupancy the table is mostly empty buckets; reclaim them.
        if (used_[0] * 10 > tables_[0].size()) return;
        resizeTo(std::max(kInitialSize, nextPowerOfTwo(used_[0] + 1)));
    }

    void resizeTo(std::size_t new_size) {
        tables_[1].assign(new_size, nullptr);
        used_[1]      = 0;
        rehash_index_ = 0;
    }

    static std::size_t nextPowerOfTwo(std::size_t n) {
        std::size_t p = kInitialSize;
        while (p < n) p *= 2;
        return p;
    }

    static std::uint64_t rng() {
        static thread_local std::mt19937_64 gen{std::random_device{}()};
        return gen();
    }

    std::vector<Entry*> tables_[2];
    std::size_t         used_[2]      = {0, 0};
    std::int64_t        rehash_index_ = -1;
};

}  // namespace mnemos::core
