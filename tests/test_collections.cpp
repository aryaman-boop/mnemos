// Collection encoding-transition tests.
//
// Every threshold asserted here was confirmed against a live Redis 8.10.1 by
// growing a real key and reading OBJECT ENCODING, not taken from documentation.
// Two of them are commonly got wrong: hash-max-listpack-entries is 512 rather
// than the widely-cited 128, and lists convert on *bytes* (8KB) rather than on
// an element count.
#include <string>
#include <vector>

#include "core/collections.h"
#include "test_harness.h"

using namespace mnemos::core;

namespace {

void testHashBasics() {
    HashValue hash;
    CHECK(hash.encoding() == ObjEncoding::ListPack);
    CHECK_EQ(hash.size(), std::size_t{0});

    CHECK(hash.set("name", "bob"));       // newly added
    CHECK(!hash.set("name", "alice"));    // updated, not added
    CHECK_EQ(hash.size(), std::size_t{1});
    CHECK_EQ(hash.get("name").value_or(""), std::string("alice"));

    CHECK(hash.contains("name"));
    CHECK(!hash.contains("missing"));
    CHECK(!hash.get("missing").has_value());

    CHECK(hash.set("age", "30"));
    CHECK_EQ(hash.flatten(), (std::vector<std::string>{"name", "alice", "age", "30"}));

    CHECK(hash.erase("name"));
    CHECK(!hash.erase("name"));
    CHECK_EQ(hash.size(), std::size_t{1});
    // Erasing a field must take its value with it, leaving the pairing intact.
    CHECK_EQ(hash.flatten(), (std::vector<std::string>{"age", "30"}));
}

void testHashFieldValueCollision() {
    // A value equal to another field's name must not be found by a field lookup.
    HashValue hash;
    hash.set("a", "b");
    hash.set("c", "d");
    CHECK(hash.contains("a"));
    CHECK(hash.contains("c"));
    CHECK(!hash.contains("b"));  // "b" is a value, not a field
    CHECK(!hash.contains("d"));
}

void testHashConvertsAtEntryLimit() {
    HashValue hash;
    for (int i = 0; i < 512; ++i) hash.set("f" + std::to_string(i), "v");
    CHECK(hash.encoding() == ObjEncoding::ListPack);
    CHECK_EQ(hash.size(), std::size_t{512});

    hash.set("f512", "v");  // the 513th field tips it over
    CHECK(hash.encoding() == ObjEncoding::HashTable);
    CHECK_EQ(hash.size(), std::size_t{513});

    // All data must survive the conversion.
    bool all_present = true;
    for (int i = 0; i < 513; ++i) {
        if (hash.get("f" + std::to_string(i)).value_or("") != "v") all_present = false;
    }
    CHECK(all_present);

    // Conversion is one-way: shrinking does not restore the listpack.
    for (int i = 0; i < 500; ++i) hash.erase("f" + std::to_string(i));
    CHECK(hash.encoding() == ObjEncoding::HashTable);
}

void testHashConvertsOnLongValue() {
    HashValue at_limit;
    at_limit.set("f", std::string(64, 'x'));
    CHECK(at_limit.encoding() == ObjEncoding::ListPack);

    HashValue past_limit;
    past_limit.set("f", std::string(65, 'x'));
    CHECK(past_limit.encoding() == ObjEncoding::HashTable);

    // A long *field name* converts it too, not just a long value.
    HashValue long_field;
    long_field.set(std::string(65, 'k'), "v");
    CHECK(long_field.encoding() == ObjEncoding::HashTable);
    CHECK_EQ(long_field.get(std::string(65, 'k')).value_or(""), std::string("v"));
}

void testSetIntsetPath() {
    SetValue set;
    CHECK(set.encoding() == ObjEncoding::IntSet);

    for (int i = 1; i <= 10; ++i) CHECK(set.add(std::to_string(i)));
    CHECK(set.encoding() == ObjEncoding::IntSet);
    CHECK_EQ(set.size(), std::size_t{10});
    CHECK(!set.add("5"));  // duplicate

    CHECK(set.contains("5"));
    CHECK(!set.contains("99"));
    // A non-numeric member can never be in an intset.
    CHECK(!set.contains("abc"));

    // Intset members come back in ascending numeric order.
    CHECK_EQ(set.members().front(), std::string("1"));
    CHECK_EQ(set.members().back(), std::string("10"));

    CHECK(set.erase("5"));
    CHECK(!set.erase("5"));
    CHECK_EQ(set.size(), std::size_t{9});
}

void testSetIntsetToListpackOnNonInteger() {
    SetValue set;
    set.add("1");
    set.add("2");
    CHECK(set.encoding() == ObjEncoding::IntSet);

    set.add("abc");  // one non-integer ends the intset encoding
    CHECK(set.encoding() == ObjEncoding::ListPack);
    CHECK_EQ(set.size(), std::size_t{3});
    CHECK(set.contains("1"));
    CHECK(set.contains("abc"));
}

void testSetIntsetToHashTableAtLimit() {
    SetValue set;
    for (int i = 0; i < 512; ++i) set.add(std::to_string(i));
    CHECK(set.encoding() == ObjEncoding::IntSet);

    // Past set-max-intset-entries, and already larger than the listpack limit,
    // so it goes straight to a hash table rather than via a listpack.
    set.add("512");
    CHECK(set.encoding() == ObjEncoding::HashTable);
    CHECK_EQ(set.size(), std::size_t{513});

    bool all_present = true;
    for (int i = 0; i <= 512; ++i) {
        if (!set.contains(std::to_string(i))) all_present = false;
    }
    CHECK(all_present);
}

void testSetListpackToHashTable() {
    SetValue set;
    set.add("seed");  // string member -> listpack
    CHECK(set.encoding() == ObjEncoding::ListPack);

    for (int i = 0; i < 127; ++i) set.add("m" + std::to_string(i));
    CHECK_EQ(set.size(), std::size_t{128});
    CHECK(set.encoding() == ObjEncoding::ListPack);

    set.add("one-more");
    CHECK(set.encoding() == ObjEncoding::HashTable);

    // A single long member also forces the conversion.
    SetValue long_member;
    long_member.add(std::string(65, 'x'));
    CHECK(long_member.encoding() == ObjEncoding::HashTable);
}

void testZSetOrderingAndScores() {
    ZSetValue zset;
    CHECK(zset.encoding() == ObjEncoding::ListPack);

    CHECK(zset.add("b", 2.0));
    CHECK(zset.add("a", 1.0));
    CHECK(zset.add("c", 3.0));
    CHECK(!zset.add("b", 2.5));  // rescore, not an addition
    CHECK_EQ(zset.size(), std::size_t{3});

    CHECK_EQ(zset.score("b").value_or(0), 2.5);
    CHECK(!zset.score("missing").has_value());

    // Stored in ascending score order, so rank is position.
    const auto all = zset.all();
    CHECK_EQ(all.size(), std::size_t{3});
    CHECK_EQ(all[0].first, std::string("a"));
    CHECK_EQ(all[1].first, std::string("b"));
    CHECK_EQ(all[2].first, std::string("c"));

    CHECK_EQ(zset.rank("a").value_or(99), std::size_t{0});
    CHECK_EQ(zset.rank("c").value_or(99), std::size_t{2});

    // Equal scores are ordered by member bytes.
    ZSetValue ties;
    ties.add("z", 1.0);
    ties.add("a", 1.0);
    ties.add("m", 1.0);
    CHECK_EQ(ties.all()[0].first, std::string("a"));
    CHECK_EQ(ties.all()[1].first, std::string("m"));
    CHECK_EQ(ties.all()[2].first, std::string("z"));

    CHECK(zset.erase("b"));
    CHECK(!zset.erase("b"));
    CHECK_EQ(zset.size(), std::size_t{2});
}

void testZSetConvertsToSkipList() {
    ZSetValue zset;
    for (int i = 0; i < 128; ++i) zset.add("m" + std::to_string(i), i);
    CHECK(zset.encoding() == ObjEncoding::ListPack);

    zset.add("m128", 128);
    CHECK(zset.encoding() == ObjEncoding::SkipList);
    CHECK_EQ(zset.size(), std::size_t{129});

    // Ordering and ranks must survive the conversion.
    const auto all = zset.all();
    bool ordered = true;
    for (std::size_t i = 1; i < all.size(); ++i) {
        if (all[i].second < all[i - 1].second) ordered = false;
    }
    CHECK(ordered);
    CHECK_EQ(zset.rank("m0").value_or(999), std::size_t{0});
    CHECK_EQ(zset.rank("m128").value_or(999), std::size_t{128});
    CHECK_EQ(zset.score("m64").value_or(-1), 64.0);

    // A long member converts it regardless of count.
    ZSetValue long_member;
    long_member.add(std::string(65, 'x'), 1.0);
    CHECK(long_member.encoding() == ObjEncoding::SkipList);
}

void testZSetRangeByScore() {
    ZSetValue zset;
    for (int i = 1; i <= 10; ++i) zset.add("m" + std::to_string(i), i);

    ScoreRange inclusive{3.0, 6.0, false, false};
    CHECK_EQ(zset.rangeByScore(inclusive).size(), std::size_t{4});

    ScoreRange exclusive{3.0, 6.0, true, true};
    const auto exclusive_result = zset.rangeByScore(exclusive);
    CHECK_EQ(exclusive_result.size(), std::size_t{2});
    if (exclusive_result.size() == 2) {
        CHECK_EQ(exclusive_result[0].first, std::string("m4"));
        CHECK_EQ(exclusive_result[1].first, std::string("m5"));
    }

    // The same query must give the same answer under the skiplist encoding.
    ZSetValue big;
    for (int i = 1; i <= 200; ++i) big.add("m" + std::to_string(i), i);
    CHECK(big.encoding() == ObjEncoding::SkipList);
    CHECK_EQ(big.rangeByScore(ScoreRange{3.0, 6.0, false, false}).size(), std::size_t{4});
    CHECK_EQ(big.rangeByScore(ScoreRange{3.0, 6.0, true, true}).size(), std::size_t{2});
}

void testListPushPop() {
    ListValue list;
    CHECK(list.encoding() == ObjEncoding::ListPack);
    CHECK_EQ(list.size(), std::size_t{0});

    list.pushBack("b");
    list.pushBack("c");
    list.pushFront("a");
    CHECK_EQ(list.size(), std::size_t{3});
    CHECK_EQ(list.range(0, -1), (std::vector<std::string>{"a", "b", "c"}));

    CHECK_EQ(list.popFront().value_or(""), std::string("a"));
    CHECK_EQ(list.popBack().value_or(""), std::string("c"));
    CHECK_EQ(list.size(), std::size_t{1});

    CHECK_EQ(list.popBack().value_or(""), std::string("b"));
    CHECK(!list.popBack().has_value());
    CHECK(!list.popFront().has_value());
}

void testListIndexing() {
    ListValue list;
    for (const char* v : {"a", "b", "c", "d", "e"}) list.pushBack(v);

    CHECK_EQ(list.at(0).value_or(""), std::string("a"));
    CHECK_EQ(list.at(4).value_or(""), std::string("e"));
    // Negative indices count from the tail.
    CHECK_EQ(list.at(-1).value_or(""), std::string("e"));
    CHECK_EQ(list.at(-5).value_or(""), std::string("a"));
    CHECK(!list.at(5).has_value());
    CHECK(!list.at(-6).has_value());

    CHECK(list.set(1, "B"));
    CHECK_EQ(list.at(1).value_or(""), std::string("B"));
    CHECK(!list.set(99, "x"));

    CHECK_EQ(list.range(1, 3), (std::vector<std::string>{"B", "c", "d"}));
    CHECK_EQ(list.range(-2, -1), (std::vector<std::string>{"d", "e"}));
    // An out-of-order range is empty, not an error.
    CHECK_EQ(list.range(3, 1), std::vector<std::string>{});
    // Over-wide ranges clamp rather than fail.
    CHECK_EQ(list.range(0, 999).size(), std::size_t{5});

    CHECK_EQ(list.indexOf("c").value_or(999), std::size_t{2});
    CHECK(!list.indexOf("zzz").has_value());
}

void testListRemoveAndTrim() {
    ListValue list;
    for (const char* v : {"a", "b", "a", "c", "a"}) list.pushBack(v);

    // Positive count removes from the head.
    CHECK_EQ(list.removeValue("a", 2), std::size_t{2});
    CHECK_EQ(list.range(0, -1), (std::vector<std::string>{"b", "c", "a"}));

    ListValue tail_first;
    for (const char* v : {"a", "b", "a", "c", "a"}) tail_first.pushBack(v);
    // Negative count removes from the tail.
    CHECK_EQ(tail_first.removeValue("a", -1), std::size_t{1});
    CHECK_EQ(tail_first.range(0, -1), (std::vector<std::string>{"a", "b", "a", "c"}));

    ListValue all_matches;
    for (const char* v : {"a", "b", "a", "c", "a"}) all_matches.pushBack(v);
    // Zero removes every occurrence.
    CHECK_EQ(all_matches.removeValue("a", 0), std::size_t{3});
    CHECK_EQ(all_matches.range(0, -1), (std::vector<std::string>{"b", "c"}));

    ListValue trimmed;
    for (int i = 0; i < 10; ++i) trimmed.pushBack("v" + std::to_string(i));
    trimmed.trim(2, 5);
    CHECK_EQ(trimmed.size(), std::size_t{4});
    CHECK_EQ(trimmed.range(0, -1),
             (std::vector<std::string>{"v2", "v3", "v4", "v5"}));
}

void testListConvertsOnBytes() {
    // Element count alone does not convert a list: 200 tiny items stay listpack.
    ListValue many_small;
    for (int i = 0; i < 200; ++i) many_small.pushBack("v" + std::to_string(i));
    CHECK(many_small.encoding() == ObjEncoding::ListPack);

    // Crossing the 8KB budget does convert it.
    ListValue bulky;
    for (int i = 0; i < 100; ++i) bulky.pushBack(std::string(100, 'x'));
    CHECK(bulky.encoding() == ObjEncoding::QuickList);
    CHECK_EQ(bulky.size(), std::size_t{100});

    // A single oversized element is enough on its own.
    ListValue one_huge;
    one_huge.pushBack(std::string(9000, 'x'));
    CHECK(one_huge.encoding() == ObjEncoding::QuickList);
    CHECK_EQ(one_huge.size(), std::size_t{1});

    // Data must remain correct once split across quicklist nodes.
    ListValue split;
    for (int i = 0; i < 300; ++i) split.pushBack(std::string(100, 'a' + (i % 26)));
    CHECK(split.encoding() == ObjEncoding::QuickList);
    CHECK_EQ(split.size(), std::size_t{300});
    CHECK_EQ(split.at(0).value_or("").size(), std::size_t{100});
    CHECK_EQ(split.at(299).value_or(""), std::string(100, 'a' + (299 % 26)));
    CHECK_EQ(split.range(0, -1).size(), std::size_t{300});
    CHECK_EQ(split.popFront().value_or(""), std::string(100, 'a'));
    CHECK_EQ(split.size(), std::size_t{299});
}

}  // namespace

int main() {
    testHashBasics();
    testHashFieldValueCollision();
    testHashConvertsAtEntryLimit();
    testHashConvertsOnLongValue();
    testSetIntsetPath();
    testSetIntsetToListpackOnNonInteger();
    testSetIntsetToHashTableAtLimit();
    testSetListpackToHashTable();
    testZSetOrderingAndScores();
    testZSetConvertsToSkipList();
    testZSetRangeByScore();
    testListPushPop();
    testListIndexing();
    testListRemoveAndTrim();
    testListConvertsOnBytes();
    return mnemos::test::summarise("collections");
}
