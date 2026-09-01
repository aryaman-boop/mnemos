// Key-level commands: existence, type, iteration, renaming, and TTLs.
#include <limits>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/notify.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;
using core::globMatch;
using core::stringToInt64;
using core::Value;

namespace {

// SCAN's COUNT is a *hint* about how much work to do per call, not a limit on
// how many elements come back. We honour it by running whole buckets until we
// have visited roughly that many slots.
constexpr std::int64_t kDefaultScanCount = 10;
constexpr std::int64_t kMaxScanIterations = 10000;

}  // namespace

void del(CommandContext& ctx) {
    std::int64_t removed = 0;
    for (std::size_t i = 1; i < ctx.argc(); ++i) {
        // lookupRead first so an already-expired key is not counted as deleted.
        if (!ctx.db.lookupRead(ctx.arg(i), ctx.nowMs())) continue;
        if (ctx.db.erase(ctx.arg(i))) {
            ++removed;
            notifyKeyspaceEvent(ctx, notify::kGeneric, "del", ctx.arg(i));
        }
    }
    if (removed > 0) ctx.server.markDirty(static_cast<std::uint64_t>(removed));
    ctx.reply.integer(removed);
}

// UNLINK differs from DEL only in that real Redis frees the value on a
// background thread when it is large. Our values are freed inline, so the
// observable semantics are identical -- which is exactly what the docs promise.
void unlink(CommandContext& ctx) { del(ctx); }

void exists(CommandContext& ctx) {
    std::int64_t count = 0;
    // Repeated keys are counted repeatedly: EXISTS k k returns 2.
    for (std::size_t i = 1; i < ctx.argc(); ++i) {
        if (ctx.db.exists(ctx.arg(i), ctx.nowMs())) ++count;
        else                                        notifyKeyMiss(ctx, ctx.arg(i));
    }
    ctx.reply.integer(count);
}

void type(CommandContext& ctx) {
    Value* v = ctx.db.lookupRead(ctx.arg(1), ctx.nowMs());
    if (!v) notifyKeyMiss(ctx, ctx.arg(1));
    ctx.reply.simpleString(v ? core::typeName(v->type()) : std::string_view("none"));
}

void touch(CommandContext& ctx) {
    std::int64_t count = 0;
    for (std::size_t i = 1; i < ctx.argc(); ++i) {
        if (ctx.db.lookupRead(ctx.arg(i), ctx.nowMs())) ++count;
        else                                            notifyKeyMiss(ctx, ctx.arg(i));
    }
    ctx.reply.integer(count);
}

void keys(CommandContext& ctx) {
    const std::string& pattern = ctx.arg(1);
    std::vector<std::string> matches;

    for (const std::string& key : ctx.db.keys()) {
        if (!globMatch(pattern, key)) continue;
        // Filter through the read path so logically-expired keys never surface.
        if (!ctx.db.lookupRead(key, ctx.nowMs())) continue;
        matches.push_back(key);
    }
    ctx.reply.bulkArray(matches);
}

void scan(CommandContext& ctx) {
    std::uint64_t cursor = 0;
    {
        // The cursor is an opaque uint64 on the wire, so parse it as unsigned;
        // it is not a key count and must not be validated as one.
        try {
            cursor = std::stoull(ctx.arg(1));
        } catch (...) {
            ctx.reply.error("ERR invalid cursor");
            return;
        }
    }

    std::string  pattern;
    bool         has_pattern = false;
    std::int64_t count       = kDefaultScanCount;
    std::string  type_filter;

    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        if (equalsIgnoreCase(ctx.arg(i), "MATCH") && i + 1 < ctx.argc()) {
            pattern     = ctx.arg(i + 1);
            has_pattern = true;
            ++i;
        } else if (equalsIgnoreCase(ctx.arg(i), "COUNT") && i + 1 < ctx.argc()) {
            if (!stringToInt64(ctx.arg(i + 1), count) || count < 1) {
                replies::syntaxError(ctx.reply);
                return;
            }
            ++i;
        } else if (equalsIgnoreCase(ctx.arg(i), "TYPE") && i + 1 < ctx.argc()) {
            type_filter = core::toLower(ctx.arg(i + 1));
            ++i;
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    std::vector<std::string> collected;
    std::int64_t iterations = 0;
    do {
        cursor = ctx.db.scan(cursor, collected);
        ++iterations;
    } while (cursor != 0 && static_cast<std::int64_t>(collected.size()) < count &&
             iterations < kMaxScanIterations);

    std::vector<std::string> results;
    results.reserve(collected.size());
    for (const std::string& key : collected) {
        if (has_pattern && !globMatch(pattern, key)) continue;
        Value* v = ctx.db.lookupRead(key, ctx.nowMs());
        if (!v) continue;
        if (!type_filter.empty() && core::typeName(v->type()) != type_filter) continue;
        results.push_back(key);
    }

    ctx.reply.arrayHeader(2);
    ctx.reply.bulk(std::to_string(cursor));
    ctx.reply.bulkArray(results);
}

void randomkey(CommandContext& ctx) {
    const std::string* key = ctx.db.randomKey(ctx.nowMs());
    if (!key) ctx.reply.nullBulk();
    else      ctx.reply.bulk(*key);
}

namespace {
void renameGeneric(CommandContext& ctx, bool fail_if_exists) {
    const std::string& src = ctx.arg(1);
    const std::string& dst = ctx.arg(2);

    Value* source = ctx.db.lookupWrite(src, ctx.nowMs());
    if (!source) {
        ctx.reply.error("ERR no such key");
        return;
    }

    if (src == dst) {
        // Renaming a key to itself is a no-op, but RENAMENX must still report
        // that the destination "already existed".
        if (fail_if_exists) ctx.reply.integer(0);
        else                replies::ok(ctx.reply);
        return;
    }

    if (fail_if_exists && ctx.db.lookupWrite(dst, ctx.nowMs())) {
        ctx.reply.integer(0);
        return;
    }

    // The TTL travels with the value: RENAME preserves the source's expiry.
    Value        moved  = *source;
    const std::int64_t expire_at = ctx.db.expireAtMs(src);

    ctx.db.erase(src);
    ctx.db.setKey(dst, std::move(moved));
    if (expire_at >= 0) ctx.db.setExpireAt(dst, expire_at);

    ctx.server.markDirty();
    // Two events for one command, in the order a subscriber can act on: the key
    // that went away, then the key that appeared.
    notifyKeyspaceEvent(ctx, notify::kGeneric, "rename_from", src);
    notifyKeyspaceEvent(ctx, notify::kGeneric, "rename_to", dst);
    if (fail_if_exists) ctx.reply.integer(1);
    else                replies::ok(ctx.reply);
}
}  // namespace

void rename(CommandContext& ctx)   { renameGeneric(ctx, false); }
void renamenx(CommandContext& ctx) { renameGeneric(ctx, true); }

void copy(CommandContext& ctx) {
    const std::string& src = ctx.arg(1);
    const std::string& dst = ctx.arg(2);

    bool replace   = false;
    int  target_db = ctx.client.dbIndex();

    for (std::size_t i = 3; i < ctx.argc(); ++i) {
        if (equalsIgnoreCase(ctx.arg(i), "REPLACE")) {
            replace = true;
        } else if (equalsIgnoreCase(ctx.arg(i), "DB") && i + 1 < ctx.argc()) {
            std::int64_t n = 0;
            if (!stringToInt64(ctx.arg(i + 1), n) || n < 0 ||
                n >= static_cast<std::int64_t>(ctx.server.databaseCount())) {
                ctx.reply.error("ERR DB index is out of range");
                return;
            }
            target_db = static_cast<int>(n);
            ++i;
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    if (target_db == ctx.client.dbIndex() && src == dst) {
        ctx.reply.error("ERR source and destination objects are the same");
        return;
    }

    Value* source = ctx.db.lookupRead(src, ctx.nowMs());
    if (!source) {
        notifyKeyMiss(ctx, src);
        ctx.reply.integer(0);
        return;
    }

    Database& destination = ctx.server.db(target_db);
    if (!replace && destination.lookupRead(dst, ctx.nowMs())) {
        ctx.reply.integer(0);
        return;
    }

    Value copied = *source;
    const std::int64_t expire_at = ctx.db.expireAtMs(src);
    destination.setKey(dst, std::move(copied));
    if (expire_at >= 0) destination.setExpireAt(dst, expire_at);

    ctx.server.markDirty();
    // The event belongs to the database the copy landed in, which COPY ... DB
    // makes a different one from the caller's.
    notifyKeyspaceEvent(ctx, notify::kGeneric, "copy_to", dst, target_db);
    ctx.reply.integer(1);
}

// ---------------------------------------------------------------------------
// Expiry
// ---------------------------------------------------------------------------

namespace {

// unit_multiplier converts the argument to milliseconds; `absolute` selects
// between EXPIRE-style (relative) and EXPIREAT-style (absolute) semantics.
void expireGeneric(CommandContext& ctx, std::int64_t unit_multiplier, bool absolute,
                   std::string_view command) {
    std::int64_t raw = 0;
    if (!stringToInt64(ctx.arg(2), raw)) {
        replies::notAnInteger(ctx.reply);
        return;
    }

    bool nx = false, xx = false, gt = false, lt = false;
    for (std::size_t i = 3; i < ctx.argc(); ++i) {
        if      (equalsIgnoreCase(ctx.arg(i), "NX")) nx = true;
        else if (equalsIgnoreCase(ctx.arg(i), "XX")) xx = true;
        else if (equalsIgnoreCase(ctx.arg(i), "GT")) gt = true;
        else if (equalsIgnoreCase(ctx.arg(i), "LT")) lt = true;
        else {
            ctx.reply.error("ERR Unsupported option " + ctx.arg(i));
            return;
        }
    }
    // NX is mutually exclusive with the others, and GT/LT contradict each other.
    if ((gt && lt) || (nx && (xx || gt || lt))) {
        ctx.reply.error("ERR NX and XX, GT or LT options at the same time are not compatible");
        return;
    }

    if (!ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs())) {
        ctx.reply.integer(0);
        return;
    }

    // Overflow guard before the multiply and before adding `now`.
    if (raw > std::numeric_limits<std::int64_t>::max() / unit_multiplier ||
        raw < std::numeric_limits<std::int64_t>::min() / unit_multiplier) {
        ctx.reply.error("ERR invalid expire time in '" + std::string(command) + "' command");
        return;
    }
    std::int64_t when = raw * unit_multiplier;
    if (!absolute) {
        if (when > std::numeric_limits<std::int64_t>::max() - ctx.nowMs()) {
            ctx.reply.error("ERR invalid expire time in '" + std::string(command) + "' command");
            return;
        }
        when += ctx.nowMs();
    }

    const std::int64_t current = ctx.db.expireAtMs(ctx.arg(1));
    const bool         has_ttl = current >= 0;

    if (nx && has_ttl)  { ctx.reply.integer(0); return; }
    if (xx && !has_ttl) { ctx.reply.integer(0); return; }
    // A key with no TTL behaves as though its expiry were infinitely far away,
    // so GT can never set a TTL on it while LT always can.
    if (gt && (!has_ttl || when <= current)) { ctx.reply.integer(0); return; }
    if (lt && has_ttl && when >= current)    { ctx.reply.integer(0); return; }

    if (when <= ctx.nowMs()) {
        // A deadline in the past deletes the key immediately rather than
        // storing an expiry that the next lookup would have to clean up.
        ctx.db.erase(ctx.arg(1));
        ctx.server.markDirty();
        notifyKeyspaceEvent(ctx, notify::kGeneric, "del", ctx.arg(1));
        ctx.reply.integer(1);
        return;
    }

    ctx.db.setExpireAt(ctx.arg(1), when);
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kGeneric, "expire", ctx.arg(1));
    ctx.reply.integer(1);
}

void ttlGeneric(CommandContext& ctx, bool in_milliseconds) {
    const std::int64_t ms = ctx.db.ttlMs(ctx.arg(1), ctx.nowMs());
    if (ms < 0) {
        if (ms == -2) notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.integer(ms);  // -1 no TTL, -2 no key
        return;
    }
    // Seconds are rounded to nearest, matching Redis, so a 1500ms TTL reports 2.
    ctx.reply.integer(in_milliseconds ? ms : (ms + 500) / 1000);
}

void expireTimeGeneric(CommandContext& ctx, bool in_milliseconds) {
    if (!ctx.db.lookupRead(ctx.arg(1), ctx.nowMs())) {
        notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.integer(-2);
        return;
    }
    const std::int64_t when = ctx.db.expireAtMs(ctx.arg(1));
    if (when < 0) {
        ctx.reply.integer(-1);
        return;
    }
    ctx.reply.integer(in_milliseconds ? when : when / 1000);
}

}  // namespace

void expire(CommandContext& ctx)    { expireGeneric(ctx, 1000, false, "expire"); }
void pexpire(CommandContext& ctx)   { expireGeneric(ctx, 1,    false, "pexpire"); }
void expireat(CommandContext& ctx)  { expireGeneric(ctx, 1000, true,  "expireat"); }
void pexpireat(CommandContext& ctx) { expireGeneric(ctx, 1,    true,  "pexpireat"); }

void ttl(CommandContext& ctx)  { ttlGeneric(ctx, false); }
void pttl(CommandContext& ctx) { ttlGeneric(ctx, true); }

void expiretime(CommandContext& ctx)  { expireTimeGeneric(ctx, false); }
void pexpiretime(CommandContext& ctx) { expireTimeGeneric(ctx, true); }

void persist(CommandContext& ctx) {
    if (!ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs())) {
        ctx.reply.integer(0);
        return;
    }
    const bool removed = ctx.db.persist(ctx.arg(1));
    if (removed) {
        ctx.server.markDirty();
        notifyKeyspaceEvent(ctx, notify::kGeneric, "persist", ctx.arg(1));
    }
    ctx.reply.integer(removed ? 1 : 0);
}

}  // namespace mnemos::server::cmd
