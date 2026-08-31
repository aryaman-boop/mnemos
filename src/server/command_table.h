// Command registry: arity, flags, and key positions for every command.
//
// The metadata is not decoration. Arity and key positions are what let the
// server validate a command before executing it, and what a cluster-aware proxy
// would use to route by key -- which is why COMMAND exposes them on the wire.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "net/resp.h"
#include "server/db.h"

namespace mnemos::server {

class Server;
class Client;

struct CommandContext {
    Server&                         server;
    Client&                         client;
    Database&                       db;
    const std::vector<std::string>& argv;
    net::ReplyWriter&               reply;

    std::size_t argc() const { return argv.size(); }
    const std::string& arg(std::size_t i) const { return argv[i]; }
    std::int64_t nowMs() const;
};

using CommandHandler = void (*)(CommandContext&);

namespace flags {
inline constexpr std::uint32_t kWrite    = 1u << 0;  // may modify the keyspace
inline constexpr std::uint32_t kReadOnly = 1u << 1;
inline constexpr std::uint32_t kAdmin    = 1u << 2;
inline constexpr std::uint32_t kNoAuth   = 1u << 3;  // allowed before AUTH
inline constexpr std::uint32_t kFast     = 1u << 4;  // O(1)
inline constexpr std::uint32_t kLoading  = 1u << 5;  // allowed while loading
}  // namespace flags

struct CommandSpec {
    std::string_view name;
    CommandHandler   handler;
    // Total argument count including the command name. Negative means "at least
    // |arity| arguments" -- exactly Redis's convention.
    int              arity;
    std::uint32_t    flags;
    int              first_key;  // 1-based index of the first key argument, 0 = none
    int              last_key;   // -1 = last argument
    int              key_step;

    bool isWrite() const { return flags & flags::kWrite; }
};

// Case-insensitive lookup. Returns nullptr for an unknown command.
const CommandSpec* lookupCommand(std::string_view name);
const std::vector<CommandSpec>& allCommands();

// True when `argc` satisfies the spec's arity rule.
bool arityOk(const CommandSpec& spec, std::size_t argc);

// Shared reply helpers, so error strings stay byte-identical to Redis's.
namespace replies {
void wrongType(net::ReplyWriter& w);
void syntaxError(net::ReplyWriter& w);
void notAnInteger(net::ReplyWriter& w);
void notAFloat(net::ReplyWriter& w);
void outOfRange(net::ReplyWriter& w);
void wrongArgs(net::ReplyWriter& w, std::string_view command);
void ok(net::ReplyWriter& w);
}  // namespace replies

}  // namespace mnemos::server
