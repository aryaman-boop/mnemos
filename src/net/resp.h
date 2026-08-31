// RESP (REdis Serialization Protocol) -- both wire versions.
//
// RESP2 is the classic protocol; RESP3 (negotiated with `HELLO 3`) adds real
// types for maps, sets, doubles, booleans, nulls and out-of-band pushes. A
// client's protocol version changes how the *same* reply is encoded, so every
// command handler writes through ReplyWriter rather than emitting bytes itself.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mnemos::net {

// Protocol ceilings, matching Redis. They exist to bound the memory a single
// unauthenticated peer can make the server allocate.
inline constexpr long kMaxMultibulkLength = 1024L * 1024L;
inline constexpr long kMaxBulkLength      = 512L * 1024L * 1024L;
inline constexpr std::size_t kMaxInlineRequest = 64 * 1024;

// ---------------------------------------------------------------------------
// Request parsing
// ---------------------------------------------------------------------------

// Incremental parser for inbound commands. Holds partial state between calls so
// a command split across several read() syscalls is never re-scanned from the
// start -- the same reason Redis keeps multibulklen/bulklen on the client struct.
class RequestParser {
public:
    enum class Status {
        Incomplete,  // need more bytes; nothing consumed beyond what is parsed
        Complete,    // `argv` holds a full command
        Error,       // protocol violation; `error` is set, connection must close
    };

    // Parses from `buf` starting at `pos`, advancing `pos` past consumed bytes.
    Status parse(std::string_view buf, std::size_t& pos,
                 std::vector<std::string>& argv, std::string& error);

    void reset();

private:
    Status parseMultibulk(std::string_view buf, std::size_t& pos,
                          std::vector<std::string>& argv, std::string& error);
    Status parseInline(std::string_view buf, std::size_t& pos,
                       std::vector<std::string>& argv, std::string& error);

    std::vector<std::string> argv_;
    long multibulk_len_ = 0;   // arguments still to read for the current command
    long bulk_len_      = -1;  // declared length of the bulk being read, -1 if none
};

// Splits an inline/config line the way Redis's sdssplitargs does: whitespace
// separated, honouring "double" and 'single' quotes plus \xHH escapes.
// Returns nullopt on unbalanced quotes.
std::optional<std::vector<std::string>> splitArgs(std::string_view line);

// ---------------------------------------------------------------------------
// Reply writing
// ---------------------------------------------------------------------------

class ReplyWriter {
public:
    explicit ReplyWriter(std::string& out, int protocol_version = 2)
        : out_(out), protover_(protocol_version) {}

    int protocolVersion() const { return protover_; }

    void simpleString(std::string_view s);
    void error(std::string_view msg);
    void integer(std::int64_t n);
    void bulk(std::string_view s);
    void verbatim(std::string_view s, std::string_view format = "txt");

    // RESP2 has no null of its own, so a null degrades to the null *bulk* or
    // null *array* depending on what the command would otherwise have returned.
    void nullBulk();
    void nullArray();

    void arrayHeader(std::int64_t n);
    void mapHeader(std::int64_t pairs);   // RESP2: flat array of 2*pairs
    void setHeader(std::int64_t n);       // RESP2: plain array
    void pushHeader(std::int64_t n);      // RESP2: plain array

    void boolean(bool b);                 // RESP2: :1 / :0
    void doubleValue(double d);           // RESP2: bulk string
    void bigNumber(std::string_view digits);

    // Emits an array of bulk strings -- the single most common reply shape.
    void bulkArray(const std::vector<std::string>& items);

    void raw(std::string_view bytes) { out_.append(bytes); }

private:
    std::string& out_;
    int          protover_;
};

// Formats a double the way Redis does: integral values lose the ".0", and the
// infinities become "inf" / "-inf" rather than C's "inf"/"-inf" spelling drift.
std::string formatDouble(double d);

// ---------------------------------------------------------------------------
// Reply parsing (used by the replica link and by mnemos-mcp)
// ---------------------------------------------------------------------------

struct Reply {
    enum class Type {
        SimpleString, Error, Integer, Bulk, Array,
        Null, Boolean, Double, Map, Set, Push, BigNumber, Verbatim,
    };

    Type                     type = Type::Null;
    std::string              str;       // SimpleString/Error/Bulk/BigNumber/Verbatim
    std::int64_t             integer = 0;
    double                   dbl = 0.0;
    bool                     boolean = false;
    std::vector<Reply>       elements;  // Array/Map/Set/Push

    bool isError() const { return type == Type::Error; }
    bool isNull() const { return type == Type::Null; }
};

// Parses one complete reply from `buf` at `pos`. Returns false (leaving `pos`
// untouched) when more bytes are needed. Sets `error` on a protocol violation.
bool parseReply(std::string_view buf, std::size_t& pos, Reply& out, std::string& error);

// Encodes argv as a RESP multibulk command -- what a client sends on the wire.
std::string encodeCommand(const std::vector<std::string>& argv);

}  // namespace mnemos::net
