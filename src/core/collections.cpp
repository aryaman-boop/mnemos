#include "core/collections.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "net/resp.h"

namespace mnemos::core {

const EncodingLimits& defaultLimits() {
    static const EncodingLimits limits;
    return limits;
}

namespace {

// Scores are stored in listpacks as text. Reuse the reply formatter so the
// round trip is exact and matches what ZSCORE would print.
std::string scoreToString(double score) { return net::formatDouble(score); }

double scoreFromString(std::string_view s) {
    return std::strtod(std::string(s).c_str(), nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// HashValue
// ---------------------------------------------------------------------------

std::size_t HashValue::size() const {
    return encoding_ == ObjEncoding::ListPack ? listpack_.numElements() / 2 : table_.size();
}

// Returns by value rather than by pointer: under the listpack encoding the
// value has to be materialised from the packed bytes, so there is no stable
// address to hand back, and a shared scratch buffer would make two consecutive
// get() calls alias each other.
std::optional<std::string> HashValue::get(std::string_view field) const {
    if (encoding_ == ObjEncoding::HashTable) {
        const std::string* found = table_.find(field);
        return found ? std::optional<std::string>(*found) : std::nullopt;
    }

    // Fields sit at even indices, so search with step 2 to avoid matching a
    // value that happens to equal the field name we are looking for.
    const auto index = listpack_.find(field, 0, 2);
    if (!index) return std::nullopt;
    Listpack::Element element;
    if (!listpack_.get(*index + 1, element)) return std::nullopt;
    return element.toString();
}

bool HashValue::contains(std::string_view field) const {
    if (encoding_ == ObjEncoding::HashTable) return table_.find(field) != nullptr;
    return listpack_.find(field, 0, 2).has_value();
}

bool HashValue::shouldConvert(std::string_view field, std::string_view value) const {
    const EncodingLimits& limits = defaultLimits();
    if (size() >= limits.hash_max_listpack_entries) return true;
    // A single oversized field or value converts the whole hash: past that size
    // the linear scan stops being cheaper than hashing.
    if (field.size() > limits.hash_max_listpack_value) return true;
    if (value.size() > limits.hash_max_listpack_value) return true;
    return false;
}

void HashValue::convertToHashTable() {
    if (encoding_ == ObjEncoding::HashTable) return;

    std::vector<std::string> flat = listpack_.toStrings();
    for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
        table_.insert(flat[i], flat[i + 1]);
    }
    listpack_.clear();
    encoding_ = ObjEncoding::HashTable;
}

bool HashValue::set(std::string_view field, std::string_view value) {
    if (encoding_ == ObjEncoding::HashTable) {
        const bool is_new = table_.find(field) == nullptr;
        table_.insert(field, std::string(value));
        return is_new;
    }

    const auto index = listpack_.find(field, 0, 2);
    if (index) {
        listpack_.replaceAt(*index + 1, value);
        return false;
    }

    if (shouldConvert(field, value)) {
        convertToHashTable();
        table_.insert(field, std::string(value));
        return true;
    }

    listpack_.append(field);
    listpack_.append(value);
    return true;
}

bool HashValue::erase(std::string_view field) {
    if (encoding_ == ObjEncoding::HashTable) return table_.erase(field);

    const auto index = listpack_.find(field, 0, 2);
    if (!index) return false;
    // Field and value must go together, or the flat pairing is corrupted.
    return listpack_.eraseRange(*index, 2);
}

std::vector<std::string> HashValue::flatten() const {
    if (encoding_ == ObjEncoding::ListPack) return listpack_.toStrings();

    std::vector<std::string> out;
    out.reserve(table_.size() * 2);
    table_.forEach([&out](const std::string& k, const std::string& v) {
        out.push_back(k);
        out.push_back(v);
    });
    return out;
}

std::vector<std::string> HashValue::fields() const {
    std::vector<std::string> flat = flatten();
    std::vector<std::string> out;
    out.reserve(flat.size() / 2);
    for (std::size_t i = 0; i < flat.size(); i += 2) out.push_back(std::move(flat[i]));
    return out;
}

std::vector<std::string> HashValue::values() const {
    std::vector<std::string> flat = flatten();
    std::vector<std::string> out;
    out.reserve(flat.size() / 2);
    for (std::size_t i = 1; i < flat.size(); i += 2) out.push_back(std::move(flat[i]));
    return out;
}

// ---------------------------------------------------------------------------
// SetValue
// ---------------------------------------------------------------------------

std::size_t SetValue::size() const {
    switch (encoding_) {
        case ObjEncoding::IntSet:   return intset_.size();
        case ObjEncoding::ListPack: return listpack_.numElements();
        default:                    return table_.size();
    }
}

bool SetValue::contains(std::string_view member) const {
    if (encoding_ == ObjEncoding::IntSet) {
        std::int64_t value = 0;
        // A non-numeric member can never be in an intset, so this is a definite no.
        if (!stringToInt64(member, value)) return false;
        return intset_.contains(value);
    }
    if (encoding_ == ObjEncoding::ListPack) return listpack_.find(member).has_value();
    return table_.find(member) != nullptr;
}

void SetValue::convertToListpack() {
    if (encoding_ != ObjEncoding::IntSet) return;
    for (std::int64_t v : intset_.values()) listpack_.appendInteger(v);
    intset_.clear();
    encoding_ = ObjEncoding::ListPack;
}

void SetValue::convertToHashTable() {
    if (encoding_ == ObjEncoding::HashTable) return;
    if (encoding_ == ObjEncoding::IntSet) {
        for (std::int64_t v : intset_.values()) table_.insert(std::to_string(v), true);
        intset_.clear();
    } else {
        for (const std::string& member : listpack_.toStrings()) table_.insert(member, true);
        listpack_.clear();
    }
    encoding_ = ObjEncoding::HashTable;
}

bool SetValue::add(std::string_view member) {
    const EncodingLimits& limits = defaultLimits();

    if (encoding_ == ObjEncoding::IntSet) {
        std::int64_t value = 0;
        if (stringToInt64(member, value)) {
            if (intset_.contains(value)) return false;
            if (intset_.size() < limits.set_max_intset_entries) {
                return intset_.add(value);
            }
            // Outgrew the intset: become a listpack if still small, else a table.
            if (intset_.size() < limits.set_max_listpack_entries) convertToListpack();
            else                                                  convertToHashTable();
        } else {
            // A single non-integer member ends the intset encoding permanently.
            if (intset_.size() >= limits.set_max_listpack_entries ||
                member.size() > limits.set_max_listpack_value) {
                convertToHashTable();
            } else {
                convertToListpack();
            }
        }
    }

    if (encoding_ == ObjEncoding::ListPack) {
        if (listpack_.find(member)) return false;
        if (listpack_.numElements() >= limits.set_max_listpack_entries ||
            member.size() > limits.set_max_listpack_value) {
            convertToHashTable();
        } else {
            listpack_.append(member);
            return true;
        }
    }

    if (table_.find(member)) return false;
    table_.insert(member, true);
    return true;
}

bool SetValue::erase(std::string_view member) {
    if (encoding_ == ObjEncoding::IntSet) {
        std::int64_t value = 0;
        if (!stringToInt64(member, value)) return false;
        return intset_.remove(value);
    }
    if (encoding_ == ObjEncoding::ListPack) {
        const auto index = listpack_.find(member);
        if (!index) return false;
        return listpack_.eraseAt(*index);
    }
    return table_.erase(member);
}

std::vector<std::string> SetValue::members() const {
    if (encoding_ == ObjEncoding::IntSet) {
        std::vector<std::string> out;
        out.reserve(intset_.size());
        for (std::int64_t v : intset_.values()) out.push_back(std::to_string(v));
        return out;
    }
    if (encoding_ == ObjEncoding::ListPack) return listpack_.toStrings();

    std::vector<std::string> out;
    out.reserve(table_.size());
    table_.forEach([&out](const std::string& k, const bool&) { out.push_back(k); });
    return out;
}

std::optional<std::string> SetValue::randomMember() const {
    if (size() == 0) return std::nullopt;

    if (encoding_ == ObjEncoding::HashTable) {
        const auto* entry = table_.randomEntry();
        if (!entry) return std::nullopt;
        return entry->key;
    }
    // Both array-backed encodings can be indexed directly, so no sampling loop.
    const std::vector<std::string> all = members();
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    return all[gen() % all.size()];
}

// ---------------------------------------------------------------------------
// ZSetValue
// ---------------------------------------------------------------------------

std::size_t ZSetValue::size() const {
    return encoding_ == ObjEncoding::ListPack ? listpack_.numElements() / 2 : scores_.size();
}

std::optional<double> ZSetValue::score(std::string_view member) const {
    if (encoding_ == ObjEncoding::SkipList) {
        const double* found = scores_.find(member);
        return found ? std::optional<double>(*found) : std::nullopt;
    }
    const auto index = listpack_.find(member, 0, 2);
    if (!index) return std::nullopt;
    Listpack::Element element;
    if (!listpack_.get(*index + 1, element)) return std::nullopt;
    return element.is_integer ? static_cast<double>(element.integer)
                              : scoreFromString(element.string);
}

void ZSetValue::convertToSkipList() {
    if (encoding_ == ObjEncoding::SkipList) return;

    std::vector<std::string> flat = listpack_.toStrings();
    for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
        const double s = scoreFromString(flat[i + 1]);
        skiplist_.insert(s, flat[i]);
        scores_.insert(flat[i], s);
    }
    listpack_.clear();
    encoding_ = ObjEncoding::SkipList;
}

bool ZSetValue::add(std::string_view member, double new_score) {
    const EncodingLimits& limits = defaultLimits();

    if (encoding_ == ObjEncoding::ListPack) {
        const auto index = listpack_.find(member, 0, 2);
        if (index) {
            // Rescoring must re-sort, so remove the pair and reinsert in place.
            listpack_.eraseRange(*index, 2);
        } else if (size() >= limits.zset_max_listpack_entries ||
                   member.size() > limits.zset_max_listpack_value) {
            convertToSkipList();
        }

        if (encoding_ == ObjEncoding::ListPack) {
            // A zset listpack is kept ordered by (score, member), so range
            // queries are a straight walk with no sorting at read time.
            std::size_t insert_at = listpack_.numElements();
            for (std::size_t i = 0; i < listpack_.numElements(); i += 2) {
                Listpack::Element member_element, score_element;
                if (!listpack_.get(i, member_element)) break;
                if (!listpack_.get(i + 1, score_element)) break;
                const double existing = score_element.is_integer
                                            ? static_cast<double>(score_element.integer)
                                            : scoreFromString(score_element.string);
                const std::string existing_member = member_element.toString();
                if (existing > new_score ||
                    (existing == new_score && existing_member > member)) {
                    insert_at = i;
                    break;
                }
            }
            listpack_.insertAt(insert_at, member);
            listpack_.insertAt(insert_at + 1, scoreToString(new_score));
            return !index.has_value();
        }
        // Fell through to skiplist encoding; handled below.
    }

    const double* existing = scores_.find(member);
    if (existing) {
        const double old_score = *existing;
        if (old_score == new_score) return false;
        skiplist_.remove(old_score, member);
        skiplist_.insert(new_score, std::string(member));
        scores_.insert(member, new_score);
        return false;
    }
    skiplist_.insert(new_score, std::string(member));
    scores_.insert(member, new_score);
    return true;
}

bool ZSetValue::erase(std::string_view member) {
    if (encoding_ == ObjEncoding::ListPack) {
        const auto index = listpack_.find(member, 0, 2);
        if (!index) return false;
        return listpack_.eraseRange(*index, 2);
    }
    const double* existing = scores_.find(member);
    if (!existing) return false;
    skiplist_.remove(*existing, member);
    scores_.erase(member);
    return true;
}

std::optional<std::size_t> ZSetValue::rank(std::string_view member) const {
    if (encoding_ == ObjEncoding::SkipList) {
        const double* found = scores_.find(member);
        if (!found) return std::nullopt;
        return skiplist_.rankOf(*found, member);
    }
    const auto index = listpack_.find(member, 0, 2);
    if (!index) return std::nullopt;
    // The listpack is already score-ordered, so position is rank.
    return *index / 2;
}

std::vector<std::pair<std::string, double>> ZSetValue::all() const {
    std::vector<std::pair<std::string, double>> out;
    out.reserve(size());

    if (encoding_ == ObjEncoding::ListPack) {
        const std::vector<std::string> flat = listpack_.toStrings();
        for (std::size_t i = 0; i + 1 < flat.size(); i += 2) {
            out.emplace_back(flat[i], scoreFromString(flat[i + 1]));
        }
        return out;
    }
    for (const SkipList::Node* n = skiplist_.head(); n; n = n->levels[0].forward) {
        out.emplace_back(n->member, n->score);
    }
    return out;
}

std::vector<std::pair<std::string, double>> ZSetValue::range(std::size_t start,
                                                             std::size_t stop) const {
    std::vector<std::pair<std::string, double>> everything = all();
    if (start >= everything.size()) return {};
    stop = std::min(stop, everything.size() - 1);
    return {everything.begin() + static_cast<std::ptrdiff_t>(start),
            everything.begin() + static_cast<std::ptrdiff_t>(stop) + 1};
}

std::vector<std::pair<std::string, double>> ZSetValue::rangeByScore(
    const ScoreRange& range) const {
    std::vector<std::pair<std::string, double>> out;
    if (range.isEmpty()) return out;

    if (encoding_ == ObjEncoding::SkipList) {
        // Jump straight to the first in-range node instead of scanning.
        for (const SkipList::Node* n = skiplist_.firstInRange(range); n;
             n = n->levels[0].forward) {
            if (!range.containsScore(n->score)) break;
            out.emplace_back(n->member, n->score);
        }
        return out;
    }
    for (const auto& [member, score] : all()) {
        if (range.containsScore(score)) out.emplace_back(member, score);
    }
    return out;
}

// ---------------------------------------------------------------------------
// ListValue
// ---------------------------------------------------------------------------

std::size_t ListValue::size() const {
    std::size_t total = 0;
    for (const Listpack& node : nodes_) total += node.numElements();
    return total;
}

void ListValue::maybeConvert() {
    // A list stays `listpack` only while it is a single node within the byte
    // budget. One oversized element is enough to make it a quicklist, which is
    // why the check is on bytes and not on the element count.
    encoding_ =
        (nodes_.size() == 1 && nodes_[0].totalBytes() <= defaultLimits().list_max_listpack_bytes)
            ? ObjEncoding::ListPack
            : ObjEncoding::QuickList;
}

// True when appending `value` would push `node` past the per-node byte budget.
// The +11 covers the worst-case encoding header (5 bytes) and backlen (5), plus
// a byte of slack, so a node never overshoots the limit.
static bool wouldOverflowNode(const Listpack& node, std::string_view value) {
    const std::size_t projected = node.totalBytes() + value.size() + 11;
    return node.numElements() > 0 && projected > defaultLimits().list_max_listpack_bytes;
}

void ListValue::pushBack(std::string_view value) {
    if (wouldOverflowNode(nodes_.back(), value)) nodes_.emplace_back();
    nodes_.back().append(value);
    maybeConvert();
}

void ListValue::pushFront(std::string_view value) {
    if (wouldOverflowNode(nodes_.front(), value)) nodes_.insert(nodes_.begin(), Listpack{});
    nodes_.front().insertAt(0, value);
    maybeConvert();
}

std::optional<std::string> ListValue::popFront() {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->numElements() == 0) continue;
        Listpack::Element element;
        if (!it->get(0, element)) return std::nullopt;
        std::string value = element.toString();
        it->eraseAt(0);
        // Keep at least one node so the container is never in a headless state.
        if (it->numElements() == 0 && nodes_.size() > 1) nodes_.erase(it);
        maybeConvert();
        return value;
    }
    return std::nullopt;
}

std::optional<std::string> ListValue::popBack() {
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        const std::size_t count = it->numElements();
        if (count == 0) continue;
        Listpack::Element element;
        if (!it->get(count - 1, element)) return std::nullopt;
        std::string value = element.toString();
        it->eraseAt(count - 1);
        if (it->numElements() == 0 && nodes_.size() > 1) {
            nodes_.erase(std::next(it).base());
        }
        maybeConvert();
        return value;
    }
    return std::nullopt;
}

std::size_t ListValue::resolveIndex(std::int64_t index) const {
    const auto length = static_cast<std::int64_t>(size());
    if (index < 0) index += length;
    if (index < 0 || index >= length) return std::string::npos;
    return static_cast<std::size_t>(index);
}

std::vector<std::string> ListValue::toVector() const {
    std::vector<std::string> out;
    out.reserve(size());
    for (const Listpack& node : nodes_) {
        node.forEach([&out](const Listpack::Element& e) { out.push_back(e.toString()); });
    }
    return out;
}

void ListValue::rebuildFrom(const std::vector<std::string>& items) {
    nodes_.clear();
    nodes_.emplace_back();
    for (const std::string& item : items) {
        if (wouldOverflowNode(nodes_.back(), item)) nodes_.emplace_back();
        nodes_.back().append(item);
    }
    maybeConvert();
}

std::optional<std::string> ListValue::at(std::int64_t index) const {
    const std::size_t resolved = resolveIndex(index);
    if (resolved == std::string::npos) return std::nullopt;

    // Walk node by node rather than materialising the whole list.
    std::size_t remaining = resolved;
    for (const Listpack& node : nodes_) {
        const std::size_t count = node.numElements();
        if (remaining < count) {
            Listpack::Element element;
            if (!node.get(remaining, element)) return std::nullopt;
            return element.toString();
        }
        remaining -= count;
    }
    return std::nullopt;
}

bool ListValue::set(std::int64_t index, std::string_view value) {
    const std::size_t resolved = resolveIndex(index);
    if (resolved == std::string::npos) return false;

    std::size_t remaining = resolved;
    for (Listpack& node : nodes_) {
        const std::size_t count = node.numElements();
        if (remaining < count) return node.replaceAt(remaining, value);
        remaining -= count;
    }
    return false;
}

std::vector<std::string> ListValue::range(std::int64_t start, std::int64_t stop) const {
    const auto length = static_cast<std::int64_t>(size());
    if (length == 0) return {};

    if (start < 0) start += length;
    if (stop  < 0) stop  += length;
    if (start < 0) start = 0;
    if (stop >= length) stop = length - 1;
    if (start > stop || start >= length) return {};

    std::vector<std::string> everything = toVector();
    return {everything.begin() + start, everything.begin() + stop + 1};
}

std::optional<std::size_t> ListValue::indexOf(std::string_view value) const {
    std::size_t index = 0;
    for (const Listpack& node : nodes_) {
        const auto found = node.find(value);
        if (found) return index + *found;
        index += node.numElements();
    }
    return std::nullopt;
}

std::size_t ListValue::removeValue(std::string_view value, std::int64_t count) {
    std::vector<std::string> items = toVector();
    const std::size_t limit = count == 0 ? items.size()
                                         : static_cast<std::size_t>(std::abs(count));
    std::size_t removed = 0;

    if (count >= 0) {
        for (auto it = items.begin(); it != items.end() && removed < limit;) {
            if (*it == value) {
                it = items.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    } else {
        // A negative count scans from the tail, so LREM key -1 v removes the
        // *last* occurrence rather than the first.
        for (auto it = items.rbegin(); it != items.rend() && removed < limit;) {
            if (*it == value) {
                it = std::reverse_iterator(items.erase(std::next(it).base()));
                ++removed;
            } else {
                ++it;
            }
        }
    }

    if (removed > 0) rebuildFrom(items);
    return removed;
}

void ListValue::trim(std::int64_t start, std::int64_t stop) {
    rebuildFrom(range(start, stop));
}

}  // namespace mnemos::core

// ---------------------------------------------------------------------------
// Deep copies
//
// None of these can be a memberwise copy: Dict and SkipList own raw nodes and
// are deliberately non-copyable. Rebuilding from the logical contents also has
// the useful property that the copy re-derives its own encoding, so copying a
// collection that has shrunk below a threshold produces a compact result.
// ---------------------------------------------------------------------------

namespace mnemos::core {

HashValue::HashValue(const HashValue& other) {
    const std::vector<std::string> flat = other.flatten();
    for (std::size_t i = 0; i + 1 < flat.size(); i += 2) set(flat[i], flat[i + 1]);
}

HashValue& HashValue::operator=(const HashValue& other) {
    if (this != &other) *this = HashValue(other);
    return *this;
}

SetValue::SetValue(const SetValue& other) {
    for (const std::string& member : other.members()) add(member);
}

SetValue& SetValue::operator=(const SetValue& other) {
    if (this != &other) *this = SetValue(other);
    return *this;
}

ZSetValue::ZSetValue(const ZSetValue& other) {
    for (const auto& [member, score] : other.all()) add(member, score);
}

ZSetValue& ZSetValue::operator=(const ZSetValue& other) {
    if (this != &other) *this = ZSetValue(other);
    return *this;
}

}  // namespace mnemos::core
