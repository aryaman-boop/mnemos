// SUBSCRIBE, PUBLISH and friends.
//
// The subscription bookkeeping lives here; delivery itself is Server::publish-
// Message, because keyspace notifications publish through exactly the same path
// without going near a command handler.
#include <string>
#include <vector>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/server.h"

namespace mnemos::server::cmd {

namespace {

// The three subscription namespaces. They differ only in the set the client
// holds them in, the word in the confirmation, and the count it reports.
enum class SubKind { Channel, Pattern, Shard };

std::set<std::string>& heldBy(Client& client, SubKind kind) {
    switch (kind) {
        case SubKind::Pattern: return client.patterns();
        case SubKind::Shard:   return client.shardChannels();
        case SubKind::Channel: break;
    }
    return client.channels();
}

// Shard subscriptions are counted on their own, so an SSUBSCRIBE reports how
// many shard channels the client holds and says nothing about its ordinary
// subscriptions -- and vice versa.
std::size_t reportedCount(const Client& client, SubKind kind) {
    return kind == SubKind::Shard ? client.shardChannels().size()
                                  : client.subscriptionCount();
}

std::string_view confirmKind(SubKind kind, bool subscribing) {
    switch (kind) {
        case SubKind::Pattern: return subscribing ? "psubscribe" : "punsubscribe";
        case SubKind::Shard:   return subscribing ? "ssubscribe" : "sunsubscribe";
        case SubKind::Channel: break;
    }
    return subscribing ? "subscribe" : "unsubscribe";
}

void updateRegistry(CommandContext& ctx, SubKind kind, const std::string& name,
                    bool subscribing) {
    PubSub&   pubsub = ctx.server.pubsub();
    const int fd     = ctx.client.fd();
    if (kind == SubKind::Pattern) {
        if (subscribing) pubsub.subscribePattern(name, fd);
        else             pubsub.unsubscribePattern(name, fd);
        return;
    }
    const ChannelKind channel_kind =
        kind == SubKind::Shard ? ChannelKind::Shard : ChannelKind::Global;
    if (subscribing) pubsub.subscribeChannel(channel_kind, name, fd);
    else             pubsub.unsubscribeChannel(channel_kind, name, fd);
}

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

void subscribeTo(CommandContext& ctx, SubKind kind) {
    Client& client = ctx.client;
    for (std::size_t i = 1; i < ctx.argc(); ++i) {
        const std::string& name = ctx.arg(i);
        if (heldBy(client, kind).insert(name).second) {
            updateRegistry(ctx, kind, name, true);
        }
        // Redis confirms every name given, including one already subscribed.
        confirm(ctx, confirmKind(kind, true), &name, reportedCount(client, kind));
    }
}

void unsubscribeFrom(CommandContext& ctx, SubKind kind) {
    Client&                client = ctx.client;
    std::set<std::string>& held   = heldBy(client, kind);
    const std::string_view word   = confirmKind(kind, false);

    std::vector<std::string> targets;
    if (ctx.argc() > 1) {
        for (std::size_t i = 1; i < ctx.argc(); ++i) targets.push_back(ctx.arg(i));
    } else {
        targets.assign(held.begin(), held.end());
        // Nothing to drop still owes the client one frame, with a null name --
        // otherwise a client waiting for confirmation would hang forever.
        if (targets.empty()) {
            confirm(ctx, word, nullptr, reportedCount(client, kind));
            return;
        }
    }

    for (const std::string& name : targets) {
        if (held.erase(name) > 0) updateRegistry(ctx, kind, name, false);
        confirm(ctx, word, &name, reportedCount(client, kind));
    }
}

// PUBSUB CHANNELS and PUBSUB SHARDCHANNELS differ only in which namespace they
// enumerate; the same holds for NUMSUB and SHARDNUMSUB.
bool listChannels(CommandContext& ctx, ChannelKind kind) {
    if (ctx.argc() > 3) return false;
    const std::string* pattern = ctx.argc() == 3 ? &ctx.arg(2) : nullptr;
    std::vector<std::string> names;
    for (std::string& name : ctx.server.pubsub().channelNames(kind)) {
        if (pattern && !core::globMatch(*pattern, name)) continue;
        names.push_back(std::move(name));
    }
    ctx.reply.bulkArray(names);
    return true;
}

void numSub(CommandContext& ctx, ChannelKind kind) {
    // A flat channel/count array, not a map: Redis kept this shape even in
    // RESP3, so mirroring it matters more than the tidier encoding would.
    ctx.reply.arrayHeader(static_cast<std::int64_t>((ctx.argc() - 2) * 2));
    for (std::size_t i = 2; i < ctx.argc(); ++i) {
        ctx.reply.bulk(ctx.arg(i));
        ctx.reply.integer(static_cast<std::int64_t>(
            ctx.server.pubsub().channelSubscriberCount(kind, ctx.arg(i))));
    }
}

}  // namespace

void subscribe(CommandContext& ctx)     { subscribeTo(ctx, SubKind::Channel); }
void psubscribe(CommandContext& ctx)    { subscribeTo(ctx, SubKind::Pattern); }
void ssubscribe(CommandContext& ctx)    { subscribeTo(ctx, SubKind::Shard); }
void unsubscribe(CommandContext& ctx)   { unsubscribeFrom(ctx, SubKind::Channel); }
void punsubscribe(CommandContext& ctx)  { unsubscribeFrom(ctx, SubKind::Pattern); }
void sunsubscribe(CommandContext& ctx)  { unsubscribeFrom(ctx, SubKind::Shard); }

void publish(CommandContext& ctx) {
    ctx.reply.integer(ctx.server.publishMessage(ctx.arg(1), ctx.arg(2)));
}

// SPUBLISH reaches shard subscribers only. Patterns are deliberately not
// consulted: in a cluster a pattern subscription lives on every node, so
// matching one here would defeat the point of routing by slot.
void spublish(CommandContext& ctx) {
    ctx.reply.integer(ctx.server.publishShardMessage(ctx.arg(1), ctx.arg(2)));
}

void pubsub(CommandContext& ctx) {
    const std::string sub = core::toLower(ctx.arg(1));

    const bool is_channels      = sub == "channels";
    const bool is_shardchannels = sub == "shardchannels";
    if (is_channels || is_shardchannels) {
        const ChannelKind kind = is_shardchannels ? ChannelKind::Shard : ChannelKind::Global;
        if (listChannels(ctx, kind)) return;
        // The subcommand exists but was handed too many patterns.
        replies::subcommandSyntaxError(ctx.reply, "PUBSUB", ctx.arg(1));
        return;
    }

    if (sub == "numsub" || sub == "shardnumsub") {
        numSub(ctx, sub == "shardnumsub" ? ChannelKind::Shard : ChannelKind::Global);
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

    replies::unknownSubcommand(ctx.reply, "PUBSUB", ctx.arg(1));
}

}  // namespace mnemos::server::cmd
