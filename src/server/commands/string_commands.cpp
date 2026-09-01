// String commands. Note how often the work is really about *encodings* and
// *TTL semantics* rather than about the bytes themselves.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/notify.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;
using core::stringToInt64;
using core::Value;

namespace {

// Largest string we will let a key grow to, mirroring proto-max-bulk-len.
constexpr std::int64_t kMaxStringSize = 512LL * 1024 * 1024;

struct ExpireOptions {
    bool         set        = false;
    bool         persist    = false;
    bool         keep_ttl   = false;
    std::int64_t at_ms      = 0;
};

enum class ParseResult { Ok, Syntax, NotInteger, InvalidExpire };

// Parses EX/PX/EXAT/PXAT into an absolute millisecond deadline. Redis rejects a
// non-positive relative TTL outright rather than treating it as "delete now",
// because it is almost always a caller bug.
ParseResult parseExpireToken(std::string_view token, const std::string& value,
                             std::int64_t now_ms, std::string_view command,
                             ExpireOptions& out) {
    std::int64_t n = 0;
    if (!stringToInt64(value, n)) return ParseResult::NotInteger;

    const bool relative = equalsIgnoreCase(token, "EX") || equalsIgnoreCase(token, "PX");
    const bool seconds  = equalsIgnoreCase(token, "EX") || equalsIgnoreCase(token, "EXAT");

    if (relative && n <= 0) return ParseResult::InvalidExpire;

    // Overflow guard: seconds -> ms is a x1000 multiply that a hostile TTL can
    // easily push past int64.
    if (seconds) {
        if (n > std::numeric_limits<std::int64_t>::max() / 1000 ||
            n < std::numeric_limits<std::int64_t>::min() / 1000) {
            return ParseResult::InvalidExpire;
        }
        n *= 1000;
    }

    out.set   = true;
    out.at_ms = relative ? now_ms + n : n;
    (void)command;
    return ParseResult::Ok;
}

bool isExpireToken(std::string_view t) {
    return equalsIgnoreCase(t, "EX") || equalsIgnoreCase(t, "PX") ||
           equalsIgnoreCase(t, "EXAT") || equalsIgnoreCase(t, "PXAT");
}

// Applies a parsed TTL decision to a key that has just been written.
void applyExpire(CommandContext& ctx, const std::string& key, const ExpireOptions& opts) {
    if (opts.set)            ctx.db.setExpireAt(key, opts.at_ms);
    else if (opts.persist)   ctx.db.persist(key);
    else if (!opts.keep_ttl) ctx.db.persist(key);
}

// Redis's ld2string(LD_STR_HUMAN): fixed notation with trailing zeros stripped,
// so INCRBYFLOAT k 1.0 returns "1" and not "1.000000000000000".
std::string formatLongDouble(long double value) {
    char buf[256];
    int len = std::snprintf(buf, sizeof(buf), "%.17Lf", value);
    if (len < 0) return "0";
    std::string s(buf, static_cast<std::size_t>(len));
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

bool ensureString(CommandContext& ctx, const Value* v) {
    if (v && !v->isString()) {
        replies::wrongType(ctx.reply);
        return false;
    }
    return true;
}

}  // namespace

void set(CommandContext& ctx) {
    const std::string& key   = ctx.arg(1);
    const std::string& value = ctx.arg(2);

    bool nx = false, xx = false, get = false;
    ExpireOptions opts;

    for (std::size_t i = 3; i < ctx.argc(); ++i) {
        const std::string& token = ctx.arg(i);
        if (equalsIgnoreCase(token, "NX")) {
            if (xx) { replies::syntaxError(ctx.reply); return; }
            nx = true;
        } else if (equalsIgnoreCase(token, "XX")) {
            if (nx) { replies::syntaxError(ctx.reply); return; }
            xx = true;
        } else if (equalsIgnoreCase(token, "GET")) {
            get = true;
        } else if (equalsIgnoreCase(token, "KEEPTTL")) {
            if (opts.set) { replies::syntaxError(ctx.reply); return; }
            opts.keep_ttl = true;
        } else if (isExpireToken(token)) {
            if (opts.set || opts.keep_ttl || i + 1 >= ctx.argc()) {
                replies::syntaxError(ctx.reply);
                return;
            }
            switch (parseExpireToken(token, ctx.arg(i + 1), ctx.nowMs(), "set", opts)) {
                case ParseResult::Ok: break;
                case ParseResult::NotInteger: replies::notAnInteger(ctx.reply); return;
                case ParseResult::InvalidExpire:
                    ctx.reply.error("ERR invalid expire time in 'set' command");
                    return;
                case ParseResult::Syntax: replies::syntaxError(ctx.reply); return;
            }
            ++i;
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    Value* existing = ctx.db.lookupWrite(key, ctx.nowMs());

    // SET ... GET must report WRONGTYPE if the old value is not a string, even
    // though the write itself would have been legal.
    std::string old_value;
    bool        had_old = false;
    if (get) {
        if (!ensureString(ctx, existing)) return;
        if (existing) {
            old_value = existing->stringValue();
            had_old   = true;
        } else {
            // The GET option makes this a read as well as a write, and a read
            // that found nothing is a key miss -- even when the write is then
            // aborted by NX/XX.
            notifyKeyMiss(ctx, key);
        }
    }

    const bool aborted = (nx && existing) || (xx && !existing);
    if (aborted) {
        if (get) {
            if (had_old) ctx.reply.bulk(old_value);
            else         ctx.reply.nullBulk();
        } else {
            ctx.reply.nullBulk();
        }
        return;
    }

    // overwriteValue rather than setKey: we manage the TTL ourselves just below,
    // so that KEEPTTL can preserve it.
    ctx.db.overwriteValue(key, Value::makeString(value));
    applyExpire(ctx, key, opts);
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "set", key);
    // SET with EX is two events, not one: the TTL is a separate fact about the
    // key and subscribers watching only `g` still want to hear about it.
    if (opts.set) notifyKeyspaceEvent(ctx, notify::kGeneric, "expire", key);

    if (get) {
        if (had_old) ctx.reply.bulk(old_value);
        else         ctx.reply.nullBulk();
    } else {
        replies::ok(ctx.reply);
    }
}

void get(CommandContext& ctx) {
    Value* v = ctx.db.lookupRead(ctx.arg(1), ctx.nowMs());
    if (!v) {
        ++ctx.server.stats().keyspace_misses;
        notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.nullBulk();
        return;
    }
    if (!ensureString(ctx, v)) return;
    ++ctx.server.stats().keyspace_hits;
    ctx.reply.bulk(v->stringValue());
}

void getset(CommandContext& ctx) {
    Value* existing = ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs());
    if (!ensureString(ctx, existing)) return;

    if (existing) {
        ctx.reply.bulk(existing->stringValue());
    } else {
        notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.nullBulk();
    }

    ctx.db.setKey(ctx.arg(1), Value::makeString(ctx.arg(2)));
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "set", ctx.arg(1));
}

void getdel(CommandContext& ctx) {
    Value* existing = ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs());
    if (!ensureString(ctx, existing)) return;
    if (!existing) {
        notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.nullBulk();
        return;
    }
    ctx.reply.bulk(existing->stringValue());
    ctx.db.erase(ctx.arg(1));
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kGeneric, "del", ctx.arg(1));
}

void getex(CommandContext& ctx) {
    const std::string& key = ctx.arg(1);
    ExpireOptions opts;

    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        const std::string& token = ctx.arg(i);
        if (equalsIgnoreCase(token, "PERSIST")) {
            if (opts.set) { replies::syntaxError(ctx.reply); return; }
            opts.persist = true;
        } else if (isExpireToken(token)) {
            if (opts.set || opts.persist || i + 1 >= ctx.argc()) {
                replies::syntaxError(ctx.reply);
                return;
            }
            switch (parseExpireToken(token, ctx.arg(i + 1), ctx.nowMs(), "getex", opts)) {
                case ParseResult::Ok: break;
                case ParseResult::NotInteger: replies::notAnInteger(ctx.reply); return;
                case ParseResult::InvalidExpire:
                    ctx.reply.error("ERR invalid expire time in 'getex' command");
                    return;
                case ParseResult::Syntax: replies::syntaxError(ctx.reply); return;
            }
            ++i;
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    Value* v = ctx.db.lookupRead(key, ctx.nowMs());
    if (!v) {
        notifyKeyMiss(ctx, key);
        ctx.reply.nullBulk();
        return;
    }
    if (!ensureString(ctx, v)) return;

    ctx.reply.bulk(v->stringValue());
    // GETEX with no TTL option is a pure read and must not clear the TTL, which
    // is why this cannot just call applyExpire unconditionally.
    if (opts.set) {
        if (opts.at_ms <= ctx.nowMs()) {
            // An EXAT already in the past is a delete, and announces itself as
            // one -- not as an expiry, which nothing here waited for.
            ctx.db.erase(key);
            ctx.server.markDirty();
            notifyKeyspaceEvent(ctx, notify::kGeneric, "del", key);
        } else {
            ctx.db.setExpireAt(key, opts.at_ms);
            ctx.server.markDirty();
            notifyKeyspaceEvent(ctx, notify::kGeneric, "expire", key);
        }
    } else if (opts.persist) {
        if (ctx.db.persist(key)) {
            ctx.server.markDirty();
            notifyKeyspaceEvent(ctx, notify::kGeneric, "persist", key);
        }
    }
}

void setnx(CommandContext& ctx) {
    if (ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs())) {
        ctx.reply.integer(0);
        return;
    }
    ctx.db.setKey(ctx.arg(1), Value::makeString(ctx.arg(2)));
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "set", ctx.arg(1));
    ctx.reply.integer(1);
}

namespace {
void setexGeneric(CommandContext& ctx, std::int64_t unit_multiplier, std::string_view name) {
    std::int64_t ttl = 0;
    if (!stringToInt64(ctx.arg(2), ttl)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    if (ttl <= 0) {
        ctx.reply.error("ERR invalid expire time in '" + std::string(name) + "' command");
        return;
    }
    if (ttl > std::numeric_limits<std::int64_t>::max() / unit_multiplier) {
        ctx.reply.error("ERR invalid expire time in '" + std::string(name) + "' command");
        return;
    }
    ctx.db.setKey(ctx.arg(1), Value::makeString(ctx.arg(3)));
    ctx.db.setExpireAt(ctx.arg(1), ctx.nowMs() + ttl * unit_multiplier);
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "set", ctx.arg(1));
    notifyKeyspaceEvent(ctx, notify::kGeneric, "expire", ctx.arg(1));
    replies::ok(ctx.reply);
}
}  // namespace

void setex(CommandContext& ctx)  { setexGeneric(ctx, 1000, "setex"); }
void psetex(CommandContext& ctx) { setexGeneric(ctx, 1, "psetex"); }

void mset(CommandContext& ctx) {
    if ((ctx.argc() - 1) % 2 != 0) {
        replies::wrongArgs(ctx.reply, "mset");
        return;
    }
    for (std::size_t i = 1; i < ctx.argc(); i += 2) {
        ctx.db.setKey(ctx.arg(i), Value::makeString(ctx.arg(i + 1)));
        notifyKeyspaceEvent(ctx, notify::kString, "set", ctx.arg(i));
    }
    ctx.server.markDirty((ctx.argc() - 1) / 2);
    replies::ok(ctx.reply);
}

void msetnx(CommandContext& ctx) {
    if ((ctx.argc() - 1) % 2 != 0) {
        replies::wrongArgs(ctx.reply, "msetnx");
        return;
    }
    // All-or-nothing: check every key before writing any of them.
    for (std::size_t i = 1; i < ctx.argc(); i += 2) {
        if (ctx.db.lookupWrite(ctx.arg(i), ctx.nowMs())) {
            ctx.reply.integer(0);
            return;
        }
    }
    for (std::size_t i = 1; i < ctx.argc(); i += 2) {
        ctx.db.setKey(ctx.arg(i), Value::makeString(ctx.arg(i + 1)));
        notifyKeyspaceEvent(ctx, notify::kString, "set", ctx.arg(i));
    }
    ctx.server.markDirty((ctx.argc() - 1) / 2);
    ctx.reply.integer(1);
}

void mget(CommandContext& ctx) {
    ctx.reply.arrayHeader(static_cast<std::int64_t>(ctx.argc() - 1));
    for (std::size_t i = 1; i < ctx.argc(); ++i) {
        Value* v = ctx.db.lookupRead(ctx.arg(i), ctx.nowMs());
        // A wrong-type key inside MGET yields a nil for that slot rather than
        // failing the whole command -- MGET is explicitly best-effort.
        if (!v) notifyKeyMiss(ctx, ctx.arg(i));
        if (!v || !v->isString()) ctx.reply.nullBulk();
        else                      ctx.reply.bulk(v->stringValue());
    }
}

void append(CommandContext& ctx) {
    Value* existing = ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs());
    if (!ensureString(ctx, existing)) return;

    if (!existing) {
        // APPEND creates a *raw* string: the result is mutable by definition, so
        // leaving it embstr-encoded would force a re-encode on the next append.
        ctx.db.setKey(ctx.arg(1), Value::makeRawString(ctx.arg(2)));
        ctx.server.markDirty();
        notifyKeyspaceEvent(ctx, notify::kString, "append", ctx.arg(1));
        ctx.reply.integer(static_cast<std::int64_t>(ctx.arg(2).size()));
        return;
    }

    if (static_cast<std::int64_t>(existing->stringLength() + ctx.arg(2).size()) > kMaxStringSize) {
        ctx.reply.error("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
        return;
    }

    existing->appendString(ctx.arg(2));
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "append", ctx.arg(1));
    ctx.reply.integer(static_cast<std::int64_t>(existing->stringLength()));
}

void strlen_(CommandContext& ctx) {
    Value* v = ctx.db.lookupRead(ctx.arg(1), ctx.nowMs());
    if (!v) {
        notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.integer(0);
        return;
    }
    if (!ensureString(ctx, v)) return;
    ctx.reply.integer(static_cast<std::int64_t>(v->stringLength()));
}

namespace {
void incrDecrBy(CommandContext& ctx, std::int64_t delta) {
    Value* v = ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs());
    if (!ensureString(ctx, v)) return;

    std::int64_t current = 0;
    if (v && !v->asInt(current)) {
        replies::notAnInteger(ctx.reply);
        return;
    }

    // Signed overflow is UB in C++, so the check has to happen *before* the add.
    if ((delta > 0 && current > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && current < std::numeric_limits<std::int64_t>::min() - delta)) {
        ctx.reply.error("ERR increment or decrement would overflow");
        return;
    }

    const std::int64_t result = current + delta;
    ctx.db.overwriteValue(ctx.arg(1), Value::makeInt(result));
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "incrby", ctx.arg(1));
    ctx.reply.integer(result);
}
}  // namespace

void incr(CommandContext& ctx) { incrDecrBy(ctx, 1); }
void decr(CommandContext& ctx) { incrDecrBy(ctx, -1); }

void incrby(CommandContext& ctx) {
    std::int64_t delta = 0;
    if (!stringToInt64(ctx.arg(2), delta)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    incrDecrBy(ctx, delta);
}

void decrby(CommandContext& ctx) {
    std::int64_t delta = 0;
    if (!stringToInt64(ctx.arg(2), delta)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    // DECRBY INT64_MIN cannot be expressed as a positive increment, so negating
    // it here would overflow before we ever reach the arithmetic guard.
    if (delta == std::numeric_limits<std::int64_t>::min()) {
        ctx.reply.error("ERR decrement would overflow");
        return;
    }
    incrDecrBy(ctx, -delta);
}

void incrbyfloat(CommandContext& ctx) {
    Value* v = ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs());
    if (!ensureString(ctx, v)) return;

    char* end = nullptr;
    const std::string& increment_str = ctx.arg(2);
    const long double increment = std::strtold(increment_str.c_str(), &end);
    if (end == increment_str.c_str() || *end != '\0' || std::isnan(increment)) {
        replies::notAFloat(ctx.reply);
        return;
    }

    long double current = 0.0L;
    if (v) {
        const std::string s = v->stringValue();
        char* value_end = nullptr;
        current = std::strtold(s.c_str(), &value_end);
        if (value_end == s.c_str() || *value_end != '\0' || std::isnan(current)) {
            replies::notAFloat(ctx.reply);
            return;
        }
    }

    const long double result = current + increment;
    if (std::isnan(result) || std::isinf(result)) {
        ctx.reply.error("ERR increment would produce NaN or Infinity");
        return;
    }

    // The result is stored as its *string* form, so a subsequent GET returns
    // exactly what INCRBYFLOAT replied -- no float formatting drift.
    const std::string formatted = formatLongDouble(result);
    ctx.db.overwriteValue(ctx.arg(1), Value::makeString(formatted));
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "incrbyfloat", ctx.arg(1));
    ctx.reply.bulk(formatted);
}

void setrange(CommandContext& ctx) {
    std::int64_t offset = 0;
    if (!stringToInt64(ctx.arg(2), offset)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    if (offset < 0) {
        ctx.reply.error("ERR offset is out of range");
        return;
    }

    const std::string& patch = ctx.arg(3);
    Value* v = ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs());
    if (!ensureString(ctx, v)) return;

    if (patch.empty()) {
        ctx.reply.integer(v ? static_cast<std::int64_t>(v->stringLength()) : 0);
        return;
    }
    if (offset + static_cast<std::int64_t>(patch.size()) > kMaxStringSize) {
        ctx.reply.error("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
        return;
    }

    if (!v) {
        ctx.db.setKey(ctx.arg(1), Value::makeRawString(""));
        v = ctx.db.lookupWrite(ctx.arg(1), ctx.nowMs());
    }

    std::string& data = v->mutableString();
    // The gap between the old end and `offset` is zero-filled, which is why
    // SETRANGE on a fresh key can produce embedded NULs.
    if (data.size() < static_cast<std::size_t>(offset) + patch.size()) {
        data.resize(static_cast<std::size_t>(offset) + patch.size(), '\0');
    }
    std::copy(patch.begin(), patch.end(), data.begin() + offset);

    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kString, "setrange", ctx.arg(1));
    ctx.reply.integer(static_cast<std::int64_t>(data.size()));
}

void getrange(CommandContext& ctx) {
    std::int64_t start = 0;
    std::int64_t end   = 0;
    if (!stringToInt64(ctx.arg(2), start) || !stringToInt64(ctx.arg(3), end)) {
        replies::notAnInteger(ctx.reply);
        return;
    }

    Value* v = ctx.db.lookupRead(ctx.arg(1), ctx.nowMs());
    if (!v) {
        notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.bulk("");
        return;
    }
    if (!ensureString(ctx, v)) return;

    const std::string data = v->stringValue();
    const auto length = static_cast<std::int64_t>(data.size());

    // Negative indices count back from the end; -1 is the last byte.
    if (start < 0) start = length + start;
    if (end < 0)   end   = length + end;
    if (start < 0) start = 0;
    if (end < 0)   end   = 0;
    if (end >= length) end = length - 1;

    if (length == 0 || start > end || start >= length) {
        ctx.reply.bulk("");
        return;
    }
    ctx.reply.bulk(std::string_view(data).substr(static_cast<std::size_t>(start),
                                                 static_cast<std::size_t>(end - start + 1)));
}

}  // namespace mnemos::server::cmd
