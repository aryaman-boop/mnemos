#include "server/command_table.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "server/commands/commands.h"
#include "server/server.h"

namespace mnemos::server {

std::int64_t CommandContext::nowMs() const { return server.nowMs(); }

namespace {

using namespace flags;

// Kept in one place so COMMAND, COMMAND COUNT and COMMAND DOCS all agree, and so
// adding a command is a single-line change.
const std::vector<CommandSpec>& table() {
    static const std::vector<CommandSpec> commands = {
        // name           handler                arity  flags                        fk  lk  step
        {"ping",          cmd::ping,               -1,  kFast | kNoAuth | kLoading,   0,  0,  0},
        {"echo",          cmd::echo,                2,  kFast,                        0,  0,  0},
        {"select",        cmd::select,              2,  kFast | kLoading,             0,  0,  0},
        {"hello",         cmd::hello,              -1,  kFast | kNoAuth | kLoading,   0,  0,  0},
        {"auth",          cmd::auth,               -2,  kFast | kNoAuth | kLoading,   0,  0,  0},
        {"quit",          cmd::quit,               -1,  kFast | kNoAuth | kLoading,   0,  0,  0},
        {"reset",         cmd::reset,               1,  kFast | kNoAuth | kLoading,   0,  0,  0},
        {"client",        cmd::client,             -2,  kAdmin | kNoAuth | kLoading,  0,  0,  0},

        // Transactions. All five run rather than queue when the client is
        // already inside MULTI -- see Server::dispatch.
        {"multi",         cmd::multi,               1,  kFast | kLoading,             0,  0,  0},
        {"exec",          cmd::exec,                1,  kLoading,                     0,  0,  0},
        {"discard",       cmd::discard,             1,  kFast | kLoading,             0,  0,  0},
        {"watch",         cmd::watch,              -2,  kFast | kLoading,             1, -1,  1},
        {"unwatch",       cmd::unwatch,             1,  kFast | kLoading,             0,  0,  0},

        // Global pub/sub carries no keys, so first/last/step stay zero -- the
        // channel name is not a keyspace key and must not be routed as one.
        {"subscribe",     cmd::subscribe,          -2,  kFast | kLoading,             0,  0,  0},
        {"unsubscribe",   cmd::unsubscribe,        -1,  kFast | kLoading,             0,  0,  0},
        {"psubscribe",    cmd::psubscribe,         -2,  kFast | kLoading,             0,  0,  0},
        {"punsubscribe",  cmd::punsubscribe,       -1,  kFast | kLoading,             0,  0,  0},
        {"publish",       cmd::publish,             3,  kFast | kLoading,             0,  0,  0},
        // The shard variants do report key positions: a cluster hashes the
        // shard channel name to pick the node, so a proxy has to find it the
        // same way it finds a key, even though the name is not a key.
        {"ssubscribe",    cmd::ssubscribe,         -2,  kFast | kLoading,             1, -1,  1},
        {"sunsubscribe",  cmd::sunsubscribe,       -1,  kFast | kLoading,             1, -1,  1},
        {"spublish",      cmd::spublish,            3,  kFast | kLoading,             1,  1,  1},
        {"pubsub",        cmd::pubsub,             -2,  kFast | kLoading | kContainer, 0, 0,  0},

        {"set",           cmd::set,                -3,  kWrite,                       1,  1,  1},
        {"get",           cmd::get,                 2,  kReadOnly | kFast,            1,  1,  1},
        {"getset",        cmd::getset,              3,  kWrite | kFast,               1,  1,  1},
        {"getdel",        cmd::getdel,              2,  kWrite | kFast,               1,  1,  1},
        {"getex",         cmd::getex,              -2,  kWrite | kFast,               1,  1,  1},
        {"setnx",         cmd::setnx,               3,  kWrite | kFast,               1,  1,  1},
        {"setex",         cmd::setex,               4,  kWrite,                       1,  1,  1},
        {"psetex",        cmd::psetex,              4,  kWrite,                       1,  1,  1},
        {"mset",          cmd::mset,               -3,  kWrite,                       1, -1,  2},
        {"msetnx",        cmd::msetnx,             -3,  kWrite,                       1, -1,  2},
        {"mget",          cmd::mget,               -2,  kReadOnly | kFast,            1, -1,  1},
        {"append",        cmd::append,              3,  kWrite | kFast,               1,  1,  1},
        {"strlen",        cmd::strlen_,             2,  kReadOnly | kFast,            1,  1,  1},
        {"incr",          cmd::incr,                2,  kWrite | kFast,               1,  1,  1},
        {"decr",          cmd::decr,                2,  kWrite | kFast,               1,  1,  1},
        {"incrby",        cmd::incrby,              3,  kWrite | kFast,               1,  1,  1},
        {"decrby",        cmd::decrby,              3,  kWrite | kFast,               1,  1,  1},
        {"incrbyfloat",   cmd::incrbyfloat,         3,  kWrite | kFast,               1,  1,  1},
        {"setrange",      cmd::setrange,            4,  kWrite,                       1,  1,  1},
        {"getrange",      cmd::getrange,            4,  kReadOnly,                    1,  1,  1},
        {"substr",        cmd::getrange,            4,  kReadOnly,                    1,  1,  1},

        {"del",           cmd::del,                -2,  kWrite,                       1, -1,  1},
        {"unlink",        cmd::unlink,             -2,  kWrite | kFast,               1, -1,  1},
        {"exists",        cmd::exists,             -2,  kReadOnly | kFast,            1, -1,  1},
        {"type",          cmd::type,                2,  kReadOnly | kFast,            1,  1,  1},
        {"keys",          cmd::keys,                2,  kReadOnly,                    0,  0,  0},
        {"scan",          cmd::scan,               -2,  kReadOnly,                    0,  0,  0},
        {"randomkey",     cmd::randomkey,           1,  kReadOnly,                    0,  0,  0},
        {"rename",        cmd::rename,              3,  kWrite,                       1,  2,  1},
        {"renamenx",      cmd::renamenx,            3,  kWrite | kFast,               1,  2,  1},
        {"copy",          cmd::copy,               -3,  kWrite,                       1,  2,  1},
        {"touch",         cmd::touch,              -2,  kReadOnly | kFast,            1, -1,  1},
        {"expire",        cmd::expire,             -3,  kWrite | kFast,               1,  1,  1},
        {"pexpire",       cmd::pexpire,            -3,  kWrite | kFast,               1,  1,  1},
        {"expireat",      cmd::expireat,           -3,  kWrite | kFast,               1,  1,  1},
        {"pexpireat",     cmd::pexpireat,          -3,  kWrite | kFast,               1,  1,  1},
        {"ttl",           cmd::ttl,                 2,  kReadOnly | kFast,            1,  1,  1},
        {"pttl",          cmd::pttl,                2,  kReadOnly | kFast,            1,  1,  1},
        {"expiretime",    cmd::expiretime,          2,  kReadOnly | kFast,            1,  1,  1},
        {"pexpiretime",   cmd::pexpiretime,         2,  kReadOnly | kFast,            1,  1,  1},
        {"persist",       cmd::persist,             2,  kWrite | kFast,               1,  1,  1},

        {"dbsize",        cmd::dbsize,              1,  kReadOnly | kFast,            0,  0,  0},
        {"flushdb",       cmd::flushdb,            -1,  kWrite,                       0,  0,  0},
        {"flushall",      cmd::flushall,           -1,  kWrite,                       0,  0,  0},
        {"info",          cmd::info,               -1,  kLoading | kNoAuth,           0,  0,  0},
        {"config",        cmd::config,             -2,  kAdmin | kLoading,            0,  0,  0},
        {"command",       cmd::command,            -1,  kLoading | kNoAuth,           0,  0,  0},
        {"object",        cmd::object,             -2,  kReadOnly,                    2,  2,  1},
        {"memory",        cmd::memory,             -2,  kReadOnly,                    0,  0,  0},
        {"debug",         cmd::debug,              -2,  kAdmin,                       0,  0,  0},
        {"time",          cmd::time,                1,  kFast | kLoading,             0,  0,  0},
        {"lastsave",      cmd::lastsave,            1,  kFast | kLoading,             0,  0,  0},
        {"shutdown",      cmd::shutdown,           -1,  kAdmin | kNoAuth | kLoading,  0,  0,  0},

        {"lpush",         cmd::lpush,              -3,  kWrite | kFast,               1,  1,  1},
        {"rpush",         cmd::rpush,              -3,  kWrite | kFast,               1,  1,  1},
        {"lpushx",        cmd::lpushx,             -3,  kWrite | kFast,               1,  1,  1},
        {"rpushx",        cmd::rpushx,             -3,  kWrite | kFast,               1,  1,  1},
        {"lpop",          cmd::lpop,               -2,  kWrite | kFast,               1,  1,  1},
        {"rpop",          cmd::rpop,               -2,  kWrite | kFast,               1,  1,  1},
        {"llen",          cmd::llen,                2,  kReadOnly | kFast,            1,  1,  1},
        {"lrange",        cmd::lrange,              4,  kReadOnly,                    1,  1,  1},
        {"lindex",        cmd::lindex,              3,  kReadOnly,                    1,  1,  1},
        {"lset",          cmd::lset,                4,  kWrite,                       1,  1,  1},
        {"lrem",          cmd::lrem,                4,  kWrite,                       1,  1,  1},
        {"ltrim",         cmd::ltrim,               4,  kWrite,                       1,  1,  1},
        {"lpos",          cmd::lpos,               -3,  kReadOnly,                    1,  1,  1},
        {"rpoplpush",     cmd::rpoplpush,           3,  kWrite,                       1,  2,  1},
        {"lmove",         cmd::lmove,               5,  kWrite,                       1,  2,  1},

        {"hset",          cmd::hset,               -4,  kWrite | kFast,               1,  1,  1},
        {"hsetnx",        cmd::hsetnx,              4,  kWrite | kFast,               1,  1,  1},
        {"hmset",         cmd::hmset,              -4,  kWrite | kFast,               1,  1,  1},
        {"hget",          cmd::hget,                3,  kReadOnly | kFast,            1,  1,  1},
        {"hmget",         cmd::hmget,              -3,  kReadOnly | kFast,            1,  1,  1},
        {"hdel",          cmd::hdel,               -3,  kWrite | kFast,               1,  1,  1},
        {"hlen",          cmd::hlen,                2,  kReadOnly | kFast,            1,  1,  1},
        {"hexists",       cmd::hexists,             3,  kReadOnly | kFast,            1,  1,  1},
        {"hstrlen",       cmd::hstrlen,             3,  kReadOnly | kFast,            1,  1,  1},
        {"hkeys",         cmd::hkeys,               2,  kReadOnly,                    1,  1,  1},
        {"hvals",         cmd::hvals,               2,  kReadOnly,                    1,  1,  1},
        {"hgetall",       cmd::hgetall,             2,  kReadOnly,                    1,  1,  1},
        {"hincrby",       cmd::hincrby,             4,  kWrite | kFast,               1,  1,  1},
        {"hincrbyfloat",  cmd::hincrbyfloat,        4,  kWrite | kFast,               1,  1,  1},
        {"hrandfield",    cmd::hrandfield,         -2,  kReadOnly,                    1,  1,  1},

        {"sadd",          cmd::sadd,               -3,  kWrite | kFast,               1,  1,  1},
        {"srem",          cmd::srem,               -3,  kWrite | kFast,               1,  1,  1},
        {"scard",         cmd::scard,               2,  kReadOnly | kFast,            1,  1,  1},
        {"sismember",     cmd::sismember,           3,  kReadOnly | kFast,            1,  1,  1},
        {"smismember",    cmd::smismember,         -3,  kReadOnly | kFast,            1,  1,  1},
        {"smembers",      cmd::smembers,            2,  kReadOnly,                    1,  1,  1},
        {"spop",          cmd::spop,               -2,  kWrite | kFast,               1,  1,  1},
        {"srandmember",   cmd::srandmember,        -2,  kReadOnly,                    1,  1,  1},
        {"smove",         cmd::smove,               4,  kWrite | kFast,               1,  2,  1},
        {"sinter",        cmd::sinter,             -2,  kReadOnly,                    1, -1,  1},
        {"sunion",        cmd::sunion,             -2,  kReadOnly,                    1, -1,  1},
        {"sdiff",         cmd::sdiff,              -2,  kReadOnly,                    1, -1,  1},
        {"sinterstore",   cmd::sinterstore,        -3,  kWrite,                       1, -1,  1},
        {"sunionstore",   cmd::sunionstore,        -3,  kWrite,                       1, -1,  1},
        {"sdiffstore",    cmd::sdiffstore,         -3,  kWrite,                       1, -1,  1},
        {"sintercard",    cmd::sintercard,         -3,  kReadOnly,                    0,  0,  0},

        {"zadd",          cmd::zadd,               -4,  kWrite | kFast,               1,  1,  1},
        {"zrem",          cmd::zrem,               -3,  kWrite | kFast,               1,  1,  1},
        {"zscore",        cmd::zscore,              3,  kReadOnly | kFast,            1,  1,  1},
        {"zmscore",       cmd::zmscore,            -3,  kReadOnly | kFast,            1,  1,  1},
        {"zcard",         cmd::zcard,               2,  kReadOnly | kFast,            1,  1,  1},
        {"zincrby",       cmd::zincrby,             4,  kWrite | kFast,               1,  1,  1},
        {"zrank",         cmd::zrank,               3,  kReadOnly | kFast,            1,  1,  1},
        {"zrevrank",      cmd::zrevrank,            3,  kReadOnly | kFast,            1,  1,  1},
        {"zrange",        cmd::zrange,             -4,  kReadOnly,                    1,  1,  1},
        {"zrevrange",     cmd::zrevrange,          -4,  kReadOnly,                    1,  1,  1},
        {"zrangebyscore", cmd::zrangebyscore,      -4,  kReadOnly,                    1,  1,  1},
        {"zrevrangebyscore", cmd::zrevrangebyscore,-4,  kReadOnly,                    1,  1,  1},
        {"zcount",        cmd::zcount,              4,  kReadOnly | kFast,            1,  1,  1},
        {"zremrangebyrank", cmd::zremrangebyrank,   4,  kWrite,                       1,  1,  1},
        {"zremrangebyscore", cmd::zremrangebyscore, 4,  kWrite,                       1,  1,  1},
        {"zpopmin",       cmd::zpopmin,            -2,  kWrite | kFast,               1,  1,  1},
        {"zpopmax",       cmd::zpopmax,            -2,  kWrite | kFast,               1,  1,  1},
        {"zrandmember",   cmd::zrandmember,        -2,  kReadOnly,                    1,  1,  1},
    };
    return commands;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const std::unordered_map<std::string, const CommandSpec*>& index() {
    static const auto* map = [] {
        auto* m = new std::unordered_map<std::string, const CommandSpec*>();
        for (const CommandSpec& spec : table()) m->emplace(std::string(spec.name), &spec);
        return m;
    }();
    return *map;
}

}  // namespace

const CommandSpec* lookupCommand(std::string_view name) {
    const auto& map = index();
    const auto it = map.find(toLower(name));
    return it == map.end() ? nullptr : it->second;
}

const std::vector<CommandSpec>& allCommands() { return table(); }

bool arityOk(const CommandSpec& spec, std::size_t argc) {
    const auto n = static_cast<int>(argc);
    return spec.arity >= 0 ? n == spec.arity : n >= -spec.arity;
}

namespace replies {

void wrongType(net::ReplyWriter& w) {
    w.error("WRONGTYPE Operation against a key holding the wrong kind of value");
}
void syntaxError(net::ReplyWriter& w) { w.error("ERR syntax error"); }
void notAnInteger(net::ReplyWriter& w) {
    w.error("ERR value is not an integer or out of range");
}
void notAFloat(net::ReplyWriter& w) { w.error("ERR value is not a valid float"); }
void outOfRange(net::ReplyWriter& w) { w.error("ERR value is out of range"); }
void wrongArgs(net::ReplyWriter& w, std::string_view command) {
    w.error("ERR " + wrongArgsText(command));
}
void ok(net::ReplyWriter& w) { w.simpleString("OK"); }

std::string wrongArgsText(std::string_view command) {
    std::string msg = "wrong number of arguments for '";
    msg.append(command);
    msg.append("' command");
    return msg;
}

std::string subscriberModeText(std::string_view command) {
    std::string msg = "Can't execute '";
    msg.append(command);
    msg.append("': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / "
               "RESET are allowed in this context");
    return msg;
}

void unknownSubcommand(net::ReplyWriter& w, std::string_view container,
                       std::string_view sub) {
    std::string msg = "ERR unknown subcommand '";
    msg.append(sub);
    msg.append("'. Try ");
    msg.append(container);
    msg.append(" HELP.");
    w.error(msg);
}

void subcommandSyntaxError(net::ReplyWriter& w, std::string_view container,
                           std::string_view sub) {
    std::string msg = "ERR unknown subcommand or wrong number of arguments for '";
    msg.append(sub);
    msg.append("'. Try ");
    msg.append(container);
    msg.append(" HELP.");
    w.error(msg);
}

}  // namespace replies

}  // namespace mnemos::server
