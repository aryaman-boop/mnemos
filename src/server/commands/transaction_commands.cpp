// MULTI, EXEC, DISCARD, WATCH, UNWATCH.
//
// A transaction here is not a rollback log. Redis queues the commands, then runs
// them back to back on the single event-loop thread, which is what makes them
// atomic: nothing else can be interleaved because nothing else is running. The
// consequence is the part people are surprised by -- a command that fails inside
// EXEC fails alone. The ones after it still run, and the ones before it stay
// done. There is nothing to undo to.
//
// WATCH is what makes that usable. It is a check-and-set: the connection
// registers an interest in some keys, and if any of them is touched by anyone
// (including by the watching connection itself) before EXEC, the transaction is
// abandoned and the client is told to retry. The check happens once, at EXEC,
// against a flag the write path set -- no key is compared, and no lock is held.
#include <string>
#include <utility>
#include <vector>

#include "server/command_table.h"
#include "server/commands/commands.h"
#include "server/server.h"

namespace mnemos::server::cmd {

void multi(CommandContext& ctx) {
    if (ctx.client.inMulti()) {
        // Not an abort: the transaction opened by the first MULTI is untouched
        // and can still be EXECed.
        ctx.reply.error("ERR MULTI calls can not be nested");
        return;
    }
    ctx.client.beginMulti();
    replies::ok(ctx.reply);
}

void discard(CommandContext& ctx) {
    if (!ctx.client.inMulti()) {
        ctx.reply.error("ERR DISCARD without MULTI");
        return;
    }
    ctx.server.discardTransaction(ctx.client);
    replies::ok(ctx.reply);
}

void exec(CommandContext& ctx) {
    Client& client = ctx.client;

    if (!client.inMulti()) {
        ctx.reply.error("ERR EXEC without MULTI");
        return;
    }

    // A command that could not be queued poisons the whole transaction, and the
    // client is told why. This is the one place where Redis refuses to run a
    // transaction on grounds of something it noticed earlier.
    if (client.multiError()) {
        ctx.server.discardTransaction(client);
        ctx.reply.error("EXECABORT Transaction discarded because of previous errors.");
        return;
    }

    // A watched key moved. Not an error -- the client asked to be told, and the
    // null reply is the answer to "did my assumption hold?".
    if (client.dirtyCas()) {
        ctx.server.discardTransaction(client);
        ctx.reply.nullArray();
        return;
    }

    // Moved out before anything runs, so a handler cannot see a half-consumed
    // queue -- and so the transaction is fully ended before its first command,
    // which is what lets a queued command touch the client's own state.
    const std::vector<std::vector<std::string>> queued = std::move(client.multiQueue());
    ctx.server.discardTransaction(client);

    ctx.reply.arrayHeader(static_cast<std::int64_t>(queued.size()));
    for (const std::vector<std::string>& argv : queued) {
        // Non-null by construction: queueing rejected anything that did not
        // resolve to a command, and the table is immutable at runtime.
        const CommandSpec* spec = lookupCommand(argv[0]);
        ctx.server.callCommand(client, *spec, argv, ctx.reply);
    }
}

void watch(CommandContext& ctx) {
    if (ctx.client.inMulti()) {
        // The point of a watch is to be established *before* the transaction it
        // guards; one added afterwards could only ever check keys the client has
        // not read yet.
        ctx.reply.error("ERR WATCH inside MULTI is not allowed");
        return;
    }
    for (std::size_t i = 1; i < ctx.argc(); ++i) {
        ctx.server.watchKey(ctx.client, ctx.arg(i));
    }
    replies::ok(ctx.reply);
}

void unwatch(CommandContext& ctx) {
    // Unconditionally fine, even with nothing watched and outside MULTI: it is
    // the "never mind" a client sends when it decides not to run the
    // transaction it was preparing.
    ctx.server.unwatchAllKeys(ctx.client);
    ctx.client.clearDirtyCas();
    replies::ok(ctx.reply);
}

}  // namespace mnemos::server::cmd
