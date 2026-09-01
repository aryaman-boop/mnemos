// List commands.
#include <algorithm>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/commands/type_helpers.h"
#include "server/notify.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;
using core::ListValue;
using core::stringToInt64;

namespace {

// LPUSH/RPUSH and their conditional -X variants all reduce to this.
void pushGeneric(CommandContext& ctx, bool at_front, bool require_existing) {
    bool type_error = false;
    Value* value = require_existing
                       ? lookupTyped(ctx, ctx.arg(1), ObjType::List, type_error)
                       : lookupOrCreate(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;
    if (!value) {
        // LPUSHX/RPUSHX on a missing key is a no-op returning 0.
        ctx.reply.integer(0);
        return;
    }

    ListValue* list = value->list();
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        // LPUSH a b c leaves the list as [c, b, a]: each element is pushed onto
        // the head in turn, so argument order is reversed in the result.
        if (at_front) list->pushFront(ctx.arg(i));
        else          list->pushBack(ctx.arg(i));
    }
    ctx.server.markDirty(ctx.argc() - 2);
    // One event for the whole command, however many elements it pushed.
    notifyKeyspaceEvent(ctx, notify::kList, at_front ? "lpush" : "rpush", ctx.arg(1));
    ctx.reply.integer(static_cast<std::int64_t>(list->size()));
}

void popGeneric(CommandContext& ctx, bool from_front) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;

    // An optional count changes the reply shape from a bulk string to an array,
    // and `LPOP key 0` must return an empty array rather than nil.
    bool         has_count = ctx.argc() > 2;
    std::int64_t count     = 1;
    if (has_count) {
        if (!stringToInt64(ctx.arg(2), count)) {
            replies::notAnInteger(ctx.reply);
            return;
        }
        if (count < 0) {
            ctx.reply.error("ERR value is out of range, must be positive");
            return;
        }
    }

    if (!value) {
        if (has_count) ctx.reply.nullArray();
        else           ctx.reply.nullBulk();
        return;
    }

    ListValue* list = value->list();
    std::vector<std::string> popped;
    for (std::int64_t i = 0; i < count; ++i) {
        auto item = from_front ? list->popFront() : list->popBack();
        if (!item) break;
        popped.push_back(std::move(*item));
    }

    if (!popped.empty()) {
        ctx.server.markDirty(popped.size());
        notifyKeyspaceEvent(ctx, notify::kList, from_front ? "lpop" : "rpop", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);

    if (has_count) {
        ctx.reply.bulkArray(popped);
    } else if (popped.empty()) {
        ctx.reply.nullBulk();
    } else {
        ctx.reply.bulk(popped[0]);
    }
}

// Shared by RPOPLPUSH and LMOVE.
void moveGeneric(CommandContext& ctx, const std::string& source, const std::string& destination,
                 bool from_front, bool to_front) {
    bool type_error = false;
    Value* src = lookupTyped(ctx, source, ObjType::List, type_error);
    if (type_error) return;
    if (!src) {
        ctx.reply.nullBulk();
        return;
    }
    // Check the destination's type before mutating the source, so a WRONGTYPE
    // destination cannot leave the element popped and lost.
    Value* dst_existing = lookupTyped(ctx, destination, ObjType::List, type_error);
    if (type_error) return;
    (void)dst_existing;

    auto item = from_front ? src->list()->popFront() : src->list()->popBack();
    if (!item) {
        ctx.reply.nullBulk();
        return;
    }
    // The destination is filled -- and announced -- before the source's pop is,
    // which is the order Redis emits them in and the only order in which a
    // subscriber never sees the element belong to neither list.
    Value* dst = lookupOrCreate(ctx, destination, ObjType::List, type_error);
    if (type_error || !dst) return;
    if (to_front) dst->list()->pushFront(*item);
    else          dst->list()->pushBack(*item);
    notifyKeyspaceEvent(ctx, notify::kList, to_front ? "lpush" : "rpush", destination);
    notifyKeyspaceEvent(ctx, notify::kList, from_front ? "lpop" : "rpop", source);

    // Re-looked-up rather than reusing `src`: creating the destination may have
    // rehashed the dict out from under it. When source and destination are the
    // same key this also correctly finds it non-empty again.
    if (Value* src_now = ctx.db.lookupWrite(source, ctx.nowMs())) {
        deleteIfEmpty(ctx, source, *src_now);
    }

    ctx.server.markDirty();
    ctx.reply.bulk(*item);
}

}  // namespace

void lpush(CommandContext& ctx)  { pushGeneric(ctx, true,  false); }
void rpush(CommandContext& ctx)  { pushGeneric(ctx, false, false); }
void lpushx(CommandContext& ctx) { pushGeneric(ctx, true,  true); }
void rpushx(CommandContext& ctx) { pushGeneric(ctx, false, true); }
void lpop(CommandContext& ctx)   { popGeneric(ctx, true); }
void rpop(CommandContext& ctx)   { popGeneric(ctx, false); }

void llen(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;
    ctx.reply.integer(value ? static_cast<std::int64_t>(value->list()->size()) : 0);
}

void lrange(CommandContext& ctx) {
    std::int64_t start = 0, stop = 0;
    if (!stringToInt64(ctx.arg(2), start) || !stringToInt64(ctx.arg(3), stop)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.arrayHeader(0);
        return;
    }
    ctx.reply.bulkArray(value->list()->range(start, stop));
}

void lindex(CommandContext& ctx) {
    std::int64_t index = 0;
    if (!stringToInt64(ctx.arg(2), index)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.nullBulk();
        return;
    }
    auto item = value->list()->at(index);
    if (item) ctx.reply.bulk(*item);
    else      ctx.reply.nullBulk();
}

void lset(CommandContext& ctx) {
    std::int64_t index = 0;
    if (!stringToInt64(ctx.arg(2), index)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.error("ERR no such key");
        return;
    }
    if (!value->list()->set(index, ctx.arg(3))) {
        ctx.reply.error("ERR index out of range");
        return;
    }
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kList, "lset", ctx.arg(1));
    replies::ok(ctx.reply);
}

void lrem(CommandContext& ctx) {
    std::int64_t count = 0;
    if (!stringToInt64(ctx.arg(2), count)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }
    const std::size_t removed = value->list()->removeValue(ctx.arg(3), count);
    if (removed > 0) {
        ctx.server.markDirty(removed);
        notifyKeyspaceEvent(ctx, notify::kList, "lrem", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(static_cast<std::int64_t>(removed));
}

void ltrim(CommandContext& ctx) {
    std::int64_t start = 0, stop = 0;
    if (!stringToInt64(ctx.arg(2), start) || !stringToInt64(ctx.arg(3), stop)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;
    if (!value) {
        replies::ok(ctx.reply);
        return;
    }
    value->list()->trim(start, stop);
    ctx.server.markDirty();
    // Announced even when the range trimmed nothing: LTRIM reports on the key,
    // not on the elements, and Redis fires it for every call that finds the key.
    notifyKeyspaceEvent(ctx, notify::kList, "ltrim", ctx.arg(1));
    // Trimming to an empty range deletes the key outright.
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    replies::ok(ctx.reply);
}

void lpos(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::List, type_error);
    if (type_error) return;

    std::int64_t rank = 1, count = -1;
    for (std::size_t i = 3; i < ctx.argc(); ++i) {
        if (equalsIgnoreCase(ctx.arg(i), "RANK") && i + 1 < ctx.argc()) {
            if (!stringToInt64(ctx.arg(i + 1), rank) || rank == 0) {
                ctx.reply.error("ERR RANK can't be zero. Use 1 to start searching from the "
                                "first matching element in the head of the list or a negative "
                                "rank to start searching from the tail. A rank larger than the "
                                "list size is also acceptable.");
                return;
            }
            ++i;
        } else if (equalsIgnoreCase(ctx.arg(i), "COUNT") && i + 1 < ctx.argc()) {
            if (!stringToInt64(ctx.arg(i + 1), count) || count < 0) {
                ctx.reply.error("ERR COUNT can't be negative");
                return;
            }
            ++i;
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    const bool want_multiple = count >= 0;
    if (!value) {
        if (want_multiple) ctx.reply.arrayHeader(0);
        else               ctx.reply.nullBulk();
        return;
    }

    const std::vector<std::string> items = value->list()->range(0, -1);
    std::vector<std::int64_t> matches;
    const std::int64_t wanted = (count == 0) ? static_cast<std::int64_t>(items.size()) : count;

    if (rank > 0) {
        std::int64_t skipped = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i] != ctx.arg(2)) continue;
            if (++skipped < rank) continue;
            matches.push_back(static_cast<std::int64_t>(i));
            if (want_multiple && static_cast<std::int64_t>(matches.size()) >= wanted) break;
            if (!want_multiple) break;
        }
    } else {
        // A negative rank searches backwards from the tail.
        std::int64_t skipped = 0;
        for (std::int64_t i = static_cast<std::int64_t>(items.size()) - 1; i >= 0; --i) {
            if (items[static_cast<std::size_t>(i)] != ctx.arg(2)) continue;
            if (++skipped < -rank) continue;
            matches.push_back(i);
            if (want_multiple && static_cast<std::int64_t>(matches.size()) >= wanted) break;
            if (!want_multiple) break;
        }
    }

    if (want_multiple) {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(matches.size()));
        for (std::int64_t m : matches) ctx.reply.integer(m);
    } else if (matches.empty()) {
        ctx.reply.nullBulk();
    } else {
        ctx.reply.integer(matches[0]);
    }
}

void rpoplpush(CommandContext& ctx) {
    moveGeneric(ctx, ctx.arg(1), ctx.arg(2), /*from_front=*/false, /*to_front=*/true);
}

void lmove(CommandContext& ctx) {
    const bool from_front = equalsIgnoreCase(ctx.arg(3), "LEFT");
    const bool to_front   = equalsIgnoreCase(ctx.arg(4), "LEFT");
    if ((!from_front && !equalsIgnoreCase(ctx.arg(3), "RIGHT")) ||
        (!to_front && !equalsIgnoreCase(ctx.arg(4), "RIGHT"))) {
        replies::syntaxError(ctx.reply);
        return;
    }
    moveGeneric(ctx, ctx.arg(1), ctx.arg(2), from_front, to_front);
}

}  // namespace mnemos::server::cmd
