// Set commands, including the multi-key set algebra.
#include <algorithm>
#include <random>
#include <unordered_set>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/commands/type_helpers.h"
#include "server/notify.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;
using core::SetValue;
using core::stringToInt64;

namespace {

enum class SetOp { Intersect, Union, Difference };

// Collects the members of each named key. Returns false after emitting
// WRONGTYPE if any key holds a non-set. A missing key contributes an empty set,
// which is why SINTER with a nonexistent key is empty rather than an error.
bool gatherSets(CommandContext& ctx, std::size_t first, std::size_t last,
                std::vector<std::vector<std::string>>& out) {
    for (std::size_t i = first; i <= last && i < ctx.argc(); ++i) {
        bool type_error = false;
        Value* value = lookupTypedRead(ctx, ctx.arg(i), ObjType::Set, type_error);
        if (type_error) return false;
        out.push_back(value ? value->set()->members() : std::vector<std::string>{});
    }
    return true;
}

std::vector<std::string> applySetOp(SetOp op, std::vector<std::vector<std::string>>& sets,
                                    std::size_t limit) {
    std::vector<std::string> result;
    if (sets.empty()) return result;

    if (op == SetOp::Intersect) {
        // Start from the smallest set: the result cannot be larger than it, so
        // this bounds the number of membership probes.
        auto smallest = std::min_element(
            sets.begin(), sets.end(),
            [](const auto& a, const auto& b) { return a.size() < b.size(); });
        std::vector<std::unordered_set<std::string>> others;
        for (auto it = sets.begin(); it != sets.end(); ++it) {
            if (it == smallest) continue;
            others.emplace_back(it->begin(), it->end());
        }
        for (const std::string& member : *smallest) {
            bool in_all = true;
            for (const auto& other : others) {
                if (!other.count(member)) {
                    in_all = false;
                    break;
                }
            }
            if (in_all) {
                result.push_back(member);
                if (limit > 0 && result.size() >= limit) break;
            }
        }
        return result;
    }

    if (op == SetOp::Union) {
        std::unordered_set<std::string> seen;
        for (const auto& set : sets) {
            for (const std::string& member : set) {
                if (seen.insert(member).second) result.push_back(member);
            }
        }
        return result;
    }

    // Difference: everything in the first set that is in none of the rest.
    std::vector<std::unordered_set<std::string>> others;
    for (std::size_t i = 1; i < sets.size(); ++i) {
        others.emplace_back(sets[i].begin(), sets[i].end());
    }
    for (const std::string& member : sets[0]) {
        bool excluded = false;
        for (const auto& other : others) {
            if (other.count(member)) {
                excluded = true;
                break;
            }
        }
        if (!excluded) result.push_back(member);
    }
    return result;
}

// The STORE variants name their event after themselves, not after the
// operation: a subscriber sees "sinterstore", not "sadd".
std::string_view storeEventName(SetOp op) {
    switch (op) {
        case SetOp::Intersect:  return "sinterstore";
        case SetOp::Union:      return "sunionstore";
        case SetOp::Difference: return "sdiffstore";
    }
    return "";
}

void setOpCommand(CommandContext& ctx, SetOp op, bool store) {
    const std::size_t first_key = store ? 2 : 1;
    std::vector<std::vector<std::string>> sets;
    if (!gatherSets(ctx, first_key, ctx.argc() - 1, sets)) return;

    std::vector<std::string> result = applySetOp(op, sets, 0);

    if (!store) {
        ctx.reply.setHeader(static_cast<std::int64_t>(result.size()));
        for (const std::string& member : result) ctx.reply.bulk(member);
        return;
    }

    const std::string& dst = ctx.arg(1);
    if (!result.empty()) {
        // setKey rather than erase-then-create: it drops the old TTL the same
        // way, but only announces `new` when the destination really is new.
        ctx.db.setKey(dst, Value::makeSet());
        Value* destination = ctx.db.lookupWrite(dst, ctx.nowMs());
        for (const std::string& member : result) destination->set()->add(member);
        notifyKeyspaceEvent(ctx, notify::kSet, storeEventName(op), dst);
    } else if (ctx.db.erase(dst)) {
        // An empty result is a delete, and that is the only event it raises.
        notifyKeyspaceEvent(ctx, notify::kGeneric, "del", dst);
    }
    ctx.server.markDirty();
    ctx.reply.integer(static_cast<std::int64_t>(result.size()));
}

}  // namespace

void sadd(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupOrCreate(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error || !value) return;

    std::int64_t added = 0;
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        if (value->set()->add(ctx.arg(i))) ++added;
    }
    if (added > 0) {
        ctx.server.markDirty(static_cast<std::uint64_t>(added));
        notifyKeyspaceEvent(ctx, notify::kSet, "sadd", ctx.arg(1));
    }
    ctx.reply.integer(added);
}

void srem(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.integer(0);
        return;
    }
    std::int64_t removed = 0;
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        if (value->set()->erase(ctx.arg(i))) ++removed;
    }
    if (removed > 0) {
        ctx.server.markDirty(static_cast<std::uint64_t>(removed));
        notifyKeyspaceEvent(ctx, notify::kSet, "srem", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);
    ctx.reply.integer(removed);
}

void scard(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;
    ctx.reply.integer(value ? static_cast<std::int64_t>(value->set()->size()) : 0);
}

void sismember(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;
    ctx.reply.integer(value && value->set()->contains(ctx.arg(2)) ? 1 : 0);
}

void smismember(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;

    ctx.reply.arrayHeader(static_cast<std::int64_t>(ctx.argc() - 2));
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        ctx.reply.integer(value && value->set()->contains(ctx.arg(i)) ? 1 : 0);
    }
}

void smembers(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;
    if (!value) {
        ctx.reply.setHeader(0);
        return;
    }
    const std::vector<std::string> members = value->set()->members();
    ctx.reply.setHeader(static_cast<std::int64_t>(members.size()));
    for (const std::string& member : members) ctx.reply.bulk(member);
}

void spop(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTyped(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;

    bool         has_count = ctx.argc() > 2;
    std::int64_t count     = 1;
    if (has_count) {
        if (!stringToInt64(ctx.arg(2), count) || count < 0) {
            ctx.reply.error("ERR value is out of range, must be positive");
            return;
        }
    }

    if (!value || value->set()->size() == 0) {
        if (has_count) ctx.reply.setHeader(0);
        else           ctx.reply.nullBulk();
        return;
    }

    std::vector<std::string> popped;
    for (std::int64_t i = 0; i < count; ++i) {
        auto member = value->set()->randomMember();
        if (!member) break;
        value->set()->erase(*member);
        popped.push_back(std::move(*member));
    }

    if (!popped.empty()) {
        ctx.server.markDirty(popped.size());
        notifyKeyspaceEvent(ctx, notify::kSet, "spop", ctx.arg(1));
    }
    deleteIfEmpty(ctx, ctx.arg(1), *value);

    if (has_count) {
        ctx.reply.setHeader(static_cast<std::int64_t>(popped.size()));
        for (const std::string& member : popped) ctx.reply.bulk(member);
    } else {
        ctx.reply.bulk(popped[0]);
    }
}

void srandmember(CommandContext& ctx) {
    bool type_error = false;
    Value* value = lookupTypedRead(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;

    bool         has_count = ctx.argc() > 2;
    std::int64_t count     = 1;
    if (has_count && !stringToInt64(ctx.arg(2), count)) {
        replies::notAnInteger(ctx.reply);
        return;
    }

    if (!value || value->set()->size() == 0) {
        if (has_count) ctx.reply.arrayHeader(0);
        else           ctx.reply.nullBulk();
        return;
    }

    const std::vector<std::string> members = value->set()->members();
    static thread_local std::mt19937_64 gen{std::random_device{}()};

    if (!has_count) {
        ctx.reply.bulk(members[gen() % members.size()]);
        return;
    }

    std::vector<std::string> picked;
    if (count < 0) {
        // Negative count: repeats allowed, exactly |count| returned.
        const auto wanted = static_cast<std::size_t>(-count);
        for (std::size_t i = 0; i < wanted; ++i) picked.push_back(members[gen() % members.size()]);
    } else {
        std::vector<std::string> shuffled = members;
        std::shuffle(shuffled.begin(), shuffled.end(), gen);
        const auto wanted = std::min<std::size_t>(static_cast<std::size_t>(count), shuffled.size());
        picked.assign(shuffled.begin(), shuffled.begin() + static_cast<std::ptrdiff_t>(wanted));
    }
    ctx.reply.bulkArray(picked);
}

void smove(CommandContext& ctx) {
    bool type_error = false;
    Value* source = lookupTyped(ctx, ctx.arg(1), ObjType::Set, type_error);
    if (type_error) return;
    // Validate the destination's type before mutating the source.
    Value* destination = lookupTyped(ctx, ctx.arg(2), ObjType::Set, type_error);
    if (type_error) return;
    (void)destination;

    if (!source || !source->set()->contains(ctx.arg(3))) {
        ctx.reply.integer(0);
        return;
    }
    source->set()->erase(ctx.arg(3));
    notifyKeyspaceEvent(ctx, notify::kSet, "srem", ctx.arg(1));
    deleteIfEmpty(ctx, ctx.arg(1), *source);

    Value* target = lookupOrCreate(ctx, ctx.arg(2), ObjType::Set, type_error);
    if (type_error || !target) return;
    target->set()->add(ctx.arg(3));
    notifyKeyspaceEvent(ctx, notify::kSet, "sadd", ctx.arg(2));

    ctx.server.markDirty();
    ctx.reply.integer(1);
}

void sinter(CommandContext& ctx) { setOpCommand(ctx, SetOp::Intersect, false); }
void sunion(CommandContext& ctx) { setOpCommand(ctx, SetOp::Union, false); }
void sdiff(CommandContext& ctx)  { setOpCommand(ctx, SetOp::Difference, false); }
void sinterstore(CommandContext& ctx) { setOpCommand(ctx, SetOp::Intersect, true); }
void sunionstore(CommandContext& ctx) { setOpCommand(ctx, SetOp::Union, true); }
void sdiffstore(CommandContext& ctx)  { setOpCommand(ctx, SetOp::Difference, true); }

void sintercard(CommandContext& ctx) {
    std::int64_t numkeys = 0;
    if (!stringToInt64(ctx.arg(1), numkeys) || numkeys <= 0) {
        ctx.reply.error("ERR numkeys should be greater than 0");
        return;
    }
    if (static_cast<std::size_t>(numkeys) + 2 > ctx.argc()) {
        ctx.reply.error("ERR Number of keys can't be greater than number of args");
        return;
    }

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

    std::vector<std::vector<std::string>> sets;
    if (!gatherSets(ctx, 2, after_keys - 1, sets)) return;
    // SINTERCARD only needs the count, and LIMIT lets it stop early rather than
    // materialising an intersection the caller will not read.
    ctx.reply.integer(static_cast<std::int64_t>(applySetOp(SetOp::Intersect, sets, limit).size()));
}

}  // namespace mnemos::server::cmd
