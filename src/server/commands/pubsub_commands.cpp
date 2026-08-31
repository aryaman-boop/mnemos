// SUBSCRIBE, PUBLISH and friends.
//
// Pub/sub is the one place where a command writes to a connection other than
// the one that issued it, so every reply here is built against the *recipient's*
// protocol version rather than the publisher's: the same message goes out as a
// plain array to a RESP2 subscriber and as a tagged push frame to a RESP3 one.
#include <string>
#include <vector>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/server.h"

namespace mnemos::server::cmd {

namespace {

// A subscribe/unsubscribe confirmation: kind, what it applied to, and the
// client's remaining subscription count.
void confirm(CommandContext& ctx, std::string_view kind,
             const std::string* target, std::size_t count) {
    ctx.reply.pushHeader(3);
    ctx.reply.bulk(kind);
    if (target) ctx.reply.bulk(*target);
    else        ctx.reply.nullBulk();  // UNSUBSCRIBE with nothing to unsubscribe
    ctx.reply.integer(static_cast<std::int64_t>(count));
}

// Encodes one message for a single recipient and hands it to the server.
void deliver(Server& server, int fd, std::string_view kind,
             const std::string* pattern, const std::string& channel,
             const std::string& payload) {
    Client* recipient = server.clientByFd(fd);
    if (!recipient) return;

    std::string      frame;
    net::ReplyWriter writer(frame, recipient->protocolVersion());
    writer.pushHeader(pattern ? 4 : 3);
    writer.bulk(kind);
    if (pattern) writer.bulk(*pattern);
    writer.bulk(channel);
    writer.bulk(payload);
    server.queueWrite(fd, frame);
}

void subscribeTo(CommandContext& ctx, bool by_pattern) {
    Client& client = ctx.client;
    for (std::size_t i = 1; i < ctx.argc(); ++i) {
        const std::string& name = ctx.arg(i);
        if (by_pattern) {
            if (client.patterns().insert(name).second) {
                ctx.server.pubsub().subscribePattern(name, client.fd());
            }
        } else {
            if (client.channels().insert(name).second) {
                ctx.server.pubsub().subscribeChannel(name, client.fd());
            }
        }
        // Redis confirms every name given, including one already subscribed.
        confirm(ctx, by_pattern ? "psubscribe" : "subscribe", &name,
                client.subscriptionCount());
    }
}

void unsubscribeFrom(CommandContext& ctx, bool by_pattern) {
    Client&                 client = ctx.client;
    std::set<std::string>&  held   = by_pattern ? client.patterns() : client.channels();
    const std::string_view  kind   = by_pattern ? "punsubscribe" : "unsubscribe";

    std::vector<std::string> targets;
    if (ctx.argc() > 1) {
        for (std::size_t i = 1; i < ctx.argc(); ++i) targets.push_back(ctx.arg(i));
    } else {
        targets.assign(held.begin(), held.end());
        // Nothing to drop still owes the client one frame, with a null name --
        // otherwise a client waiting for confirmation would hang forever.
        if (targets.empty()) {
            confirm(ctx, kind, nullptr, client.subscriptionCount());
            return;
        }
    }

    for (const std::string& name : targets) {
        if (held.erase(name) > 0) {
            if (by_pattern) ctx.server.pubsub().unsubscribePattern(name, client.fd());
            else            ctx.server.pubsub().unsubscribeChannel(name, client.fd());
        }
        confirm(ctx, kind, &name, client.subscriptionCount());
    }
}

}  // namespace

void subscribe(CommandContext& ctx)    { subscribeTo(ctx, false); }
void psubscribe(CommandContext& ctx)   { subscribeTo(ctx, true); }
void unsubscribe(CommandContext& ctx)  { unsubscribeFrom(ctx, false); }
void punsubscribe(CommandContext& ctx) { unsubscribeFrom(ctx, true); }

void publish(CommandContext& ctx) {
    const std::string& channel = ctx.arg(1);
    const std::string& payload = ctx.arg(2);

    std::int64_t receivers = 0;

    // Copied before delivery: writing to a peer can close it, and a closed peer
    // is unsubscribed, which would invalidate an iterator over the live set.
    std::vector<int> targets;
    if (const std::set<int>* subscribers = ctx.server.pubsub().channelSubscribers(channel)) {
        targets.assign(subscribers->begin(), subscribers->end());
    }
    for (int fd : targets) {
        deliver(ctx.server, fd, "message", nullptr, channel, payload);
        ++receivers;
    }

    // No index for patterns: every one of them is tested against the channel.
    std::vector<std::pair<std::string, std::vector<int>>> pattern_targets;
    for (const auto& [pattern, subscribers] : ctx.server.pubsub().patterns()) {
        if (!core::globMatch(pattern, channel)) continue;
        pattern_targets.emplace_back(pattern,
                                     std::vector<int>(subscribers.begin(), subscribers.end()));
    }
    for (const auto& [pattern, fds] : pattern_targets) {
        for (int fd : fds) {
            deliver(ctx.server, fd, "pmessage", &pattern, channel, payload);
            ++receivers;
        }
    }

    ctx.reply.integer(receivers);
}

void pubsub(CommandContext& ctx) {
    const std::string sub = core::toLower(ctx.arg(1));

    if (sub == "channels" && ctx.argc() <= 3) {
        const std::string* pattern = ctx.argc() == 3 ? &ctx.arg(2) : nullptr;
        std::vector<std::string> names;
        for (std::string& name : ctx.server.pubsub().channelNames()) {
            if (pattern && !core::globMatch(*pattern, name)) continue;
            names.push_back(std::move(name));
        }
        ctx.reply.bulkArray(names);
        return;
    }

    if (sub == "numsub") {
        // A flat channel/count array, not a map: Redis kept this shape even in
        // RESP3, so mirroring it matters more than the tidier encoding would.
        ctx.reply.arrayHeader(static_cast<std::int64_t>((ctx.argc() - 2) * 2));
        for (std::size_t i = 2; i < ctx.argc(); ++i) {
            ctx.reply.bulk(ctx.arg(i));
            ctx.reply.integer(
                static_cast<std::int64_t>(ctx.server.pubsub().channelSubscriberCount(ctx.arg(i))));
        }
        return;
    }

    if (sub == "numpat") {
        // NUMPAT takes no arguments at all, so Redis rejects extras through the
        // ordinary arity path -- naming the subcommand, not the container.
        if (ctx.argc() != 2) {
            replies::wrongArgs(ctx.reply, "pubsub|numpat");
            return;
        }
        ctx.reply.integer(static_cast<std::int64_t>(ctx.server.pubsub().patternCount()));
        return;
    }

    // CHANNELS exists but was handed too many patterns; anything else is a
    // subcommand this server has never heard of.
    if (sub == "channels") replies::subcommandSyntaxError(ctx.reply, "PUBSUB", ctx.arg(1));
    else                   replies::unknownSubcommand(ctx.reply, "PUBSUB", ctx.arg(1));
}

}  // namespace mnemos::server::cmd
