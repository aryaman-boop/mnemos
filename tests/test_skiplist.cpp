// Skiplist tests. Rank and span maintenance is the fragile part: spans are
// updated at insert *and* delete across every level, and a single off-by-one
// silently corrupts ZRANK and index-based ZRANGE without breaking iteration.
// These tests cross-check every rank against a sorted reference vector.
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "core/skiplist.h"
#include "test_harness.h"

using namespace mnemos::core;

namespace {

struct Entry {
    double      score;
    std::string member;
    bool operator<(const Entry& other) const {
        if (score != other.score) return score < other.score;
        return member < other.member;
    }
};

std::vector<std::string> walk(const SkipList& list) {
    std::vector<std::string> out;
    for (const SkipList::Node* n = list.head(); n; n = n->levels[0].forward) {
        out.push_back(n->member);
    }
    return out;
}

void testOrderingAndTieBreak() {
    SkipList list;
    list.insert(2.0, "b");
    list.insert(1.0, "a");
    list.insert(3.0, "c");
    // Equal scores are ordered by member bytes, not by insertion order.
    list.insert(2.0, "a2");
    list.insert(2.0, "z2");

    CHECK_EQ(walk(list), (std::vector<std::string>{"a", "a2", "b", "z2", "c"}));
    CHECK_EQ(list.size(), std::size_t{5});
    CHECK_EQ(list.head()->member, std::string("a"));
    CHECK_EQ(list.tail()->member, std::string("c"));
}

void testRankMatchesReference() {
    SkipList list;
    std::vector<Entry> reference;

    std::mt19937 gen{12345};
    std::uniform_int_distribution<int> score_dist(0, 50);
    for (int i = 0; i < 500; ++i) {
        // Deliberately few distinct scores, so ties are common and the
        // member-based tie-break is exercised hard.
        const double score = score_dist(gen);
        const std::string member = "m" + std::to_string(i);
        list.insert(score, member);
        reference.push_back({score, member});
    }
    std::sort(reference.begin(), reference.end());

    CHECK_EQ(list.size(), reference.size());

    // Every rank must agree with the sorted reference, in both directions.
    bool ranks_ok = true;
    bool nodes_ok = true;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const auto rank = list.rankOf(reference[i].score, reference[i].member);
        if (!rank.has_value() || *rank != i) ranks_ok = false;

        const SkipList::Node* node = list.nodeByRank(i);
        if (!node || node->member != reference[i].member) nodes_ok = false;
    }
    CHECK(ranks_ok);
    CHECK(nodes_ok);

    CHECK(list.nodeByRank(reference.size()) == nullptr);
    CHECK(!list.rankOf(999.0, "nope").has_value());
}

void testRanksSurviveDeletion() {
    SkipList list;
    std::vector<Entry> reference;
    for (int i = 0; i < 300; ++i) {
        const double score = i % 20;
        const std::string member = "m" + std::to_string(i);
        list.insert(score, member);
        reference.push_back({score, member});
    }
    std::sort(reference.begin(), reference.end());

    // Remove every third entry, then re-verify all remaining ranks.
    std::mt19937 gen{999};
    for (int i = 0; i < 300; i += 3) {
        const std::string member = "m" + std::to_string(i);
        const double score = i % 20;
        CHECK(list.remove(score, member));
        reference.erase(std::remove_if(reference.begin(), reference.end(),
                                       [&](const Entry& e) { return e.member == member; }),
                        reference.end());
    }

    CHECK_EQ(list.size(), reference.size());
    bool ranks_ok = true;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const auto rank = list.rankOf(reference[i].score, reference[i].member);
        if (!rank.has_value() || *rank != i) ranks_ok = false;
    }
    CHECK(ranks_ok);

    CHECK(!list.remove(0.0, "nonexistent"));
}

void testScoreRanges() {
    SkipList list;
    for (int i = 1; i <= 10; ++i) list.insert(i, "m" + std::to_string(i));

    // Inclusive bounds.
    ScoreRange inclusive{3.0, 7.0, false, false};
    CHECK_EQ(list.firstInRange(inclusive)->member, std::string("m3"));
    CHECK_EQ(list.lastInRange(inclusive)->member, std::string("m7"));
    CHECK_EQ(list.countInRange(inclusive), std::size_t{5});

    // Exclusive bounds -- ZRANGEBYSCORE's "(3 (7".
    ScoreRange exclusive{3.0, 7.0, true, true};
    CHECK_EQ(list.firstInRange(exclusive)->member, std::string("m4"));
    CHECK_EQ(list.lastInRange(exclusive)->member, std::string("m6"));
    CHECK_EQ(list.countInRange(exclusive), std::size_t{3});

    // Range entirely outside the data.
    ScoreRange above{100.0, 200.0, false, false};
    CHECK(list.firstInRange(above) == nullptr);
    ScoreRange below{-100.0, -50.0, false, false};
    CHECK(list.firstInRange(below) == nullptr);

    // Single-point range.
    ScoreRange point{5.0, 5.0, false, false};
    CHECK_EQ(list.countInRange(point), std::size_t{1});

    // An empty range must not be treated as unbounded.
    ScoreRange empty{7.0, 3.0, false, false};
    CHECK(empty.isEmpty());
    CHECK(list.firstInRange(empty) == nullptr);

    // Unbounded, as ZRANGEBYSCORE -inf +inf.
    ScoreRange all{-INFINITY, INFINITY, false, false};
    CHECK_EQ(list.countInRange(all), std::size_t{10});
}

void testUpdateScoreMovesNode() {
    SkipList list;
    for (int i = 1; i <= 5; ++i) list.insert(i, "m" + std::to_string(i));

    // Moving m1 from the front to the back must reorder it.
    list.updateScore(1.0, "m1", 99.0);
    CHECK_EQ(walk(list), (std::vector<std::string>{"m2", "m3", "m4", "m5", "m1"}));
    CHECK_EQ(list.size(), std::size_t{5});
    CHECK_EQ(list.rankOf(99.0, "m1").value_or(999), std::size_t{4});
    CHECK_EQ(list.tail()->member, std::string("m1"));

    CHECK(list.updateScore(0.0, "missing", 1.0) == nullptr);
}

void testBackwardTraversal() {
    SkipList list;
    for (int i = 1; i <= 20; ++i) list.insert(i, "m" + std::to_string(i));

    // Walking from the tail via backward pointers must mirror forward order --
    // this is how ZREVRANGE works without a second index.
    std::vector<std::string> reverse;
    for (const SkipList::Node* n = list.tail(); n; n = n->backward) reverse.push_back(n->member);

    std::vector<std::string> forward = walk(list);
    std::reverse(forward.begin(), forward.end());
    CHECK_EQ(reverse, forward);
}

void testClearAndReuse() {
    SkipList list;
    for (int i = 0; i < 100; ++i) list.insert(i, "m" + std::to_string(i));
    list.clear();
    CHECK_EQ(list.size(), std::size_t{0});
    CHECK(list.head() == nullptr);
    CHECK(list.tail() == nullptr);
    CHECK_EQ(list.level(), 1);

    list.insert(1.0, "again");
    CHECK_EQ(list.size(), std::size_t{1});
    CHECK_EQ(list.head()->member, std::string("again"));
}

}  // namespace

int main() {
    testOrderingAndTieBreak();
    testRankMatchesReference();
    testRanksSurviveDeletion();
    testScoreRanges();
    testUpdateScoreMovesNode();
    testBackwardTraversal();
    testClearAndReuse();
    return mnemos::test::summarise("skiplist");
}
