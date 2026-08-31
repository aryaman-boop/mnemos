// Hash commands.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/commands/type_helpers.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::HashValue;
using core::stringToInt64;

namespace {

std::string formatLongDouble(long double value) {
    char buf[256];
    const int len = std::snprintf(buf, sizeof(buf), "%.17Lf", value);
    if (len < 0) return "0";
    std::string s(buf, static_cast<std::size_t>(len));
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

}  // namespace

void hset(CommandContext& ctx) {
    if ((ctx.argc() - 2) % 2 != 0) {
        replies::wrongArgs(ctx.reply, "hset");
        return;
    }
    bool type_error = false;
    Value* value = lookupOrCreate(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error || !value) return;

    std::int64_t added = 0;
    for (std::size_t i = 2; i + 1 < ctx.argc(); i += 2) {
        // HSET returns the number of fields that were *new*, not the number written.
        if (value->hash()->set(ctx.arg(i), ctx.arg(i + 1))) ++added;
    }
    ctx.server.markDirty((ctx.argc() - 2) / 2);
    ctx.reply.integer(added);
}

void hsetnx(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupOrCreate(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error || !value) return;

    if (value->hash()->contains(ctx.arg(2))) {
        deleteIfEmpty(ctx, ctx.arg(1), *value);
        ctx.reply.integer(0);
        return;
    }
    value->hash()->set(ctx.arg(2), ctx.arg(3));
    ctx.server.markDirty();
    ctx.reply.integer(1);
}

void hget(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.nullBulk();
        return;
    }
    const auto found = value->hash()->get(ctx.arg(2));
    if (found) ctx.reply.bulk(*found);
    else       ctx.reply.nullBulk();
}

void hmget(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;

    ctx.reply.arrayHeader(static_cast<std::int64_t>(ctx.argc() - 2));
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        // A missing hash yields nils rather than an error, one per requested field.
        if (!value) {
            ctx.reply.nullBulk();
            continue;
        }
        const auto found = value->hash()->get(ctx.arg(i));
        if (found) ctx.reply.bulk(*found);
        else       ctx.reply.nullBulk();
    }
}

void hdel(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }
    std::int64_t removed = 0;
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        if (value->hash()->erase(ctx.arg(i))) ++removed;
    }
    if (removed > 0) ctx.server.markDirty(static_cast<std::uint64_t>(removed));
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void hlen(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    ctx.reply.integer(value ? static_cast<std::int64_t>(value->hash()->size()) : 0);
}

void hexists(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    ctx.reply.integer(value && value->hash()->contains(ctx.arg(2)) ? 1 : 0);
}

void hstrlen(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }
    const auto found = value->hash()->get(ctx.arg(2));
    ctx.reply.integer(found ? static_cast<std::int64_t>(found->size()) : 0);
}

void hkeys(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.arrayHeader(0);
        return;
    }
    ctx.reply.bulkArray(value->hash()->fields());
}

void hvals(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.arrayHeader(0);
        return;
    }
    ctx.reply.bulkArray(value->hash()->values());
}

void hgetall(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.mapHeader(0);
        return;
    }
    const std::vector<std::string> flat = value->hash()->flatten();
    // A genuine map in RESP3; a flat array of alternating keys and values in
    // RESP2. ReplyWriter handles the divergence.
    ctx.reply.mapHeader(static_cast<std::int64_t>(flat.size() / 2));
    for (const std::string& item : flat) ctx.reply.bulk(item);
}

void hincrby(CommandContext& ctx) {
    std::int64_t delta = 0;
    if (!stringToInt64(ctx.arg(3), delta)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupOrCreate(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error || !value) return;

    std::int64_t current = 0;
    if (const auto existing = value->hash()->get(ctx.arg(2))) {
        if (!stringToInt64(*existing, current)) {
            ctx.reply.error("ERR hash value is not an integer");
            deleteIfEmpty(ctx, ctx.arg(1), *value);
            return;
        }
    }
    if ((delta > 0 && current > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && current < std::numeric_limits<std::int64_t>::min() - delta)) {
        ctx.reply.error("ERR increment or decrement would overflow");
        deleteIfEmpty(ctx, ctx.arg(1), *value);
        return;
    }

    const std::int64_t result = current + delta;
    value->hash()->set(ctx.arg(2), std::to_string(result));
    ctx.server.markDirty();
    ctx.reply.integer(result);
}

void hincrbyfloat(CommandContext& ctx) {
    char* end = nullptr;
    const long double delta = std::strtold(ctx.arg(3).c_str(), &end);
    if (end == ctx.arg(3).c_str() || *end != '\0' || std::isnan(delta)) {
        replies::notAFloat(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupOrCreate(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error || !value) return;

    long double current = 0.0L;
    if (const auto existing = value->hash()->get(ctx.arg(2))) {
        char* value_end = nullptr;
        current = std::strtold(existing->c_str(), &value_end);
        if (value_end == existing->c_str() || *value_end != '\0' || std::isnan(current)) {
            ctx.reply.error("ERR hash value is not a float");
            deleteIfEmpty(ctx, ctx.arg(1), *value);
            return;
        }
    }

    const long double result = current + delta;
    if (std::isnan(result) || std::isinf(result)) {
        ctx.reply.error("ERR increment would produce NaN or Infinity");
        deleteIfEmpty(ctx, ctx.arg(1), *value);
        return;
    }

    const std::string formatted = formatLongDouble(result);
    value->hash()->set(ctx.arg(2), formatted);
    ctx.server.markDirty();
    ctx.reply.bulk(formatted);
}

void hrandfield(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error) return;

    bool         has_count  = ctx.argc() > 2;
    std::int64_t count      = 1;
    bool         with_values = false;
    if (has_count) {
        if (!stringToInt64(ctx.arg(2), count)) {
            replies::notAnInteger(ctx.reply);
            return;
        }
        if (ctx.argc() > 3) {
            if (!core::equalsIgnoreCase(ctx.arg(3), "WITHVALUES")) {
                replies::syntaxError(ctx.reply);
                return;
            }
            with_values = true;
        }
    }

    if (!value || value->hash()->size() == 0) {
        if (has_count) ctx.reply.arrayHeader(0);
        else           ctx.reply.nullBulk();
        return;
    }

    const std::vector<std::string> flat = value->hash()->flatten();
    const std::size_t field_count = flat.size() / 2;
    static thread_local std::mt19937_64 gen{std::random_device{}()};

    if (!has_count) {
        ctx.reply.bulk(flat[(gen() % field_count) * 2]);
        return;
    }

    std::vector<std::size_t> picks;
    if (count < 0) {
        // A negative count allows repeats and always returns exactly |count|.
        const auto wanted = static_cast<std::size_t>(-count);
        for (std::size_t i = 0; i < wanted; ++i) picks.push_back(gen() % field_count);
    } else {
        // A positive count returns distinct fields, capped at the hash size.
        std::vector<std::size_t> indices(field_count);
        for (std::size_t i = 0; i < field_count; ++i) indices[i] = i;
        std::shuffle(indices.begin(), indices.end(), gen);
        const auto wanted = std::min<std::size_t>(static_cast<std::size_t>(count), field_count);
        picks.assign(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(wanted));
    }

    if (with_values) {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(picks.size() * 2));
        for (std::size_t index : picks) {
            ctx.reply.bulk(flat[index * 2]);
            ctx.reply.bulk(flat[index * 2 + 1]);
        }
    } else {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(picks.size()));
        for (std::size_t index : picks) ctx.reply.bulk(flat[index * 2]);
    }
}

}  // namespace mnemos::server::cmd

namespace mnemos::server::cmd {

// HMSET is HSET with a different reply: +OK rather than the count of new
// fields. Deprecated since 4.0 but still widely used by older clients.
void hmset(CommandContext& ctx) {
    if ((ctx.argc() - 2) % 2 != 0) {
        replies::wrongArgs(ctx.reply, "hmset");
        return;
    }
    bool type_error = false;
    Value* value = lookupOrCreate(ctx, ctx.arg(1), ObjType::Hash, type_error);
    if (type_error || !value) return;

    for (std::size_t i = 2; i + 1 < ctx.argc(); i += 2) {
        value->hash()->set(ctx.arg(i), ctx.arg(i + 1));
    }
    ctx.server.markDirty((ctx.argc() - 2) / 2);
    replies::ok(ctx.reply);
}

}  // namespace mnemos::server::cmd
