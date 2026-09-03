// Sorted-set commands.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/commands/type_helpers.h"
#include "server/notify.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;
using core::ScoreRange;
using core::stringToInt64;
using core::ZSetValue;

namespace {

// Parses a score. Accepts "inf"/"+inf"/"-inf" as Redis does, and rejects NaN.
bool parseScore(std::string_view text, double& out) {
    const std::string s(text);
    if (s == "inf" || s == "+inf" || s == "infinity" || s == "+infinity") {
        out = INFINITY;
        return true;
    }
    if (s == "-inf" || s == "-infinity") {
        out = -INFINITY;
        return true;
    }
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || std::isnan(out)) return false;
    return true;
}

// Parses one end of a ZRANGEBYSCORE range, where a leading '(' means exclusive.
bool parseScoreBound(std::string_view text, double& value, bool& exclusive) {
    exclusive = false;
    if (!text.empty() && text.front() == '(') {
        exclusive = true;
        text.remove_prefix(1);
    }
    return parseScore(text, value);
}

void emitMembers(CommandContext& ctx,
                 const std::vector<std::pair<std::string, double>>& items, bool with_scores) {
    if (!with_scores) {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(items.size()));
        for (const auto& [member, score] : items) ctx.reply.bulk(member);
        return;
    }
    // RESP3 returns an array of [member, score] pairs with a real double; RESP2
    // flattens it and sends the score as a bulk string.
    if (ctx.reply.protocolVersion() >= 3) {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(items.size()));
        for (const auto& [member, score] : items) {
            ctx.reply.arrayHeader(2);
            ctx.reply.bulk(member);
            ctx.reply.doubleValue(score);
        }
        return;
    }
    ctx.reply.arrayHeader(static_cast<std::int64_t>(items.size() * 2));
    for (const auto& [member, score] : items) {
        ctx.reply.bulk(member);
        ctx.reply.bulk(net::formatDouble(score));
    }
}

// Normalises a possibly-negative index pair against `length`. Returns false
// when the range selects nothing.
bool resolveIndexRange(std::int64_t& start, std::int64_t& stop, std::int64_t length) {
    if (start < 0) start += length;
    if (stop < 0) stop += length;
    if (start < 0) start = 0;
    if (stop >= length) stop = length - 1;
    return !(length == 0 || start > stop || start >= length);
}

void rankGeneric(CommandContext& ctx, bool reverse) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.nullBulk();
        return;
    }
    const auto rank = value->zset()->rank(ctx.arg(2));
    if (!rank) {
        ctx.reply.nullBulk();
        return;
    }
    // ZREVRANK counts from the other end of the same ordering.
    ctx.reply.integer(reverse
                          ? static_cast<std::int64_t>(value->zset()->size() - *rank - 1)
                          : static_cast<std::int64_t>(*rank));
}

void popGeneric(CommandContext& ctx, bool lowest) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;

    const bool has_count = ctx.argc() > 2;
    std::int64_t count = 1;
    if (has_count && (!stringToInt64(ctx.arg(2), count) || count < 0)) {
        ctx.reply.error("ERR value is out of range, must be positive");
        return;
    }
    if (!value) {
        ctx.reply.arrayHeader(0);
        return;
    }

    std::vector<std::pair<std::string, double>> all = value->zset()->all();
    if (!lowest) std::reverse(all.begin(), all.end());
    const auto wanted = std::min<std::size_t>(static_cast<std::size_t>(count), all.size());

    std::vector<std::pair<std::string, double>> popped(all.begin(),
                                                       all.begin() + static_cast<std::ptrdiff_t>(wanted));
    for (const auto& [member, score] : popped) value->zset()->erase(member);
    if (!popped.empty()) {
        ctx.server.markDirty(popped.size());
        notifyKeyspaceEvent(ctx, notify::kZset, lowest ? "zpopmin" : "zpopmax", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);

    // Without a count RESP3 sends the pair flat; only the counted form wraps
    // each pair in an array of its own.
    if (!has_count && ctx.reply.protocolVersion() >= 3) {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(popped.size() * 2));
        for (const auto& [member, score] : popped) {
            ctx.reply.bulk(member);
            ctx.reply.doubleValue(score);
        }
        return;
    }
    emitMembers(ctx, popped, true);
}

}  // namespace

void zadd(CommandContext& ctx) {
    std::size_t i = 2;
    bool nx = false, xx = false, gt = false, lt = false, ch = false, incr = false;

    for (; i < ctx.argc(); ++i) {
        const std::string& token = ctx.arg(i);
        if      (equalsIgnoreCase(token, "NX"))   nx = true;
        else if (equalsIgnoreCase(token, "XX"))   xx = true;
        else if (equalsIgnoreCase(token, "GT"))   gt = true;
        else if (equalsIgnoreCase(token, "LT"))   lt = true;
        else if (equalsIgnoreCase(token, "CH"))   ch = true;
        else if (equalsIgnoreCase(token, "INCR")) incr = true;
        else break;
    }

    if (nx && (xx || gt || lt)) {
        ctx.reply.error("ERR GT, LT, and/or NX options at the same time are not compatible");
        return;
    }
    if (gt && lt) {
        ctx.reply.error("ERR GT, LT, and/or NX options at the same time are not compatible");
        return;
    }
    const std::size_t pairs_start = i;
    if (pairs_start >= ctx.argc() || (ctx.argc() - pairs_start) % 2 != 0) {
        replies::syntaxError(ctx.reply);
        return;
    }
    if (incr && ctx.argc() - pairs_start != 2) {
        ctx.reply.error("ERR INCR option supports a single increment-element pair");
        return;
    }

    // Validate every score before touching the keyspace, so a malformed pair
    // late in the argument list cannot leave a partial write behind.
    std::vector<std::pair<double, std::string>> updates;
    for (std::size_t j = pairs_start; j + 1 < ctx.argc(); j += 2) {
        double score = 0;
        if (!parseScore(ctx.arg(j), score)) {
            replies::notAFloat(ctx.reply);
            return;
        }
        updates.emplace_back(score, ctx.arg(j + 1));
    }

    bool type_error = false;
    Value* value = xx ? lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error)
                      : lookupOrCreate(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        // XX on a missing key: nothing to update.
        if (incr) ctx.reply.nullBulk();
        else      ctx.reply.integer(0);
        return;
    }

    ZSetValue* zset = value->zset();
    std::int64_t added = 0, changed = 0;

    for (const auto& [score, member] : updates) {
        const auto existing = zset->score(member);

        if (nx && existing) continue;
        if (xx && !existing) continue;

        double final_score = score;
        if (incr) {
            final_score = existing.value_or(0.0) + score;
            if (std::isnan(final_score)) {
                ctx.reply.error("ERR resulting score is not a number (NaN)");
                return;
            }
        }
        // GT/LT only allow the score to move in one direction.
        if (existing && gt && final_score <= *existing) {
            if (incr) { ctx.reply.nullBulk(); return; }
            continue;
        }
        if (existing && lt && final_score >= *existing) {
            if (incr) { ctx.reply.nullBulk(); return; }
            continue;
        }

        if (zset->add(member, final_score)) ++added;
        else if (!existing || *existing != final_score) ++changed;

        if (incr) {
            ctx.server.markDirty();
            // The INCR form announces itself as ZINCRBY does, not as a ZADD.
            notifyKeyspaceEvent(ctx, notify::kZset, "zincr", ctx.arg(1));
            ctx.reply.doubleValue(final_score);
            return;
        }
    }

    ctx.server.markDirty(updates.size());
    // Silent when every update was filtered out by NX/XX/GT/LT: Redis raises the
    // event only for a set that actually moved.
    if (added + changed > 0) notifyKeyspaceEvent(ctx, notify::kZset, "zadd", ctx.arg(1));
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    // CH reports added *plus* changed rather than added alone.
    ctx.reply.integer(ch ? added + changed : added);
}

void zrem(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }
    std::int64_t removed = 0;
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        if (value->zset()->erase(ctx.arg(i))) ++removed;
    }
    if (removed > 0) {
        ctx.server.markDirty(static_cast<std::uint64_t>(removed));
        notifyKeyspaceEvent(ctx, notify::kZset, "zrem", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void zscore(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.nullBulk();
        return;
    }
    const auto score = value->zset()->score(ctx.arg(2));
    if (score) ctx.reply.doubleValue(*score);
    else       ctx.reply.nullBulk();
}

void zmscore(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;

    ctx.reply.arrayHeader(static_cast<std::int64_t>(ctx.argc() - 2));
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        const auto score = value ? value->zset()->score(ctx.arg(i)) : std::nullopt;
        if (score) ctx.reply.doubleValue(*score);
        else       ctx.reply.nullBulk();
    }
}

void zcard(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    ctx.reply.integer(value ? static_cast<std::int64_t>(value->zset()->size()) : 0);
}

void zincrby(CommandContext& ctx) {
    double delta = 0;
    if (!parseScore(ctx.arg(2), delta)) {
        replies::notAFloat(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupOrCreate(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error || !value) return;

    const double current = value->zset()->score(ctx.arg(3)).value_or(0.0);
    const double result  = current + delta;
    if (std::isnan(result)) {
        ctx.reply.error("ERR resulting score is not a number (NaN)");
        deleteIfEmpty(ctx, ctx.arg(1), *value);
        return;
    }
    value->zset()->add(ctx.arg(3), result);
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kZset, "zincr", ctx.arg(1));
    ctx.reply.doubleValue(result);
}

void zrank(CommandContext& ctx)    { rankGeneric(ctx, false); }
void zrevrank(CommandContext& ctx) { rankGeneric(ctx, true); }

namespace {

// The three things the ZRANGE family can range over. Redis folded all of them
// into one command in 7.0 and kept the older spellings as fixed forms of the
// same grammar, so one parser and one collector serve every one of them.
enum class RangeBy { Rank, Score, Lex };

struct RangeSpec {
    RangeBy          by          = RangeBy::Rank;
    bool             reverse     = false;
    bool             with_scores = false;
    bool             has_limit   = false;
    std::int64_t     offset      = 0;
    std::int64_t     count       = -1;
    std::int64_t     start       = 0;  // BYRANK only
    std::int64_t     stop        = 0;
    core::ScoreRange score;
    core::LexRange   lex;
};

// "-", "+", "[member" or "(member". A bare member is not a lex bound: the
// bracket is what says whether the endpoint is included, and Redis refuses
// rather than guessing.
bool parseLexBound(const std::string& text, core::LexBound& bound) {
    if (text == "-") { bound.kind = core::LexBound::Kind::NegInf; return true; }
    if (text == "+") { bound.kind = core::LexBound::Kind::PosInf; return true; }
    if (!text.empty() && (text.front() == '[' || text.front() == '(')) {
        bound.kind      = core::LexBound::Kind::Value;
        bound.exclusive = text.front() == '(';
        bound.value     = text.substr(1);
        return true;
    }
    return false;
}

// Parses the two bounds at `min_index` and every option after them. `generic`
// selects the modern ZRANGE/ZRANGESTORE grammar, which picks its own BY and
// REV; the fixed forms arrive with both already decided and accept only LIMIT
// and WITHSCORES. Writes the error and returns false on any refusal.
bool parseRangeSpec(CommandContext& ctx, std::size_t min_index, bool generic,
                    bool allow_with_scores, RangeSpec& spec) {
    for (std::size_t i = min_index + 2; i < ctx.argc(); ++i) {
        const std::string& token = ctx.arg(i);
        if (generic && equalsIgnoreCase(token, "BYSCORE")) {
            spec.by = RangeBy::Score;
        } else if (generic && equalsIgnoreCase(token, "BYLEX")) {
            spec.by = RangeBy::Lex;
        } else if (generic && equalsIgnoreCase(token, "REV")) {
            spec.reverse = true;
        } else if (allow_with_scores && equalsIgnoreCase(token, "WITHSCORES")) {
            spec.with_scores = true;
        // Accepted by every form, including the ones that cannot honour it: the
        // two combinations below are refused after the loop, by name, and even
        // ZREVRANGE reports them that way rather than a plain syntax error.
        } else if (equalsIgnoreCase(token, "LIMIT") && i + 2 < ctx.argc()) {
            if (!stringToInt64(ctx.arg(i + 1), spec.offset) ||
                !stringToInt64(ctx.arg(i + 2), spec.count)) {
                replies::notAnInteger(ctx.reply);
                return false;
            }
            spec.has_limit = true;
            i += 2;
        } else {
            replies::syntaxError(ctx.reply);
            return false;
        }
    }

    if (spec.has_limit && spec.by == RangeBy::Rank) {
        ctx.reply.error(
            "ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
        return false;
    }
    if (spec.with_scores && spec.by == RangeBy::Lex) {
        ctx.reply.error("ERR syntax error, WITHSCORES not supported in combination with BYLEX");
        return false;
    }

    // REV swaps which argument is the minimum for a score or lex range -- those
    // are then written (max, min). An index range keeps its argument order and
    // simply indexes the reversed sequence, which is why it reads arg directly.
    const std::string& low  = spec.reverse ? ctx.arg(min_index + 1) : ctx.arg(min_index);
    const std::string& high = spec.reverse ? ctx.arg(min_index) : ctx.arg(min_index + 1);

    switch (spec.by) {
        case RangeBy::Rank:
            if (!stringToInt64(ctx.arg(min_index), spec.start) ||
                !stringToInt64(ctx.arg(min_index + 1), spec.stop)) {
                replies::notAnInteger(ctx.reply);
                return false;
            }
            break;
        case RangeBy::Score:
            if (!parseScoreBound(low, spec.score.min, spec.score.min_exclusive) ||
                !parseScoreBound(high, spec.score.max, spec.score.max_exclusive)) {
                ctx.reply.error("ERR min or max is not a float");
                return false;
            }
            break;
        case RangeBy::Lex:
            if (!parseLexBound(low, spec.lex.min) || !parseLexBound(high, spec.lex.max)) {
                ctx.reply.error("ERR min or max not valid string range item");
                return false;
            }
            break;
    }
    return true;
}

std::vector<std::pair<std::string, double>> collectRange(const ZSetValue& zset,
                                                         const RangeSpec& spec) {
    if (spec.by == RangeBy::Rank) {
        std::vector<std::pair<std::string, double>> all = zset.all();
        if (spec.reverse) std::reverse(all.begin(), all.end());
        std::int64_t start = spec.start, stop = spec.stop;
        if (!resolveIndexRange(start, stop, static_cast<std::int64_t>(all.size()))) return {};
        return {all.begin() + start, all.begin() + stop + 1};
    }

    std::vector<std::pair<std::string, double>> items =
        spec.by == RangeBy::Score ? zset.rangeByScore(spec.score) : zset.rangeByLex(spec.lex);
    if (spec.reverse) std::reverse(items.begin(), items.end());
    if (!spec.has_limit) return items;

    // A negative offset selects nothing; a negative count means "to the end".
    if (spec.offset < 0 || static_cast<std::size_t>(spec.offset) >= items.size()) return {};
    items.erase(items.begin(), items.begin() + spec.offset);
    if (spec.count >= 0 && static_cast<std::size_t>(spec.count) < items.size()) {
        items.resize(static_cast<std::size_t>(spec.count));
    }
    return items;
}

void rangeCommand(CommandContext& ctx, RangeBy by, bool reverse, bool generic,
                  bool allow_with_scores) {
    RangeSpec spec;
    spec.by      = by;
    spec.reverse = reverse;
    if (!parseRangeSpec(ctx, 2, generic, allow_with_scores, spec)) return;

    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.arrayHeader(0);
        return;
    }
    emitMembers(ctx, collectRange(*value->zset(), spec), spec.with_scores);
}

}  // namespace

void zrange(CommandContext& ctx) {
    rangeCommand(ctx, RangeBy::Rank, false, true, true);
}
void zrevrange(CommandContext& ctx) {
    rangeCommand(ctx, RangeBy::Rank, true, false, true);
}
void zrangebyscore(CommandContext& ctx) {
    rangeCommand(ctx, RangeBy::Score, false, false, true);
}
void zrevrangebyscore(CommandContext& ctx) {
    rangeCommand(ctx, RangeBy::Score, true, false, true);
}
void zrangebylex(CommandContext& ctx) {
    rangeCommand(ctx, RangeBy::Lex, false, false, true);
}
void zrevrangebylex(CommandContext& ctx) {
    rangeCommand(ctx, RangeBy::Lex, true, false, true);
}

void zcount(CommandContext& ctx) {
    ScoreRange range;
    if (!parseScoreBound(ctx.arg(2), range.min, range.min_exclusive) ||
        !parseScoreBound(ctx.arg(3), range.max, range.max_exclusive)) {
        ctx.reply.error("ERR min or max is not a float");
        return;
    }
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    ctx.reply.integer(value ? static_cast<std::int64_t>(value->zset()->rangeByScore(range).size())
                            : 0);
}

void zremrangebyrank(CommandContext& ctx) {
    std::int64_t start = 0, stop = 0;
    if (!stringToInt64(ctx.arg(2), start) || !stringToInt64(ctx.arg(3), stop)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }

    const std::vector<std::pair<std::string, double>> all = value->zset()->all();
    if (!resolveIndexRange(start, stop, static_cast<std::int64_t>(all.size()))) {
        ctx.reply.integer(0);
        return;
    }
    std::int64_t removed = 0;
    for (std::int64_t i = start; i <= stop; ++i) {
        if (value->zset()->erase(all[static_cast<std::size_t>(i)].first)) ++removed;
    }
    if (removed > 0) {
        ctx.server.markDirty(static_cast<std::uint64_t>(removed));
        notifyKeyspaceEvent(ctx, notify::kZset, "zremrangebyrank", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void zremrangebyscore(CommandContext& ctx) {
    ScoreRange range;
    if (!parseScoreBound(ctx.arg(2), range.min, range.min_exclusive) ||
        !parseScoreBound(ctx.arg(3), range.max, range.max_exclusive)) {
        ctx.reply.error("ERR min or max is not a float");
        return;
    }
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }
    std::int64_t removed = 0;
    for (const auto& [member, score] : value->zset()->rangeByScore(range)) {
        if (value->zset()->erase(member)) ++removed;
    }
    if (removed > 0) {
        ctx.server.markDirty(static_cast<std::uint64_t>(removed));
        notifyKeyspaceEvent(ctx, notify::kZset, "zremrangebyscore", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void zpopmin(CommandContext& ctx) { popGeneric(ctx, true); }
void zpopmax(CommandContext& ctx) { popGeneric(ctx, false); }

void zrandmember(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;

    bool         has_count   = ctx.argc() > 2;
    std::int64_t count       = 1;
    bool         with_scores = false;
    if (has_count) {
        if (!stringToInt64(ctx.arg(2), count)) {
            replies::notAnInteger(ctx.reply);
            return;
        }
        if (ctx.argc() > 3) {
            if (!equalsIgnoreCase(ctx.arg(3), "WITHSCORES")) {
                replies::syntaxError(ctx.reply);
                return;
            }
            with_scores = true;
        }
    }

    if (!value || value->zset()->size() == 0) {
        if (has_count) ctx.reply.arrayHeader(0);
        else           ctx.reply.nullBulk();
        return;
    }

    const std::vector<std::pair<std::string, double>> all = value->zset()->all();
    static thread_local std::mt19937_64 gen{std::random_device{}()};

    if (!has_count) {
        ctx.reply.bulk(all[gen() % all.size()].first);
        return;
    }

    std::vector<std::pair<std::string, double>> picked;
    if (count < 0) {
        const auto wanted = static_cast<std::size_t>(-count);
        for (std::size_t i = 0; i < wanted; ++i) picked.push_back(all[gen() % all.size()]);
    } else {
        std::vector<std::pair<std::string, double>> shuffled = all;
        std::shuffle(shuffled.begin(), shuffled.end(), gen);
        const auto wanted = std::min<std::size_t>(static_cast<std::size_t>(count), shuffled.size());
        picked.assign(shuffled.begin(), shuffled.begin() + static_cast<std::ptrdiff_t>(wanted));
    }
    emitMembers(ctx, picked, with_scores);
}


void zlexcount(CommandContext& ctx) {
    core::LexRange range;
    if (!parseLexBound(ctx.arg(2), range.min) || !parseLexBound(ctx.arg(3), range.max)) {
        ctx.reply.error("ERR min or max not valid string range item");
        return;
    }
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    ctx.reply.integer(value ? static_cast<std::int64_t>(value->zset()->rangeByLex(range).size())
                            : 0);
}

void zremrangebylex(CommandContext& ctx) {
    core::LexRange range;
    if (!parseLexBound(ctx.arg(2), range.min) || !parseLexBound(ctx.arg(3), range.max)) {
        ctx.reply.error("ERR min or max not valid string range item");
        return;
    }
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }
    std::int64_t removed = 0;
    for (const auto& [member, score] : value->zset()->rangeByLex(range)) {
        if (value->zset()->erase(member)) ++removed;
    }
    if (removed > 0) {
        ctx.server.markDirty(static_cast<std::uint64_t>(removed));
        notifyKeyspaceEvent(ctx, notify::kZset, "zremrangebylex", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void zrangestore(CommandContext& ctx) {
    RangeSpec spec;
    if (!parseRangeSpec(ctx, 3, /*generic=*/true, /*allow_with_scores=*/false, spec)) return;

    bool type_error = false;
    Value* source = lookupTypedRead(ctx, ctx.arg(2), ObjType::ZSet, type_error);
    if (type_error) return;

    std::vector<std::pair<std::string, double>> items;
    if (source) items = collectRange(*source->zset(), spec);

    const std::string& destination = ctx.arg(1);
    if (items.empty()) {
        // An empty result leaves no empty zset behind: it removes whatever the
        // destination held, and a key that really disappeared says so.
        if (ctx.db.erase(destination)) {
            ctx.server.markDirty();
            notifyKeyspaceEvent(ctx, notify::kGeneric, "del", destination);
        }
        ctx.reply.integer(0);
        return;
    }

    // setKey rather than a merge: the destination is replaced outright, TTL
    // included, whatever type it used to hold.
    Value fresh = Value::makeZSet();
    for (const auto& [member, score] : items) fresh.zset()->add(member, score);
    ctx.db.setKey(destination, std::move(fresh));
    ctx.server.markDirty(items.size());
    notifyKeyspaceEvent(ctx, notify::kZset, "zrangestore", destination);
    ctx.reply.integer(static_cast<std::int64_t>(items.size()));
}


namespace {

enum class ZSetOp { Union, Intersect, Difference };
enum class Aggregate { Sum, Min, Max };

// One input to the set operations. They accept plain sets as well as sorted
// ones -- a set member enters with a score of 1, which its weight then
// multiplies like any other -- so this is the flattened form both reduce to.
struct ZSetSource {
    std::vector<std::pair<std::string, double>> members;
    double                                      weight = 1.0;
};

// Reads `count` keys starting at `first`. A missing key is an empty input
// rather than an error, which is why ZINTERSTORE over a nonexistent key stores
// nothing instead of failing.
bool gatherZSetSources(CommandContext& ctx, std::size_t first, std::size_t count,
                       std::vector<ZSetSource>& out) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& key = ctx.arg(first + i);
        ZSetSource source;
        Value* value = ctx.db.lookupRead(key, ctx.nowMs());
        if (!value) {
            notifyKeyMiss(ctx, key);
        } else if (value->type() == ObjType::ZSet) {
            source.members = value->zset()->all();
        } else if (value->type() == ObjType::Set) {
            for (const std::string& member : value->set()->members()) {
                source.members.emplace_back(member, 1.0);
            }
        } else {
            replies::wrongType(ctx.reply);
            return false;
        }
        out.push_back(std::move(source));
    }
    return true;
}

// Summing an infinity with its opposite gives a NaN, which is not a score any
// sorted set can hold. Redis flattens it to zero at both points one can appear:
// after the weight multiplication (0 * inf) and after each SUM step.
double weighted(double score, double weight) {
    const double product = score * weight;
    return std::isnan(product) ? 0.0 : product;
}

double aggregateScores(Aggregate how, double accumulated, double incoming) {
    switch (how) {
        case Aggregate::Sum: {
            const double sum = accumulated + incoming;
            return std::isnan(sum) ? 0.0 : sum;
        }
        case Aggregate::Min: return std::min(accumulated, incoming);
        case Aggregate::Max: return std::max(accumulated, incoming);
    }
    return accumulated;
}

// `limit` stops an intersection early once that many members are known, which
// only ZINTERCARD wants -- it reads the size and never the members. Takes
// `sources` by reference because the intersection reorders them.
std::vector<std::pair<std::string, double>> applyZSetOp(ZSetOp op,
                                                        std::vector<ZSetSource>& sources,
                                                        Aggregate how, std::size_t limit) {
    std::vector<std::pair<std::string, double>> result;
    if (sources.empty()) return result;

    if (op == ZSetOp::Union) {
        std::unordered_map<std::string, double> totals;
        for (const ZSetSource& source : sources) {
            for (const auto& [member, score] : source.members) {
                const double value = weighted(score, source.weight);
                auto [slot, inserted] = totals.try_emplace(member, value);
                if (!inserted) slot->second = aggregateScores(how, slot->second, value);
            }
        }
        result.assign(totals.begin(), totals.end());
    } else if (op == ZSetOp::Intersect) {
        // Smallest first: the result cannot outgrow it, so this bounds the
        // probes. It also fixes the order the scores aggregate in, and Redis
        // sorts by cardinality for the same reason -- which matters, because a
        // SUM that passes through a NaN depends on where the NaN falls.
        std::stable_sort(sources.begin(), sources.end(),
                         [](const ZSetSource& a, const ZSetSource& b) {
                             return a.members.size() < b.members.size();
                         });
        std::vector<std::unordered_map<std::string, double>> others;
        for (std::size_t i = 1; i < sources.size(); ++i) {
            others.emplace_back(sources[i].members.begin(), sources[i].members.end());
        }
        for (const auto& [member, score] : sources.front().members) {
            double total = weighted(score, sources.front().weight);
            bool in_all = true;
            for (std::size_t i = 0; i < others.size(); ++i) {
                const auto found = others[i].find(member);
                if (found == others[i].end()) {
                    in_all = false;
                    break;
                }
                total = aggregateScores(how, total, weighted(found->second, sources[i + 1].weight));
            }
            if (!in_all) continue;
            result.emplace_back(member, total);
            if (limit > 0 && result.size() >= limit) return result;
        }
    } else {
        // Difference keeps the first input's scores untouched: it takes neither
        // weights nor an aggregate, because nothing is ever combined.
        std::vector<std::unordered_set<std::string>> others;
        for (std::size_t i = 1; i < sources.size(); ++i) {
            std::unordered_set<std::string> seen;
            for (const auto& [member, score] : sources[i].members) seen.insert(member);
            others.push_back(std::move(seen));
        }
        for (const auto& entry : sources.front().members) {
            bool excluded = false;
            for (const auto& other : others) {
                if (other.count(entry.first)) {
                    excluded = true;
                    break;
                }
            }
            if (!excluded) result.push_back(entry);
        }
    }

    // Redis accumulates into a real sorted set and reads it back out, so the
    // reply is in (score, member) order however the inputs were given.
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second < b.second;
        return a.first < b.first;
    });
    return result;
}

std::string_view zsetOpName(ZSetOp op, bool store) {
    switch (op) {
        case ZSetOp::Union:      return store ? "zunionstore" : "zunion";
        case ZSetOp::Intersect:  return store ? "zinterstore" : "zinter";
        case ZSetOp::Difference: return store ? "zdiffstore" : "zdiff";
    }
    return "";
}

// The numkeys preamble every set operation shares. Three distinct errors, in
// Redis's order: a count that is not a number at all, one below 1, and one that
// claims more keys than were sent -- which is only ever a syntax error, however
// far out it is.
bool parseNumkeys(CommandContext& ctx, std::size_t index, std::string_view command,
                  std::int64_t& numkeys) {
    if (!stringToInt64(ctx.arg(index), numkeys)) {
        replies::notAnInteger(ctx.reply);
        return false;
    }
    if (numkeys < 1) {
        ctx.reply.error("ERR at least 1 input key is needed for '" + std::string(command) +
                        "' command");
        return false;
    }
    if (static_cast<std::size_t>(numkeys) > ctx.argc() - (index + 1)) {
        replies::syntaxError(ctx.reply);
        return false;
    }
    return true;
}

void zsetOpCommand(CommandContext& ctx, ZSetOp op, bool store) {
    const std::size_t numkeys_index = store ? 2 : 1;
    std::int64_t numkeys = 0;
    if (!parseNumkeys(ctx, numkeys_index, zsetOpName(op, store), numkeys)) return;
    const std::size_t first_key = numkeys_index + 1;

    // Keys before options, which is the order Redis checks them in: a WRONGTYPE
    // among the inputs beats a syntax error in the trailing arguments.
    std::vector<ZSetSource> sources;
    if (!gatherZSetSources(ctx, first_key, static_cast<std::size_t>(numkeys), sources)) return;

    Aggregate how = Aggregate::Sum;
    bool with_scores = false;
    std::vector<double> weights(static_cast<std::size_t>(numkeys), 1.0);
    for (std::size_t i = first_key + static_cast<std::size_t>(numkeys); i < ctx.argc(); ++i) {
        const std::string& token = ctx.arg(i);
        if (op != ZSetOp::Difference && equalsIgnoreCase(token, "WEIGHTS") &&
            i + static_cast<std::size_t>(numkeys) < ctx.argc()) {
            for (std::size_t w = 0; w < static_cast<std::size_t>(numkeys); ++w) {
                if (!parseScore(ctx.arg(i + 1 + w), weights[w])) {
                    ctx.reply.error("ERR weight value is not a float");
                    return;
                }
            }
            i += static_cast<std::size_t>(numkeys);
        } else if (op != ZSetOp::Difference && equalsIgnoreCase(token, "AGGREGATE") &&
                   i + 1 < ctx.argc()) {
            const std::string& mode = ctx.arg(i + 1);
            if (equalsIgnoreCase(mode, "SUM")) {
                how = Aggregate::Sum;
            } else if (equalsIgnoreCase(mode, "MIN")) {
                how = Aggregate::Min;
            } else if (equalsIgnoreCase(mode, "MAX")) {
                how = Aggregate::Max;
            } else {
                replies::syntaxError(ctx.reply);
                return;
            }
            ++i;
        } else if (!store && equalsIgnoreCase(token, "WITHSCORES")) {
            with_scores = true;
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }
    for (std::size_t i = 0; i < sources.size(); ++i) sources[i].weight = weights[i];

    std::vector<std::pair<std::string, double>> result = applyZSetOp(op, sources, how, 0);

    if (!store) {
        emitMembers(ctx, result, with_scores);
        return;
    }

    const std::string& destination = ctx.arg(1);
    if (!result.empty()) {
        // setKey rather than erase-then-create: it drops the old TTL the same
        // way, but only announces `new` when the destination really is new.
        ctx.db.setKey(destination, Value::makeZSet());
        Value* stored = ctx.db.lookupWrite(destination, ctx.nowMs());
        for (const auto& [member, score] : result) stored->zset()->add(member, score);
        notifyKeyspaceEvent(ctx, notify::kZset, zsetOpName(op, true), destination);
    } else if (ctx.db.erase(destination)) {
        // An empty result is a delete, and that is the only event it raises.
        notifyKeyspaceEvent(ctx, notify::kGeneric, "del", destination);
    }
    ctx.server.markDirty();
    ctx.reply.integer(static_cast<std::int64_t>(result.size()));
}

}  // namespace

void zunion(CommandContext& ctx)      { zsetOpCommand(ctx, ZSetOp::Union, false); }
void zinter(CommandContext& ctx)      { zsetOpCommand(ctx, ZSetOp::Intersect, false); }
void zdiff(CommandContext& ctx)       { zsetOpCommand(ctx, ZSetOp::Difference, false); }
void zunionstore(CommandContext& ctx) { zsetOpCommand(ctx, ZSetOp::Union, true); }
void zinterstore(CommandContext& ctx) { zsetOpCommand(ctx, ZSetOp::Intersect, true); }
void zdiffstore(CommandContext& ctx)  { zsetOpCommand(ctx, ZSetOp::Difference, true); }

void zintercard(CommandContext& ctx) {
    std::int64_t numkeys = 0;
    if (!parseNumkeys(ctx, 1, "zintercard", numkeys)) return;

    std::size_t limit = 0;  // 0 means unlimited
    const std::size_t after_keys = 2 + static_cast<std::size_t>(numkeys);
    if (ctx.argc() > after_keys) {
        if (!equalsIgnoreCase(ctx.arg(after_keys), "LIMIT") || ctx.argc() != after_keys + 2) {
            replies::syntaxError(ctx.reply);
            return;
        }
        std::int64_t parsed = 0;
        if (!stringToInt64(ctx.arg(after_keys + 1), parsed) || parsed < 0) {
            ctx.reply.error("ERR LIMIT can't be negative");
            return;
        }
        limit = static_cast<std::size_t>(parsed);
    }

    std::vector<ZSetSource> sources;
    if (!gatherZSetSources(ctx, 2, static_cast<std::size_t>(numkeys), sources)) return;
    ctx.reply.integer(static_cast<std::int64_t>(
        applyZSetOp(ZSetOp::Intersect, sources, Aggregate::Sum, limit).size()));
}

void zmpop(CommandContext& ctx) {
    std::int64_t numkeys = 0;
    if (!stringToInt64(ctx.arg(1), numkeys) || numkeys <= 0) {
        ctx.reply.error("ERR numkeys should be greater than 0");
        return;
    }
    if (numkeys >= static_cast<std::int64_t>(ctx.argc())) {
        replies::syntaxError(ctx.reply);
        return;
    }
    const std::size_t where_index = 2 + static_cast<std::size_t>(numkeys);
    if (where_index >= ctx.argc()) {
        replies::syntaxError(ctx.reply);
        return;
    }
    bool lowest = true;
    if (equalsIgnoreCase(ctx.arg(where_index), "MIN")) {
        lowest = true;
    } else if (equalsIgnoreCase(ctx.arg(where_index), "MAX")) {
        lowest = false;
    } else {
        replies::syntaxError(ctx.reply);
        return;
    }

    std::int64_t count = 1;
    if (ctx.argc() > where_index + 1) {
        if (!equalsIgnoreCase(ctx.arg(where_index + 1), "COUNT") ||
            ctx.argc() != where_index + 3) {
            replies::syntaxError(ctx.reply);
            return;
        }
        if (!stringToInt64(ctx.arg(where_index + 2), count) || count <= 0) {
            ctx.reply.error("ERR count should be greater than 0");
            return;
        }
    }

    // The first key holding anything wins outright and the rest are never
    // examined: ZMPOP's key list is a preference order, not a set to merge.
    for (std::size_t i = 2; i < where_index; ++i) {
        const std::string& key = ctx.arg(i);
        bool type_error = false;
        Value* value = lookupTyped(ctx, key, ObjType::ZSet, type_error);
        if (type_error) return;
        if (!value || value->zset()->size() == 0) continue;

        std::vector<std::pair<std::string, double>> all = value->zset()->all();
        if (!lowest) std::reverse(all.begin(), all.end());
        const auto wanted = std::min<std::size_t>(static_cast<std::size_t>(count), all.size());
        std::vector<std::pair<std::string, double>> popped(
            all.begin(), all.begin() + static_cast<std::ptrdiff_t>(wanted));
        for (const auto& [member, score] : popped) value->zset()->erase(member);
        ctx.server.markDirty(popped.size());
        notifyKeyspaceEvent(ctx, notify::kZset, lowest ? "zpopmin" : "zpopmax", key);
        deleteIfEmpty(ctx, key, *value);

        ctx.reply.arrayHeader(2);
        ctx.reply.bulk(key);
        ctx.reply.arrayHeader(static_cast<std::int64_t>(popped.size()));
        for (const auto& [member, score] : popped) {
            ctx.reply.arrayHeader(2);
            ctx.reply.bulk(member);
            if (ctx.reply.protocolVersion() >= 3) {
                ctx.reply.doubleValue(score);
            } else {
                ctx.reply.bulk(net::formatDouble(score));
            }
        }
        return;
    }
    // Nothing anywhere is a null array, not an empty one -- the same shape the
    // blocking form returns on timeout.
    ctx.reply.nullArray();
}

}  // namespace mnemos::server::cmd
