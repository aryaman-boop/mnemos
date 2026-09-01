// Keyspace notifications: the class flags, and the two events every mutation
// publishes.
//
// A change to key `k` in database 0 is announced twice, on two channels that
// carry the same fact the other way round:
//
//     __keyspace@0__:k      -> "lpush"     (what happened to this key)
//     __keyevent@0__:lpush  -> "k"         (which key this happened to)
//
// Which of the two goes out is chosen by the K and E flags, and whether
// anything goes out at all by the event's class flag -- `l` here. Delivery is
// an ordinary PUBLISH: subscribers and patterns see these messages exactly as
// they would any other, which is the whole point of the design.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mnemos::server {

struct CommandContext;

namespace notify {

// The class flags, one bit per character of `notify-keyspace-events`.
inline constexpr std::uint32_t kKeyspace = 1u << 0;   // K
inline constexpr std::uint32_t kKeyevent = 1u << 1;   // E
inline constexpr std::uint32_t kGeneric  = 1u << 2;   // g -- DEL, EXPIRE, RENAME
inline constexpr std::uint32_t kString   = 1u << 3;   // $
inline constexpr std::uint32_t kList     = 1u << 4;   // l
inline constexpr std::uint32_t kSet      = 1u << 5;   // s
inline constexpr std::uint32_t kHash     = 1u << 6;   // h
inline constexpr std::uint32_t kZset     = 1u << 7;   // z
inline constexpr std::uint32_t kExpired  = 1u << 8;   // x
inline constexpr std::uint32_t kEvicted  = 1u << 9;   // e
inline constexpr std::uint32_t kStream   = 1u << 10;  // t
inline constexpr std::uint32_t kModule   = 1u << 11;  // d
inline constexpr std::uint32_t kNew      = 1u << 12;  // n -- a key that did not exist
inline constexpr std::uint32_t kKeyMiss  = 1u << 13;  // m -- a read that found nothing
// Classes for event sources mnemos does not have. They are parsed and reported
// so that CONFIG round-trips a real Redis configuration unchanged, but nothing
// ever raises them.
inline constexpr std::uint32_t kClassA   = 1u << 14;  // a
inline constexpr std::uint32_t kClassO   = 1u << 15;  // o
inline constexpr std::uint32_t kClassC   = 1u << 16;  // c
inline constexpr std::uint32_t kClassS   = 1u << 17;  // S
inline constexpr std::uint32_t kClassT   = 1u << 18;  // T
inline constexpr std::uint32_t kClassI   = 1u << 19;  // I
inline constexpr std::uint32_t kClassV   = 1u << 20;  // V

// What "A" abbreviates. Note what it leaves out: K and E say *where* to
// publish rather than what to publish, and m (key misses) and n (new keys) are
// loud enough that Redis keeps them opt-in. S, T, I and V are newer and outside
// the alias too. n is the odd one: excluded here, yet swallowed by the "A" in
// formatFlags, so "An" round-trips as "A" while still delivering new-key
// events. That asymmetry is Redis's, and CONFIG GET has to reproduce it.
inline constexpr std::uint32_t kAll = kGeneric | kString | kList | kSet | kHash | kZset |
                                      kExpired | kEvicted | kStream | kModule |
                                      kClassA | kClassO | kClassC;

// The character set the error message quotes back, in Redis's own order.
inline constexpr std::string_view kClassChars = "Ag$lshzxeKEtmdnocaSTIV";

// Parses a `notify-keyspace-events` string. Returns false, leaving `out`
// untouched, when a character is not a known class.
bool parseFlags(std::string_view spec, std::uint32_t& out);

// The inverse, and not simply the string that was parsed: Redis stores flags as
// a bitmask and rebuilds the string from it, so CONFIG GET reports a canonical
// spelling ("KEA" comes back as "AKE").
std::string formatFlags(std::uint32_t flags);

}  // namespace notify

// Announces `event` on `key`, if `event_class` is enabled. The database is the
// one the command is operating on; COPY and friends that can target another
// database pass its index explicitly.
void notifyKeyspaceEvent(CommandContext& ctx, std::uint32_t event_class,
                         std::string_view event, const std::string& key);
void notifyKeyspaceEvent(CommandContext& ctx, std::uint32_t event_class,
                         std::string_view event, const std::string& key, int db_index);

// The `m` class: a read that looked for a key and did not find it. Raised by
// read paths only -- a write to a missing key reports nothing, which is why
// INCR on a fresh key is silent here but GETSET is not.
void notifyKeyMiss(CommandContext& ctx, const std::string& key);

}  // namespace mnemos::server
