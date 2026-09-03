#include "mcp/tools.h"

#include <cstdint>
#include <cstdlib>

#include "core/dtoa.h"
#include "core/strings.h"
#include "server/command_table.h"

namespace mnemos::mcp {
namespace {

// A SCAN walk returns at most this many keys however large the keyspace is. A
// model does not benefit from ten million keys and the transport does not
// survive them.
constexpr long kScanHardCap    = 10000;
constexpr long kScanDefault    = 100;
constexpr long kScanPageCount  = 500;
// SCAN's cursor is not monotonic, so "have we finished?" is the only stopping
// condition it offers. This bounds a pathological walk anyway.
constexpr int  kScanMaxPages   = 10000;

Json textSchema(std::string description) {
    Json prop = Json::object();
    prop.set("type", Json::string("string"));
    prop.set("description", Json::string(std::move(description)));
    return prop;
}

Json numberSchema(std::string description) {
    Json prop = Json::object();
    prop.set("type", Json::string("integer"));
    prop.set("description", Json::string(std::move(description)));
    return prop;
}

Json objectSchema(Json properties, std::vector<std::string> required) {
    Json schema = Json::object();
    schema.set("type", Json::string("object"));
    schema.set("properties", std::move(properties));
    Json req = Json::array();
    for (std::string& r : required) req.push(Json::string(std::move(r)));
    schema.set("required", std::move(req));
    return schema;
}

Json toolDescriptor(std::string name, std::string description, Json schema) {
    Json tool = Json::object();
    tool.set("name", Json::string(std::move(name)));
    tool.set("description", Json::string(std::move(description)));
    tool.set("inputSchema", std::move(schema));
    return tool;
}

// A string argument, or the empty string when absent. Numbers are accepted for
// the value-shaped arguments because a model that means 42 often sends 42.
std::string stringArg(const Json& args, std::string_view key, bool* present = nullptr) {
    const Json* v = args.find(key);
    if (present != nullptr) *present = v != nullptr && !v->isNull();
    if (v == nullptr) return {};
    if (v->isString()) return v->asString();
    if (v->isNumber()) return core::d2string(v->asNumber());
    if (v->isBool()) return v->asBool() ? "1" : "0";
    return {};
}

bool longArg(const Json& args, std::string_view key, long& out) {
    const Json* v = args.find(key);
    if (v == nullptr || v->isNull()) return false;
    if (v->isNumber()) {
        out = static_cast<long>(v->asNumber());
        return true;
    }
    if (v->isString()) {
        char*      end = nullptr;
        const long n   = std::strtol(v->asString().c_str(), &end, 10);
        if (end != nullptr && *end == '\0' && !v->asString().empty()) {
            out = n;
            return true;
        }
    }
    return false;
}

ToolResult failure(std::string text) { return ToolResult{std::move(text), true}; }

// Every tool answers with one serialised JSON object, so a model gets the same
// shape back whichever one it called.
ToolResult success(const Json& payload) { return ToolResult{payload.serialise(), false}; }

// The size command that means something for each type. A type with no natural
// size (none of them, currently, beyond `none`) reports nothing.
const char* sizeCommandFor(std::string_view type) {
    if (type == "string") return "STRLEN";
    if (type == "list") return "LLEN";
    if (type == "hash") return "HLEN";
    if (type == "set") return "SCARD";
    if (type == "zset") return "ZCARD";
    if (type == "stream") return "XLEN";
    return nullptr;
}

}  // namespace

Json replyToJson(const net::Reply& reply, bool& binary) {
    using Type = net::Reply::Type;

    const auto text = [&binary](const std::string& s) {
        if (!isValidUtf8(s)) binary = true;
        return Json::string(sanitiseUtf8(s));
    };

    switch (reply.type) {
        case Type::SimpleString:
        case Type::Error:
        case Type::Bulk:
        case Type::BigNumber:
            return text(reply.str);
        case Type::Verbatim:
            // A verbatim string arrives with a three-byte format tag and a
            // colon in front of the payload. The payload is what a caller
            // wants; the tag is about rendering.
            return text(reply.str.size() > 4 ? reply.str.substr(4) : reply.str);
        case Type::Integer:
            return Json::number(static_cast<double>(reply.integer));
        case Type::Double:
            return Json::number(reply.dbl);
        case Type::Boolean:
            return Json::boolean(reply.boolean);
        case Type::Null:
            return Json::null();
        case Type::Array:
        case Type::Set:
        case Type::Push: {
            Json out = Json::array();
            for (const net::Reply& e : reply.elements) out.push(replyToJson(e, binary));
            return out;
        }
        case Type::Map: {
            // Elements are stored flat as k,v,k,v. An object is the better
            // rendering, but only when every key really is a string -- RESP3
            // permits any type there, and JSON does not.
            bool keys_are_strings = true;
            for (std::size_t i = 0; i + 1 < reply.elements.size(); i += 2) {
                const Type kt = reply.elements[i].type;
                if (kt != Type::SimpleString && kt != Type::Bulk && kt != Type::Verbatim) {
                    keys_are_strings = false;
                    break;
                }
            }
            if (keys_are_strings) {
                Json out = Json::object();
                for (std::size_t i = 0; i + 1 < reply.elements.size(); i += 2) {
                    const std::string& key = reply.elements[i].str;
                    if (!isValidUtf8(key)) binary = true;
                    out.set(sanitiseUtf8(key), replyToJson(reply.elements[i + 1], binary));
                }
                return out;
            }
            Json out = Json::array();
            for (std::size_t i = 0; i + 1 < reply.elements.size(); i += 2) {
                Json pair = Json::array();
                pair.push(replyToJson(reply.elements[i], binary));
                pair.push(replyToJson(reply.elements[i + 1], binary));
                out.push(std::move(pair));
            }
            return out;
        }
    }
    return Json::null();
}

RedisTools::RedisTools(std::string host, int port, int db, int timeout_ms, bool read_only)
    : host_(std::move(host)), port_(port), db_(db), timeout_ms_(timeout_ms),
      read_only_(read_only) {
    conn_.setTimeoutMs(timeout_ms_);
}

bool RedisTools::ensureConnected(std::string& error) {
    if (conn_.connected()) return true;
    if (!conn_.connect(host_, port_, error)) return false;
    // A HELLO failure is not fatal -- negotiateResp3 falls back to RESP2 on an
    // error reply and only returns false when the link itself broke.
    if (!conn_.negotiateResp3(error)) return false;
    if (db_ != 0) {
        net::Reply reply;
        if (!conn_.command({"SELECT", std::to_string(db_)}, reply, error)) return false;
        if (reply.isError()) {
            error = reply.str;
            conn_.close();
            return false;
        }
    }
    return true;
}

bool RedisTools::warmUp(std::string& error) { return ensureConnected(error); }

bool RedisTools::run(const std::vector<std::string>& argv, net::Reply& out, std::string& error) {
    if (!ensureConnected(error)) return false;
    if (conn_.command(argv, out, error)) return true;
    // The link dropped mid-command. One reconnect covers the ordinary case of
    // a server that restarted between calls; a second failure is reported.
    std::string retry_error;
    if (!ensureConnected(retry_error)) return false;
    return conn_.command(argv, out, error);
}

Json RedisTools::listTools() const {
    Json tools = Json::array();

    {
        Json items = Json::object();
        items.set("type", Json::string("string"));

        Json argv = Json::object();
        argv.set("type", Json::string("array"));
        argv.set("items", std::move(items));
        argv.set("description",
                 Json::string("The command and its arguments, one array element each: "
                              "[\"LPUSH\", \"mylist\", \"a\"]."));
        Json props = Json::object();
        props.set("argv", std::move(argv));
        tools.push(toolDescriptor(
            "redis_command",
            std::string("Run any command against the server and return its reply as JSON. "
                        "The escape hatch: use it for anything the other tools do not "
                        "cover.") +
                (read_only_ ? " This server is read-only, so commands that write or "
                              "administer are refused."
                            : ""),
            objectSchema(std::move(props), {"argv"})));
    }

    {
        Json props = Json::object();
        props.set("key", textSchema("The key to read."));
        tools.push(toolDescriptor("get", "Read a string key. Returns null when it does not exist.",
                                  objectSchema(std::move(props), {"key"})));
    }

    if (!read_only_) {
        Json props = Json::object();
        props.set("key", textSchema("The key to write."));
        props.set("value", textSchema("The value to store."));
        props.set("ttl_seconds", numberSchema("Expire the key after this many seconds."));
        tools.push(toolDescriptor("set", "Write a string key, optionally with a TTL.",
                                  objectSchema(std::move(props), {"key", "value"})));
    }

    {
        Json props = Json::object();
        props.set("pattern", textSchema("Glob pattern to match, e.g. \"user:*\". Default \"*\"."));
        props.set("count", numberSchema("Maximum keys to return. Default 100, capped at 10000."));
        tools.push(toolDescriptor(
            "scan_keys",
            "Walk the whole keyspace with SCAN and return the matching keys. Safe on a "
            "large keyspace: it pages, and it never blocks the server the way KEYS does.",
            objectSchema(std::move(props), {})));
    }

    {
        Json props = Json::object();
        props.set("key", textSchema("The key to describe."));
        tools.push(toolDescriptor(
            "describe_key",
            "Report a key's type, in-memory encoding, TTL and size without reading its "
            "contents. The encoding is the real one (listpack, intset, quicklist, "
            "skiplist, hashtable, embstr, int), which is how you tell a small collection "
            "from one that has been converted.",
            objectSchema(std::move(props), {"key"})));
    }

    {
        Json props = Json::object();
        props.set("section", textSchema("One INFO section, e.g. \"memory\" or \"keyspace\". "
                                        "Omit for all of them."));
        tools.push(toolDescriptor("server_info",
                                  "Server statistics from INFO, parsed into an object of "
                                  "sections rather than returned as one blob.",
                                  objectSchema(std::move(props), {})));
    }

    return tools;
}

bool RedisTools::hasTool(std::string_view name) const {
    if (name == "set") return !read_only_;
    return name == "redis_command" || name == "get" || name == "scan_keys" ||
           name == "describe_key" || name == "server_info";
}

ToolResult RedisTools::callTool(const std::string& name, const Json& arguments) {
    if (name == "redis_command") return redisCommand(arguments);
    if (name == "get") return get(arguments);
    if (name == "set") return set(arguments);
    if (name == "scan_keys") return scanKeys(arguments);
    if (name == "describe_key") return describeKey(arguments);
    if (name == "server_info") return serverInfo(arguments);
    return failure("unknown tool '" + name + "'");
}

ToolResult RedisTools::redisCommand(const Json& args) {
    const Json* argv_json = args.find("argv");
    if (argv_json == nullptr || !argv_json->isArray() || argv_json->elements().empty()) {
        return failure("argv must be a non-empty array of strings");
    }

    std::vector<std::string> argv;
    argv.reserve(argv_json->elements().size());
    for (const Json& e : argv_json->elements()) {
        if (e.isString())      argv.push_back(e.asString());
        else if (e.isNumber()) argv.push_back(core::d2string(e.asNumber()));
        else if (e.isBool())   argv.push_back(e.asBool() ? "1" : "0");
        else return failure("argv elements must be strings or numbers");
    }

    if (read_only_) {
        // Classification comes from the command table this binary already links,
        // so it costs a lookup. A command the table does not know is refused
        // rather than passed through -- an unclassifiable command cannot be
        // shown to be safe.
        const server::CommandSpec* spec = server::lookupCommand(argv[0]);
        if (spec == nullptr) {
            return failure("read-only mode: unknown command '" + argv[0] + "' cannot be "
                           "classified, so it is refused");
        }
        if (spec->isWrite() || (spec->flags & server::flags::kAdmin) != 0) {
            return failure("read-only mode: " + core::toUpper(argv[0]) +
                           " modifies or administers the server and is refused");
        }
    }

    net::Reply  reply;
    std::string error;
    if (!run(argv, reply, error)) return failure("connection failed: " + error);

    bool       binary = false;
    const Json value  = replyToJson(reply, binary);
    if (reply.isError()) return failure(reply.str);

    Json out = Json::object();
    out.set("result", value);
    if (binary) out.set("binary", Json::boolean(true));
    return success(out);
}

ToolResult RedisTools::get(const Json& args) {
    bool              present = false;
    const std::string key     = stringArg(args, "key", &present);
    if (!present) return failure("key is required");

    net::Reply  reply;
    std::string error;
    if (!run({"GET", key}, reply, error)) return failure("connection failed: " + error);
    if (reply.isError()) return failure(reply.str);

    bool binary = false;
    Json out    = Json::object();
    out.set("key", Json::string(key));
    out.set("value", replyToJson(reply, binary));
    out.set("exists", Json::boolean(!reply.isNull()));
    if (binary) out.set("binary", Json::boolean(true));
    return success(out);
}

ToolResult RedisTools::set(const Json& args) {
    if (read_only_) return failure("read-only mode: set is not available");

    bool              have_key = false, have_value = false;
    const std::string key   = stringArg(args, "key", &have_key);
    const std::string value = stringArg(args, "value", &have_value);
    if (!have_key) return failure("key is required");
    if (!have_value) return failure("value is required");

    std::vector<std::string> argv = {"SET", key, value};
    long                     ttl  = 0;
    if (longArg(args, "ttl_seconds", ttl)) {
        if (ttl <= 0) return failure("ttl_seconds must be positive");
        argv.push_back("EX");
        argv.push_back(std::to_string(ttl));
    }

    net::Reply  reply;
    std::string error;
    if (!run(argv, reply, error)) return failure("connection failed: " + error);
    if (reply.isError()) return failure(reply.str);

    Json out = Json::object();
    out.set("key", Json::string(key));
    out.set("ok", Json::boolean(true));
    if (ttl > 0) out.set("ttl_seconds", Json::number(static_cast<double>(ttl)));
    return success(out);
}

ToolResult RedisTools::scanKeys(const Json& args) {
    bool              has_pattern = false;
    const std::string pattern     = stringArg(args, "pattern", &has_pattern);

    long limit = kScanDefault;
    if (longArg(args, "count", limit)) {
        if (limit <= 0) return failure("count must be positive");
        if (limit > kScanHardCap) limit = kScanHardCap;
    }

    std::string cursor    = "0";
    bool        completed = false;
    bool        truncated = false;
    Json        keys      = Json::array();
    bool        binary    = false;
    long        pages     = 0;

    do {
        std::vector<std::string> argv = {"SCAN", cursor, "COUNT", std::to_string(kScanPageCount)};
        if (has_pattern && !pattern.empty()) {
            argv.push_back("MATCH");
            argv.push_back(pattern);
        }

        net::Reply  reply;
        std::string error;
        if (!run(argv, reply, error)) return failure("connection failed: " + error);
        if (reply.isError()) return failure(reply.str);
        if (reply.elements.size() != 2) return failure("malformed SCAN reply");

        cursor = reply.elements[0].str;
        for (const net::Reply& k : reply.elements[1].elements) {
            if (static_cast<long>(keys.elements().size()) >= limit) {
                truncated = true;
                break;
            }
            if (!isValidUtf8(k.str)) binary = true;
            keys.push(Json::string(sanitiseUtf8(k.str)));
        }
        completed = cursor == "0";
    } while (!completed && static_cast<long>(keys.elements().size()) < limit &&
             ++pages < kScanMaxPages);

    const long returned = static_cast<long>(keys.elements().size());
    Json       out      = Json::object();
    out.set("pattern", Json::string(has_pattern && !pattern.empty() ? pattern : "*"));
    out.set("count", Json::number(static_cast<double>(returned)));
    // The distinction matters to a model deciding whether to narrow its
    // pattern: complete means these are all of them. A walk that reached the
    // end of the keyspace but dropped keys to stay under `count` has not.
    out.set("complete", Json::boolean(completed && !truncated));
    out.set("keys", std::move(keys));
    if (binary) out.set("binary", Json::boolean(true));
    return success(out);
}

ToolResult RedisTools::describeKey(const Json& args) {
    bool              present = false;
    const std::string key     = stringArg(args, "key", &present);
    if (!present) return failure("key is required");

    net::Reply  reply;
    std::string error;
    if (!run({"TYPE", key}, reply, error)) return failure("connection failed: " + error);
    if (reply.isError()) return failure(reply.str);
    const std::string type = reply.str;

    Json out = Json::object();
    out.set("key", Json::string(key));
    if (type == "none") {
        out.set("exists", Json::boolean(false));
        return success(out);
    }
    out.set("exists", Json::boolean(true));
    out.set("type", Json::string(type));

    if (!run({"OBJECT", "ENCODING", key}, reply, error)) {
        return failure("connection failed: " + error);
    }
    if (!reply.isError() && !reply.isNull()) out.set("encoding", Json::string(reply.str));

    // A key with no TTL answers -1 and one that has just gone answers -2. Both
    // are more useful spelled out than passed through as a magic number.
    if (!run({"PTTL", key}, reply, error)) return failure("connection failed: " + error);
    if (!reply.isError()) {
        const std::int64_t pttl = reply.integer;
        out.set("volatile", Json::boolean(pttl >= 0));
        if (pttl >= 0) {
            out.set("ttl_ms", Json::number(static_cast<double>(pttl)));
            out.set("ttl_seconds", Json::number(static_cast<double>(pttl) / 1000.0));
        }
    }

    if (const char* size_command = sizeCommandFor(type)) {
        if (!run({size_command, key}, reply, error)) {
            return failure("connection failed: " + error);
        }
        if (!reply.isError()) {
            out.set("size", Json::number(static_cast<double>(reply.integer)));
            out.set("size_command", Json::string(size_command));
        }
    }

    return success(out);
}

ToolResult RedisTools::serverInfo(const Json& args) {
    bool              has_section = false;
    const std::string section     = stringArg(args, "section", &has_section);

    std::vector<std::string> argv = {"INFO"};
    if (has_section && !section.empty()) argv.push_back(section);

    net::Reply  reply;
    std::string error;
    if (!run(argv, reply, error)) return failure("connection failed: " + error);
    if (reply.isError()) return failure(reply.str);

    // INFO is one blob of "# Section" headers and "key:value" lines. Splitting
    // it here is the whole point of the tool -- a model should not have to.
    Json        sections = Json::object();
    Json        current  = Json::object();
    std::string name     = "server";

    const std::string   body = sanitiseUtf8(reply.str);
    std::string::size_type at = 0;
    const auto flush = [&] {
        if (!current.members().empty()) sections.set(name, current);
        current = Json::object();
    };

    while (at <= body.size()) {
        const std::string::size_type end  = body.find('\n', at);
        std::string_view             line = std::string_view(body).substr(
            at, (end == std::string::npos ? body.size() : end) - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        at = end == std::string::npos ? body.size() + 1 : end + 1;

        if (line.empty()) continue;
        if (line.front() == '#') {
            flush();
            std::string_view heading = line.substr(1);
            while (!heading.empty() && heading.front() == ' ') heading.remove_prefix(1);
            name = core::toLower(heading);
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        current.set(std::string(line.substr(0, colon)),
                    Json::string(std::string(line.substr(colon + 1))));
    }
    flush();

    Json out = Json::object();
    if (has_section && !section.empty()) out.set("section", Json::string(section));
    out.set("sections", std::move(sections));
    return success(out);
}

}  // namespace mnemos::mcp
