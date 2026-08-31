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
    std::string msg = "ERR wrong number of arguments for '";
    msg.append(command);
    msg.append("' command");
    w.error(msg);
}
void ok(net::ReplyWriter& w) { w.simpleString("OK"); }

}  // namespace replies

}  // namespace mnemos::server
