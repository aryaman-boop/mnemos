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
// A command whose first argument selects a subcommand. Redis identifies these
// by their "parent|sub" full name in errors, so anything that quotes a command
// name back at the client has to know which commands are containers.
inline constexpr std::uint32_t kContainer = 1u << 6;
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
// A subcommand nobody recognises, versus one that exists but was called wrong.
// Redis distinguishes the two and client tooling keys off the difference.
void unknownSubcommand(net::ReplyWriter& w, std::string_view container,
                       std::string_view sub);
void subcommandSyntaxError(net::ReplyWriter& w, std::string_view container,
                           std::string_view sub);
// The same two messages as bare text, without the error code in front. A
// command rejected before it runs is normally reported as an error -- but when
// the rejected command is EXEC, the refusal is folded into an EXECABORT
// instead, and that wrapper supplies the code itself.
std::string wrongArgsText(std::string_view command);
std::string subscriberModeText(std::string_view command);
// RESTORE's four refusals. The distinction between the first two is Redis's and
// it matters: a footer that does not check out is a payload from the wrong
// server or a corrupted one, while a body that does not parse is a payload this
// version cannot read.
void badDumpPayload(net::ReplyWriter& w);
void badDataFormat(net::ReplyWriter& w);
void busyKey(net::ReplyWriter& w);
void invalidRestoreTtl(net::ReplyWriter& w);
void invalidIdletime(net::ReplyWriter& w);
void invalidFreq(net::ReplyWriter& w);
}  // namespace replies

}  // namespace mnemos::server
