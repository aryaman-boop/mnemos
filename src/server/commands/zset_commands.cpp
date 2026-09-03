// Sorted-set commands.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

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

}  // namespace mnemos::server::cmd
