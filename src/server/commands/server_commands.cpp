// Administrative and introspection commands. INFO and OBJECT ENCODING are the
// two that matter most here: they are how you *prove* the internals behave the
// way you claim they do, from a plain redis-cli session.
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "core/strings.h"
#include "server/commands/commands.h"
#include "server/server.h"

namespace mnemos::server::cmd {

using core::equalsIgnoreCase;
using core::stringToInt64;
using core::Value;

namespace {

// Config values we can report. Anything a client library or benchmark tool is
// likely to probe on connect belongs here, so the handshake never errors.
std::vector<std::pair<std::string, std::string>> configEntries(const Server& server) {
    const Config& c = server.config();
    return {
        {"maxmemory",                   "0"},
        {"maxmemory-policy",            "noeviction"},
        {"appendonly",                  "no"},
        {"save",                        "3600 1 300 100 60 10000"},
        {"dir",                         c.dir},
        {"dbfilename",                  c.dbfilename},
        {"bind",                        c.bind_address},
        {"port",                        std::to_string(c.port)},
        {"databases",                   std::to_string(c.databases)},
        {"maxclients",                  std::to_string(c.max_clients)},
        {"requirepass",                 c.requirepass},
        {"proto-max-bulk-len",          "536870912"},
        {"timeout",                     "0"},
        {"tcp-keepalive",               "300"},
        {"hash-max-listpack-entries",   "512"},
        {"hash-max-listpack-value",     "64"},
        {"list-max-listpack-size",      "-2"},
        {"set-max-intset-entries",      "512"},
        {"set-max-listpack-entries",    "128"},
        {"zset-max-listpack-entries",   "128"},
        {"zset-max-listpack-value",     "64"},
    };
}

std::string buildInfo(Server& server, const std::string& section) {
    const bool all = section.empty() || section == "all" || section == "everything" ||
                     section == "default";
    std::string out;
    char buf[512];

    auto want = [&](const char* name) { return all || section == name; };

    if (want("server")) {
        const std::int64_t uptime_ms = server.nowMs() - server.startTimeMs();
        out += "# Server\r\n";
        out += "redis_version:7.4.0\r\n";  // claimed for client-library compatibility
        out += "mnemos_version:0.1.0\r\n";
        out += "redis_mode:standalone\r\n";
        out += "os:" + std::string(
#if defined(__APPLE__)
            "Darwin"
#elif defined(__linux__)
            "Linux"
#else
            "unknown"
#endif
        ) + "\r\n";
        out += "arch_bits:64\r\n";
        out += "process_id:" + std::to_string(::getpid()) + "\r\n";
        out += "run_id:" + server.runId() + "\r\n";
        out += "tcp_port:" + std::to_string(server.config().port) + "\r\n";
        out += "uptime_in_seconds:" + std::to_string(uptime_ms / 1000) + "\r\n";
        out += "uptime_in_days:" + std::to_string(uptime_ms / 1000 / 86400) + "\r\n";
        out += "\r\n";
    }

    if (want("clients")) {
        out += "# Clients\r\n";
        out += "connected_clients:" + std::to_string(server.clients().size()) + "\r\n";
        out += "maxclients:" + std::to_string(server.config().max_clients) + "\r\n";
        out += "\r\n";
    }

    if (want("stats")) {
        const Stats& s = server.stats();
        out += "# Stats\r\n";
        out += "total_connections_received:" + std::to_string(s.connections_received) + "\r\n";
        out += "total_commands_processed:" + std::to_string(s.commands_processed) + "\r\n";
        out += "total_net_input_bytes:" + std::to_string(s.total_net_input) + "\r\n";
        out += "total_net_output_bytes:" + std::to_string(s.total_net_output) + "\r\n";
        out += "rejected_connections:" + std::to_string(s.rejected_connections) + "\r\n";
        out += "expired_keys:" + std::to_string(s.expired_keys) + "\r\n";
        out += "keyspace_hits:" + std::to_string(s.keyspace_hits) + "\r\n";
        out += "keyspace_misses:" + std::to_string(s.keyspace_misses) + "\r\n";
        out += "\r\n";
    }

    if (want("replication")) {
        out += "# Replication\r\n";
        out += "role:master\r\n";
        out += "connected_slaves:0\r\n";
        out += "master_replid:" + server.runId() + "\r\n";
        out += "master_repl_offset:0\r\n";
        out += "\r\n";
    }

    if (want("persistence")) {
        out += "# Persistence\r\n";
        out += "loading:0\r\n";
        out += "rdb_changes_since_last_save:" + std::to_string(server.dirty()) + "\r\n";
        out += "rdb_bgsave_in_progress:0\r\n";
        out += "aof_enabled:0\r\n";
        out += "\r\n";
    }

    if (want("keyspace")) {
        out += "# Keyspace\r\n";
        for (std::size_t i = 0; i < server.databaseCount(); ++i) {
            Database& database = server.db(static_cast<int>(i));
            if (database.size() == 0) continue;
            std::snprintf(buf, sizeof(buf), "db%zu:keys=%zu,expires=%zu,avg_ttl=0\r\n", i,
                          database.size(), database.expiresSize());
            out += buf;
        }
        out += "\r\n";
    }

    return out;
}

}  // namespace

void dbsize(CommandContext& ctx) {
    ctx.reply.integer(static_cast<std::int64_t>(ctx.db.size()));
}

void flushdb(CommandContext& ctx) {
    // ASYNC / SYNC are accepted and ignored: we free inline, so both are honest.
    ctx.db.flush();
    ctx.server.markDirty();
    replies::ok(ctx.reply);
}

void flushall(CommandContext& ctx) {
    for (std::size_t i = 0; i < ctx.server.databaseCount(); ++i) {
        ctx.server.db(static_cast<int>(i)).flush();
    }
    ctx.server.markDirty();
    replies::ok(ctx.reply);
}

void info(CommandContext& ctx) {
    const std::string section = ctx.argc() > 1 ? core::toLower(ctx.arg(1)) : std::string();
    // INFO is a verbatim string in RESP3 so clients can render it as text.
    ctx.reply.verbatim(buildInfo(ctx.server, section), "txt");
}

void config(CommandContext& ctx) {
    const std::string sub = core::toUpper(ctx.arg(1));

    if (sub == "GET") {
        if (ctx.argc() < 3) {
            replies::wrongArgs(ctx.reply, "config|get");
            return;
        }
        std::vector<std::pair<std::string, std::string>> matched;
        for (const auto& [name, value] : configEntries(ctx.server)) {
            for (std::size_t i = 2; i < ctx.argc(); ++i) {
                if (core::globMatch(core::toLower(ctx.arg(i)), name)) {
                    matched.emplace_back(name, value);
                    break;
                }
            }
        }
        ctx.reply.mapHeader(static_cast<std::int64_t>(matched.size()));
        for (const auto& [name, value] : matched) {
            ctx.reply.bulk(name);
            ctx.reply.bulk(value);
        }
        return;
    }

    if (sub == "SET") {
        if (ctx.argc() < 4 || (ctx.argc() - 2) % 2 != 0) {
            replies::wrongArgs(ctx.reply, "config|set");
            return;
        }
        for (std::size_t i = 2; i + 1 < ctx.argc(); i += 2) {
            const std::string name = core::toLower(ctx.arg(i));
            if (name == "requirepass") {
                ctx.server.config().requirepass = ctx.arg(i + 1);
            } else if (name == "dir") {
                ctx.server.config().dir = ctx.arg(i + 1);
            } else if (name == "dbfilename") {
                ctx.server.config().dbfilename = ctx.arg(i + 1);
            }
            // Unknown-but-harmless parameters are accepted silently so tools
            // that tune benchmarks against us do not abort.
        }
        replies::ok(ctx.reply);
        return;
    }

    if (sub == "RESETSTAT") {
        ctx.server.stats() = Stats{};
        replies::ok(ctx.reply);
        return;
    }

    ctx.reply.error("ERR Unknown CONFIG subcommand or wrong number of arguments for '" +
                    ctx.arg(1) + "'");
}

void command(CommandContext& ctx) {
    if (ctx.argc() == 1) {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(allCommands().size()));
        for (const CommandSpec& spec : allCommands()) {
            ctx.reply.arrayHeader(6);
            ctx.reply.bulk(spec.name);
            ctx.reply.integer(spec.arity);
            ctx.reply.arrayHeader(spec.isWrite() ? 1 : 1);
            ctx.reply.simpleString(spec.isWrite() ? "write" : "readonly");
            ctx.reply.integer(spec.first_key);
            ctx.reply.integer(spec.last_key);
            ctx.reply.integer(spec.key_step);
        }
        return;
    }

    const std::string sub = core::toUpper(ctx.arg(1));
    if (sub == "COUNT") {
        ctx.reply.integer(static_cast<std::int64_t>(allCommands().size()));
        return;
    }
    if (sub == "DOCS") {
        // redis-cli asks for this on connect to build tab-completion hints. An
        // empty map is a valid answer and keeps the handshake quiet.
        ctx.reply.mapHeader(0);
        return;
    }
    if (sub == "INFO") {
        ctx.reply.arrayHeader(static_cast<std::int64_t>(ctx.argc() - 2));
        for (std::size_t i = 2; i < ctx.argc(); ++i) {
            const CommandSpec* spec = lookupCommand(ctx.arg(i));
            if (!spec) {
                ctx.reply.nullArray();
                continue;
            }
            ctx.reply.arrayHeader(6);
            ctx.reply.bulk(spec->name);
            ctx.reply.integer(spec->arity);
            ctx.reply.arrayHeader(1);
            ctx.reply.simpleString(spec->isWrite() ? "write" : "readonly");
            ctx.reply.integer(spec->first_key);
            ctx.reply.integer(spec->last_key);
            ctx.reply.integer(spec->key_step);
        }
        return;
    }
    ctx.reply.error("ERR Unknown COMMAND subcommand or wrong number of arguments for '" +
                    ctx.arg(1) + "'");
}

void object(CommandContext& ctx) {
    const std::string sub = core::toUpper(ctx.arg(1));

    if (sub == "HELP") {
        ctx.reply.arrayHeader(3);
        ctx.reply.simpleString("OBJECT ENCODING <key> -- physical representation in use");
        ctx.reply.simpleString("OBJECT REFCOUNT <key> -- references to the value");
        ctx.reply.simpleString("OBJECT IDLETIME <key> -- seconds since last access");
        return;
    }
    if (ctx.argc() != 3) {
        replies::wrongArgs(ctx.reply, "object");
        return;
    }

    Value* v = ctx.db.lookupRead(ctx.arg(2), ctx.nowMs());
    if (!v) {
        ctx.reply.error("ERR no such key");
        return;
    }

    if (sub == "ENCODING") {
        ctx.reply.bulk(core::encodingName(v->encoding()));
        return;
    }
    if (sub == "REFCOUNT") {
        // Real Redis shares immutable integer objects 0..9999 and reports a huge
        // refcount for them. We do not share objects, so every value is 1.
        ctx.reply.integer(1);
        return;
    }
    if (sub == "IDLETIME") {
        ctx.reply.integer((ctx.nowMs() - v->lastAccess()) / 1000);
        return;
    }
    if (sub == "FREQ") {
        ctx.reply.error("ERR An LFU maxmemory policy is not selected, access frequency not "
                        "tracked. Please note that when switching between maxmemory policies at "
                        "runtime LFU and LRU data will take some time to adjust.");
        return;
    }
    ctx.reply.error("ERR Unknown OBJECT subcommand or wrong number of arguments for '" +
                    ctx.arg(1) + "'");
}

void memory(CommandContext& ctx) {
    const std::string sub = core::toUpper(ctx.arg(1));

    if (sub == "USAGE") {
        if (ctx.argc() < 3) {
            replies::wrongArgs(ctx.reply, "memory|usage");
            return;
        }
        Value* v = ctx.db.lookupRead(ctx.arg(2), ctx.nowMs());
        if (!v) {
            ctx.reply.nullBulk();
            return;
        }
        // Key string plus value, which is what Redis's MEMORY USAGE reports.
        ctx.reply.integer(static_cast<std::int64_t>(v->memoryUsage() + ctx.arg(2).size()));
        return;
    }
    if (sub == "DOCTOR") {
        ctx.reply.bulk("Sam, I detected a few issues in this Redis instance memory implants:\n\n"
                       " * Everything looks fine from here.");
        return;
    }
    ctx.reply.error("ERR Unknown MEMORY subcommand or wrong number of arguments for '" +
                    ctx.arg(1) + "'");
}

void debug(CommandContext& ctx) {
    const std::string sub = core::toUpper(ctx.arg(1));

    if (sub == "SLEEP") {
        // Deliberately blocking: this is the command you use to *demonstrate*
        // that a single-threaded server stalls every other client.
        double seconds = ctx.argc() > 2 ? std::strtod(ctx.arg(2).c_str(), nullptr) : 0.0;
        if (seconds > 0) ::usleep(static_cast<useconds_t>(seconds * 1e6));
        replies::ok(ctx.reply);
        return;
    }
    if (sub == "SET-ACTIVE-EXPIRE") {
        replies::ok(ctx.reply);
        return;
    }
    if (sub == "JMAP" || sub == "FLUSHALL") {
        replies::ok(ctx.reply);
        return;
    }
    if (sub == "OBJECT") {
        if (ctx.argc() != 3) {
            replies::wrongArgs(ctx.reply, "debug|object");
            return;
        }
        Value* v = ctx.db.lookupRead(ctx.arg(2), ctx.nowMs());
        if (!v) {
            ctx.reply.error("ERR no such key");
            return;
        }
        std::string out = "Value at:0x0 refcount:1 encoding:";
        out += core::encodingName(v->encoding());
        out += " serializedlength:" + std::to_string(v->memoryUsage());
        out += " lru_seconds_idle:" + std::to_string((ctx.nowMs() - v->lastAccess()) / 1000);
        ctx.reply.simpleString(out);
        return;
    }
    if (sub == "STRINGMATCH-LEN") {
        replies::ok(ctx.reply);
        return;
    }
    if (sub == "DICT-STATS") {
        // Not a real Redis subcommand, but the single most useful thing to be
        // able to see while testing incremental rehashing.
        std::string out = "keyspace_size:" + std::to_string(ctx.db.size());
        out += " buckets:" + std::to_string(ctx.db.bucketCount());
        out += " rehashing:" + std::string(ctx.db.isRehashing() ? "1" : "0");
        ctx.reply.simpleString(out);
        return;
    }
    ctx.reply.error("ERR DEBUG subcommand '" + ctx.arg(1) + "' not supported");
}

void time(CommandContext& ctx) {
    timeval tv{};
    ::gettimeofday(&tv, nullptr);
    ctx.reply.arrayHeader(2);
    ctx.reply.bulk(std::to_string(tv.tv_sec));
    ctx.reply.bulk(std::to_string(tv.tv_usec));
}

void lastsave(CommandContext& ctx) {
    ctx.reply.integer(ctx.server.startTimeMs() / 1000);
}

void shutdown(CommandContext& ctx) {
    if (ctx.argc() > 1 && equalsIgnoreCase(ctx.arg(1), "ABORT")) {
        ctx.reply.error("ERR No shutdown in progress");
        return;
    }
    // No reply on success: the connection simply dies with the server, which is
    // what clients expect from SHUTDOWN.
    ctx.server.stop();
}

}  // namespace mnemos::server::cmd
