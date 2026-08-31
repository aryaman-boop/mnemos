// Sorted-set commands.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/commands/type_helpers.h"
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
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
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

    std::int64_t count = 1;
    if (ctx.argc() > 2 && (!stringToInt64(ctx.arg(2), count) || count < 0)) {
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
    if (!popped.empty()) ctx.server.markDirty(popped.size());
    deleteIfEmpty(ctx, ctx.arg(1), *value);

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
            ctx.reply.doubleValue(final_score);
            return;
        }
    }

    ctx.server.markDirty(updates.size());
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
    if (removed > 0) ctx.server.markDirty(static_cast<std::uint64_t>(removed));
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void zscore(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
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
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
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
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
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
    ctx.reply.doubleValue(result);
}

void zrank(CommandContext& ctx)    { rankGeneric(ctx, false); }
void zrevrank(CommandContext& ctx) { rankGeneric(ctx, true); }

namespace {
void rangeByIndex(CommandContext& ctx, bool reverse) {
    std::int64_t start = 0, stop = 0;
    if (!stringToInt64(ctx.arg(2), start) || !stringToInt64(ctx.arg(3), stop)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool with_scores = false;
    for (std::size_t i = 4; i < ctx.argc(); ++i) {
        if (equalsIgnoreCase(ctx.arg(i), "WITHSCORES")) with_scores = true;
        else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.arrayHeader(0);
        return;
    }

    std::vector<std::pair<std::string, double>> all = value->zset()->all();
    if (reverse) std::reverse(all.begin(), all.end());

    if (!resolveIndexRange(start, stop, static_cast<std::int64_t>(all.size()))) {
        ctx.reply.arrayHeader(0);
        return;
    }
    emitMembers(ctx, {all.begin() + start, all.begin() + stop + 1}, with_scores);
}

void rangeByScore(CommandContext& ctx, bool reverse) {
    // ZREVRANGEBYSCORE takes its bounds as (max, min), the reverse of
    // ZRANGEBYSCORE -- a genuine asymmetry in the command set.
    const std::string& first  = reverse ? ctx.arg(3) : ctx.arg(2);
    const std::string& second = reverse ? ctx.arg(2) : ctx.arg(3);

    ScoreRange range;
    if (!parseScoreBound(first, range.min, range.min_exclusive) ||
        !parseScoreBound(second, range.max, range.max_exclusive)) {
        ctx.reply.error("ERR min or max is not a float");
        return;
    }

    bool         with_scores = false;
    bool         has_limit   = false;
    std::int64_t offset = 0, count = -1;
    for (std::size_t i = 4; i < ctx.argc(); ++i) {
        if (equalsIgnoreCase(ctx.arg(i), "WITHSCORES")) {
            with_scores = true;
        } else if (equalsIgnoreCase(ctx.arg(i), "LIMIT") && i + 2 < ctx.argc()) {
            if (!stringToInt64(ctx.arg(i + 1), offset) || !stringToInt64(ctx.arg(i + 2), count)) {
                replies::notAnInteger(ctx.reply);
                return;
            }
            has_limit = true;
            i += 2;
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.arrayHeader(0);
        return;
    }

    std::vector<std::pair<std::string, double>> items = value->zset()->rangeByScore(range);
    if (reverse) std::reverse(items.begin(), items.end());

    if (has_limit) {
        if (offset < 0 || static_cast<std::size_t>(offset) >= items.size()) {
            ctx.reply.arrayHeader(0);
            return;
        }
        items.erase(items.begin(), items.begin() + offset);
        // A negative count means "everything from the offset onwards".
        if (count >= 0 && static_cast<std::size_t>(count) < items.size()) {
            items.resize(static_cast<std::size_t>(count));
        }
    }
    emitMembers(ctx, items, with_scores);
}
}  // namespace

void zrange(CommandContext& ctx)             { rangeByIndex(ctx, false); }
void zrevrange(CommandContext& ctx)          { rangeByIndex(ctx, true); }
void zrangebyscore(CommandContext& ctx)      { rangeByScore(ctx, false); }
void zrevrangebyscore(CommandContext& ctx)   { rangeByScore(ctx, true); }

void zcount(CommandContext& ctx) {
    ScoreRange range;
    if (!parseScoreBound(ctx.arg(2), range.min, range.min_exclusive) ||
        !parseScoreBound(ctx.arg(3), range.max, range.max_exclusive)) {
        ctx.reply.error("ERR min or max is not a float");
        return;
    }
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
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
    if (removed > 0) ctx.server.markDirty(static_cast<std::uint64_t>(removed));
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
    if (removed > 0) ctx.server.markDirty(static_cast<std::uint64_t>(removed));
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void zpopmin(CommandContext& ctx) { popGeneric(ctx, true); }
void zpopmax(CommandContext& ctx) { popGeneric(ctx, false); }

void zrandmember(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::ZSet, type_error);
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

}  // namespace mnemos::server::cmd
