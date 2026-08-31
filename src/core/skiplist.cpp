#include "core/skiplist.h"

#include <random>

namespace mnemos::core {
namespace {

std::uint32_t randomBits() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    return gen();
}

}  // namespace

SkipList::SkipList() {
    header_ = new Node();
    header_->levels.resize(kSkipListMaxLevel);
}

SkipList::~SkipList() {
    Node* node = header_;
    while (node) {
        Node* next = node->levels[0].forward;
        delete node;
        node = next;
    }
}

void SkipList::clear() {
    Node* node = header_->levels[0].forward;
    while (node) {
        Node* next = node->levels[0].forward;
        delete node;
        node = next;
    }
    header_->levels.assign(kSkipListMaxLevel, Node::Level{});
    tail_   = nullptr;
    length_ = 0;
    level_  = 1;
}

int SkipList::randomLevel() {
    // Each additional level is taken with probability p = 1/4, giving an
    // expected 1.33 pointers per node -- the trade between height and memory.
    int level = 1;
    while ((randomBits() & 0xFFFF) < static_cast<std::uint32_t>(kSkipListP * 0xFFFF) &&
           level < kSkipListMaxLevel) {
        ++level;
    }
    return level;
}

SkipList::Node* SkipList::insert(double score, std::string member) {
    Node* update[kSkipListMaxLevel];
    std::uint64_t rank[kSkipListMaxLevel];

    Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        // rank[i] accumulates how far we have travelled at this level, so the
        // spans of the new node's pointers can be computed exactly.
        rank[i] = (i == level_ - 1) ? 0 : rank[i + 1];
        while (node->levels[i].forward &&
               isBefore(node->levels[i].forward->score, node->levels[i].forward->member,
                        score, member)) {
            rank[i] += node->levels[i].span;
            node = node->levels[i].forward;
        }
        update[i] = node;
    }

    const int new_level = randomLevel();
    if (new_level > level_) {
        for (int i = level_; i < new_level; ++i) {
            rank[i]   = 0;
            update[i] = header_;
            // The header's span at a brand-new level covers the whole list.
            update[i]->levels[i].span = length_;
        }
        level_ = new_level;
    }

    Node* inserted = new Node();
    inserted->member = std::move(member);
    inserted->score  = score;
    inserted->levels.resize(static_cast<std::size_t>(new_level));

    for (int i = 0; i < new_level; ++i) {
        inserted->levels[i].forward = update[i]->levels[i].forward;
        update[i]->levels[i].forward = inserted;
        // Split the old span at the insertion point.
        inserted->levels[i].span = update[i]->levels[i].span - (rank[0] - rank[i]);
        update[i]->levels[i].span = (rank[0] - rank[i]) + 1;
    }
    // Levels above the new node still cross it, so their spans grow by one.
    for (int i = new_level; i < level_; ++i) {
        ++update[i]->levels[i].span;
    }

    inserted->backward = (update[0] == header_) ? nullptr : update[0];
    if (inserted->levels[0].forward) {
        inserted->levels[0].forward->backward = inserted;
    } else {
        tail_ = inserted;
    }
    ++length_;
    return inserted;
}

bool SkipList::remove(double score, std::string_view member) {
    Node* update[kSkipListMaxLevel];

    Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels[i].forward &&
               isBefore(node->levels[i].forward->score, node->levels[i].forward->member,
                        score, member)) {
            node = node->levels[i].forward;
        }
        update[i] = node;
    }

    Node* target = node->levels[0].forward;
    if (!target || target->score != score || target->member != member) return false;

    for (int i = 0; i < level_; ++i) {
        if (update[i]->levels[i].forward == target) {
            // Absorb the removed node's span into the pointer that skipped to it.
            update[i]->levels[i].span += target->levels[i].span - 1;
            update[i]->levels[i].forward = target->levels[i].forward;
        } else {
            --update[i]->levels[i].span;
        }
    }

    if (target->levels[0].forward) {
        target->levels[0].forward->backward = target->backward;
    } else {
        tail_ = target->backward;
    }
    // Drop any levels that are now entirely empty.
    while (level_ > 1 && header_->levels[level_ - 1].forward == nullptr) {
        --level_;
    }

    delete target;
    --length_;
    return true;
}

SkipList::Node* SkipList::updateScore(double old_score, std::string_view member,
                                      double new_score) {
    std::string owned(member);
    if (!remove(old_score, member)) return nullptr;
    return insert(new_score, std::move(owned));
}

std::optional<std::size_t> SkipList::rankOf(double score, std::string_view member) const {
    std::uint64_t rank = 0;
    const Node* node = header_;

    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels[i].forward &&
               !isBefore(score, member, node->levels[i].forward->score,
                         node->levels[i].forward->member)) {
            rank += node->levels[i].span;
            node = node->levels[i].forward;
            // Stop as soon as we land exactly on the target.
            if (node != header_ && node->score == score && node->member == member) {
                return static_cast<std::size_t>(rank - 1);
            }
        }
    }
    return std::nullopt;
}

SkipList::Node* SkipList::nodeByRank(std::size_t rank) const {
    if (rank >= length_) return nullptr;
    const std::uint64_t target = rank + 1;  // header counts as rank 0
    std::uint64_t travelled = 0;
    Node* node = header_;

    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels[i].forward && travelled + node->levels[i].span <= target) {
            travelled += node->levels[i].span;
            node = node->levels[i].forward;
        }
        if (travelled == target && node != header_) return node;
    }
    return nullptr;
}

SkipList::Node* SkipList::firstInRange(const ScoreRange& range) const {
    if (range.isEmpty() || length_ == 0) return nullptr;
    if (tail_ == nullptr) return nullptr;
    // Quick rejection: the whole list sits below the range.
    if (range.min_exclusive ? tail_->score <= range.min : tail_->score < range.min) {
        return nullptr;
    }

    Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        // Advance while the *next* node is still below the range.
        while (node->levels[i].forward &&
               !(range.min_exclusive ? node->levels[i].forward->score > range.min
                                     : node->levels[i].forward->score >= range.min)) {
            node = node->levels[i].forward;
        }
    }
    node = node->levels[0].forward;
    if (!node) return nullptr;
    if (range.max_exclusive ? node->score >= range.max : node->score > range.max) {
        return nullptr;
    }
    return node;
}

SkipList::Node* SkipList::lastInRange(const ScoreRange& range) const {
    if (range.isEmpty() || length_ == 0) return nullptr;
    Node* first = header_->levels[0].forward;
    if (!first) return nullptr;
    if (range.max_exclusive ? first->score >= range.max : first->score > range.max) {
        return nullptr;
    }

    Node* node = header_;
    for (int i = level_ - 1; i >= 0; --i) {
        while (node->levels[i].forward &&
               (range.max_exclusive ? node->levels[i].forward->score < range.max
                                    : node->levels[i].forward->score <= range.max)) {
            node = node->levels[i].forward;
        }
    }
    if (node == header_) return nullptr;
    if (range.min_exclusive ? node->score <= range.min : node->score < range.min) {
        return nullptr;
    }
    return node;
}

std::size_t SkipList::countInRange(const ScoreRange& range) const {
    Node* first = firstInRange(range);
    if (!first) return 0;
    Node* last = lastInRange(range);
    if (!last) return 0;

    const auto first_rank = rankOf(first->score, first->member);
    const auto last_rank  = rankOf(last->score, last->member);
    if (!first_rank || !last_rank) return 0;
    return *last_rank - *first_rank + 1;
}

}  // namespace mnemos::core
