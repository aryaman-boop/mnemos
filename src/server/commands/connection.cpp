// Connection lifecycle: handshake, protocol negotiation, auth, per-client state.
#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;

void ping(CommandContext& ctx) {
    if (ctx.argc() > 2) {
        replies::wrongArgs(ctx.reply, "ping");
        return;
    }
    // PING with an argument echoes it as a bulk string; bare PING is the cheaper
    // simple-string +PONG, which is why health checks use the bare form.
    if (ctx.argc() == 2) ctx.reply.bulk(ctx.arg(1));
    else                 ctx.reply.simpleString("PONG");
}

void echo(CommandContext& ctx) { ctx.reply.bulk(ctx.arg(1)); }

void select(CommandContext& ctx) {
    std::int64_t index = 0;
    if (!core::stringToInt64(ctx.arg(1), index)) {
        ctx.reply.error("ERR value is not an integer or out of range");
        return;
    }
    if (index < 0 || index >= static_cast<std::int64_t>(ctx.server.databaseCount())) {
        ctx.reply.error("ERR DB index is out of range");
        return;
    }
    ctx.client.setDbIndex(static_cast<int>(index));
    replies::ok(ctx.reply);
}

void hello(CommandContext& ctx) {
    // HELLO [protover [AUTH user pass] [SETNAME name]]
    int requested = ctx.client.protocolVersion();

    if (ctx.argc() >= 2) {
        std::int64_t version = 0;
        if (!core::stringToInt64(ctx.arg(1), version) || (version != 2 && version != 3)) {
            ctx.reply.error(
                "NOPROTO unsupported protocol version");
            return;
        }
        requested = static_cast<int>(version);

        for (std::size_t i = 2; i < ctx.argc();) {
            if (equalsIgnoreCase(ctx.arg(i), "AUTH") && i + 2 < ctx.argc()) {
                const std::string& password = ctx.arg(i + 2);
                if (ctx.server.config().requirepass.empty() ||
                    password == ctx.server.config().requirepass) {
                    ctx.client.setAuthenticated(true);
                } else {
                    ctx.reply.error("WRONGPASS invalid username-password pair or user is disabled.");
                    return;
                }
                i += 3;
            } else if (equalsIgnoreCase(ctx.arg(i), "SETNAME") && i + 1 < ctx.argc()) {
                ctx.client.setName(ctx.arg(i + 1));
                i += 2;
            } else {
                ctx.reply.error("ERR Protocol error: unsupported HELLO option");
                return;
            }
        }
    }

    if (!ctx.server.config().requirepass.empty() && !ctx.client.authenticated()) {
        ctx.reply.error("NOAUTH HELLO must be called with the client already authenticated, "
                        "otherwise the HELLO <proto> AUTH <user> <pass> option can be used to "
                        "authenticate the client and select the RESP protocol version at the "
                        "same time");
        return;
    }

    // Switch the client over *before* writing the reply, so the handshake reply
    // is already encoded in the protocol version the client just asked for.
    ctx.client.setProtocolVersion(requested);

    net::ReplyWriter writer(ctx.client.outputBuffer(), requested);
    writer.mapHeader(7);
    writer.bulk("server");  writer.bulk("mnemos");
    writer.bulk("version"); writer.bulk("0.1.0");
    writer.bulk("proto");   writer.integer(requested);
    writer.bulk("id");      writer.integer(static_cast<std::int64_t>(ctx.client.id()));
    writer.bulk("mode");    writer.bulk("standalone");
    writer.bulk("role");    writer.bulk("master");
    writer.bulk("modules"); writer.arrayHeader(0);
}

void auth(CommandContext& ctx) {
    if (ctx.argc() > 3) {
        replies::wrongArgs(ctx.reply, "auth");
        return;
    }
    if (ctx.server.config().requirepass.empty()) {
        ctx.reply.error("ERR Client sent AUTH, but no password is set. Did you mean AUTH "
                        "<username> <password>?");
        return;
    }
    // AUTH <pass> and AUTH <user> <pass> both land here; only the password
    // matters until ACL users exist.
    const std::string& password = ctx.arg(ctx.argc() - 1);
    if (password == ctx.server.config().requirepass) {
        ctx.client.setAuthenticated(true);
        replies::ok(ctx.reply);
    } else {
        ctx.reply.error("WRONGPASS invalid username-password pair or user is disabled.");
    }
}

void quit(CommandContext& ctx) {
    replies::ok(ctx.reply);
    ctx.client.closeAfterReply();
}

void reset(CommandContext& ctx) {
    ctx.client.setDbIndex(0);
    ctx.client.setProtocolVersion(2);
    ctx.client.setName("");
    ctx.client.setAuthenticated(ctx.server.config().requirepass.empty());
    ctx.reply.simpleString("RESET");
}

namespace {

std::string describeClient(const Client& c, std::int64_t now_ms, int db_index) {
    std::string out;
    out += "id=" + std::to_string(c.id());
    out += " addr=" + c.addr();
    out += " name=" + c.name();
    out += " age=" + std::to_string((now_ms - c.createdAtMs()) / 1000);
    out += " idle=" + std::to_string((now_ms - c.lastInteractionMs()) / 1000);
    out += " db=" + std::to_string(db_index);
    out += " resp=" + std::to_string(c.protocolVersion());
    out += " cmd=" + (c.lastCommand().empty() ? std::string("NULL") : c.lastCommand());
    return out;
}

}  // namespace

void client(CommandContext& ctx) {
    const std::string sub = core::toUpper(ctx.arg(1));

    if (sub == "ID") {
        ctx.reply.integer(static_cast<std::int64_t>(ctx.client.id()));
        return;
    }
    if (sub == "GETNAME") {
        if (ctx.client.name().empty()) ctx.reply.nullBulk();
        else                           ctx.reply.bulk(ctx.client.name());
        return;
    }
    if (sub == "SETNAME") {
        if (ctx.argc() != 3) {
            replies::wrongArgs(ctx.reply, "client|setname");
            return;
        }
        // Spaces and newlines would corrupt the CLIENT LIST line format.
        if (ctx.arg(2).find_first_of(" \n\r") != std::string::npos) {
            ctx.reply.error("ERR Client names cannot contain spaces, newlines or special characters.");
            return;
        }
        ctx.client.setName(ctx.arg(2));
        replies::ok(ctx.reply);
        return;
    }
    if (sub == "SETINFO") {
        // Modern redis-cli announces its library name/version on connect. We
        // accept and ignore it rather than erroring, so the handshake is clean.
        replies::ok(ctx.reply);
        return;
    }
    if (sub == "LIST") {
        std::string out;
        for (const auto& [fd, c] : ctx.server.clients()) {
            out += describeClient(*c, ctx.nowMs(), c->dbIndex());
            out += "\n";
        }
        ctx.reply.bulk(out);
        return;
    }
    if (sub == "INFO") {
        ctx.reply.bulk(describeClient(ctx.client, ctx.nowMs(), ctx.client.dbIndex()));
        return;
    }
    if (sub == "NO-EVICT" || sub == "NO-TOUCH") {
        replies::ok(ctx.reply);
        return;
    }

    ctx.reply.error("ERR Unknown CLIENT subcommand or wrong number of arguments for '" +
                    ctx.arg(1) + "'");
}

}  // namespace mnemos::server::cmd
