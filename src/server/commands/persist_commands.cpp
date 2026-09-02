// Snapshot commands: DUMP and RESTORE, which move one value, and SAVE/BGSAVE,
// which move the whole keyspace.
#include "core/encoding.h"
#include "core/strings.h"
#include "persist/rdb.h"
#include "server/commands/commands.h"
#include "server/notify.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;
using core::stringToInt64;
using core::Value;

void dump(CommandContext& ctx) {
    Value* value = ctx.db.lookupRead(ctx.arg(1), ctx.nowMs());
    if (!value) {
        notifyKeyMiss(ctx, ctx.arg(1));
        ctx.reply.nullBulk();
        return;
    }
    ctx.reply.bulk(persist::dumpPayload(*value));
}

void restore(CommandContext& ctx) {
    const std::string& key = ctx.arg(1);

    // Options first, and their errors before anything else is even looked at:
    // Redis parses the tail of the command before it decides whether the key is
    // in the way, so a syntax error beats BUSYKEY beats a bad TTL.
    bool         replace  = false;
    bool         absttl   = false;
    std::int64_t scratch  = 0;
    for (std::size_t i = 4; i < ctx.argc(); ++i) {
        const std::string& opt = ctx.arg(i);
        if (equalsIgnoreCase(opt, "REPLACE")) {
            replace = true;
        } else if (equalsIgnoreCase(opt, "ABSTTL")) {
            absttl = true;
        } else if (equalsIgnoreCase(opt, "IDLETIME") && i + 1 < ctx.argc()) {
            if (!stringToInt64(ctx.arg(++i), scratch)) {
                replies::notAnInteger(ctx.reply);
                return;
            }
            // Accepted and discarded: mnemos has no maxmemory policy for it to
            // feed, and refusing the option would break clients that migrate.
            if (scratch < 0) {
                replies::invalidIdletime(ctx.reply);
                return;
            }
        } else if (equalsIgnoreCase(opt, "FREQ") && i + 1 < ctx.argc()) {
            if (!stringToInt64(ctx.arg(++i), scratch)) {
                replies::notAnInteger(ctx.reply);
                return;
            }
            if (scratch < 0 || scratch > 255) {
                replies::invalidFreq(ctx.reply);
                return;
            }
        } else {
            replies::syntaxError(ctx.reply);
            return;
        }
    }

    const bool exists = ctx.db.lookupWrite(key, ctx.nowMs()) != nullptr;
    if (exists && !replace) {
        replies::busyKey(ctx.reply);
        return;
    }

    std::int64_t ttl = 0;
    if (!stringToInt64(ctx.arg(2), ttl)) {
        replies::notAnInteger(ctx.reply);
        return;
    }
    if (ttl < 0) {
        replies::invalidRestoreTtl(ctx.reply);
        return;
    }

    const std::string& payload = ctx.arg(3);
    if (!persist::payloadFooterOk(payload)) {
        replies::badDumpPayload(ctx.reply);
        return;
    }
    Value value;
    if (!persist::loadPayload(payload, value)) {
        replies::badDataFormat(ctx.reply);
        return;
    }

    if (exists) ctx.db.erase(key);
    ctx.db.setKey(key, std::move(value));
    if (ttl > 0) {
        // Without ABSTTL the number is a duration; with it, it is already the
        // deadline the source server recorded.
        ctx.db.setExpireAt(key, absttl ? ttl : ctx.nowMs() + ttl);
    }
    ctx.server.markDirty();
    notifyKeyspaceEvent(ctx, notify::kGeneric, "restore", key);
    replies::ok(ctx.reply);
}

void save(CommandContext& ctx) {
    if (ctx.server.bgsaveInProgress()) {
        ctx.reply.error("ERR Background save already in progress");
        return;
    }
    std::string error;
    if (!ctx.server.saveRdb(error)) {
        ctx.reply.error("ERR " + error);
        return;
    }
    replies::ok(ctx.reply);
}

void bgsave(CommandContext& ctx) {
    // SCHEDULE only changes what happens when *another* kind of child is busy,
    // and mnemos has no other kind; either way an RDB child already running is
    // an error rather than a queued request.
    if (ctx.argc() > 2 || (ctx.argc() == 2 && !equalsIgnoreCase(ctx.arg(1), "SCHEDULE"))) {
        replies::syntaxError(ctx.reply);
        return;
    }
    std::string error;
    if (!ctx.server.startBackgroundSave(error)) {
        ctx.reply.error("ERR " + error);
        return;
    }
    ctx.reply.simpleString("Background saving started");
}

}  // namespace mnemos::server::cmd
