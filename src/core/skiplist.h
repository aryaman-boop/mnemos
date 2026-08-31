// A skiplist, as used for the sorted-set `skiplist` encoding.
//
// Two details are worth knowing beyond "it's a probabilistic balanced list":
//
//  * Ordering is by (score, member). Ties on score are broken by comparing the
//    member bytes, which is what makes ZRANGEBYLEX meaningful and what gives
//    equal-scored members a stable, defined order.
//
//  * Every forward pointer carries a `span`: how many nodes it skips over. That
//    is what turns ZRANK and index-based ZRANGE into O(log N) operations rather
//    than a linear walk. Plain textbook skiplists omit spans and cannot do this.
//
// A sorted set pairs this with a dict from member to score: the dict answers
// ZSCORE in O(1), the skiplist answers range and rank queries. Neither structure
// alone does both, which is why Redis keeps them together.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mnemos::core {

// Redis's ZSKIPLIST_MAXLEVEL / ZSKIPLIST_P.
inline constexpr int kSkipListMaxLevel = 32;
inline constexpr double kSkipListP = 0.25;

// A score bound for range queries. `exclusive` distinguishes ZRANGEBYSCORE's
// "5" from "(5".
struct ScoreRange {
    double min = 0.0;
    double max = 0.0;
    bool   min_exclusive = false;
    bool   max_exclusive = false;

    bool isEmpty() const {
        if (min > max) return true;
        if (min == max && (min_exclusive || max_exclusive)) return true;
        return false;
    }
    bool containsScore(double score) const {
        if (min_exclusive ? score <= min : score < min) return false;
        if (max_exclusive ? score >= max : score > max) return false;
        return true;
    }
};

class SkipList {
public:
    struct Node {
        std::string  member;
        double       score = 0.0;
        Node*        backward = nullptr;

        struct Level {
            Node*         forward = nullptr;
            std::uint64_t span    = 0;  // nodes skipped by `forward`
        };
        std::vector<Level> levels;
    };

    SkipList();
    ~SkipList();

    // As with Dict: not copyable (a copy must be rebuilt from the members),
    // but movable, which the declared destructor would otherwise suppress.
    SkipList(const SkipList&)            = delete;
    SkipList& operator=(const SkipList&) = delete;

    SkipList(SkipList&& other) noexcept { swap(other); }
    SkipList& operator=(SkipList&& other) noexcept {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }

    void swap(SkipList& other) noexcept {
        std::swap(header_, other.header_);
        std::swap(tail_, other.tail_);
        std::swap(length_, other.length_);
        std::swap(level_, other.level_);
    }

    std::size_t size() const { return length_; }
    int level() const { return level_; }

    Node* head() const { return header_->levels[0].forward; }
    Node* tail() const { return tail_; }

    // Inserts a member that is not already present.
    Node* insert(double score, std::string member);
    bool  remove(double score, std::string_view member);
    // Repositions an existing member. Cheaper than remove+insert only in intent;
    // it exists so callers can't forget that a score change moves the node.
    Node* updateScore(double old_score, std::string_view member, double new_score);

    // 0-based rank in ascending score order, or nullopt if absent.
    std::optional<std::size_t> rankOf(double score, std::string_view member) const;
    // 0-based; nullptr when out of range.
    Node* nodeByRank(std::size_t rank) const;

    Node* firstInRange(const ScoreRange& range) const;
    Node* lastInRange(const ScoreRange& range) const;
    std::size_t countInRange(const ScoreRange& range) const;

    void clear();

private:
    static int randomLevel();
    // Ordering predicate: is `a` strictly before `b`?
    static bool isBefore(double a_score, std::string_view a_member,
                         double b_score, std::string_view b_member) {
        if (a_score != b_score) return a_score < b_score;
        return a_member < b_member;
    }

    Node*        header_ = nullptr;
    Node*        tail_   = nullptr;
    std::size_t  length_ = 0;
    int          level_  = 1;
};

}  // namespace mnemos::core
