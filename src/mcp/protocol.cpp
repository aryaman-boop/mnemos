#include "mcp/protocol.h"

#include <algorithm>

namespace mnemos::mcp {
namespace {

constexpr std::string_view kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view kMetaClientCaps      = "io.modelcontextprotocol/clientCapabilities";
constexpr std::string_view kMetaServerInfo      = "io.modelcontextprotocol/serverInfo";

constexpr std::string_view kInstructions =
    "Inspect and operate a mnemos (or redis) keyspace. Prefer the specific "
    "tools -- get, set, scan_keys, describe_key, server_info -- and fall back "
    "to redis_command for anything they do not cover. describe_key reports the "
    "in-memory encoding, which is how you tell a listpack from a hashtable.";

Json serverInfo() {
    Json info = Json::object();
    info.set("name", Json::string(std::string(kServerName)));
    info.set("version", Json::string(std::string(kServerVersion)));
    return info;
}

Json toolCapabilities() {
    Json tools = Json::object();
    tools.set("listChanged", Json::boolean(false));
    Json caps = Json::object();
    caps.set("tools", std::move(tools));
    return caps;
}

// An id is echoed back exactly as it arrived -- string or number -- because a
// client matches responses on identity, not on value.
Json envelope(const Json& id) {
    Json out = Json::object();
    out.set("jsonrpc", Json::string("2.0"));
    out.set("id", id);
    return out;
}

std::string errorResponse(const Json& id, int code, std::string_view message,
                          Json data = Json::null()) {
    Json err = Json::object();
    err.set("code", Json::number(static_cast<double>(code)));
    err.set("message", Json::string(std::string(message)));
    if (!data.isNull()) err.set("data", std::move(data));

    Json out = envelope(id);
    out.set("error", std::move(err));
    return out.serialise();
}

// A modern result carries `resultType` and identifies the server in `_meta`; a
// legacy one carries neither. Everything else about the shape is shared.
std::string resultResponse(const Json& id, Json result, bool modern) {
    if (modern) {
        Json meta = Json::object();
        meta.set(std::string(kMetaServerInfo), serverInfo());
        result.set("_meta", std::move(meta));

        // resultType leads the object: it is what tells a client how to read
        // the rest, and a stable key order makes the wire output diffable.
        Json ordered = Json::object();
        ordered.set("resultType", Json::string("complete"));
        for (const Json::Member& m : result.members()) ordered.set(m.first, m.second);
        result = std::move(ordered);
    }
    Json out = envelope(id);
    out.set("result", std::move(result));
    return out.serialise();
}

const Json* metaField(const Json* params, std::string_view key) {
    if (params == nullptr) return nullptr;
    const Json* meta = params->find("_meta");
    if (meta == nullptr) return nullptr;
    return meta->find(key);
}

Json stringArray(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const std::string& v : values) out.push(Json::string(v));
    return out;
}

}  // namespace

const std::vector<std::string>& modernProtocolVersions() {
    static const std::vector<std::string> versions = {"2026-07-28"};
    return versions;
}

const std::vector<std::string>& legacyProtocolVersions() {
    static const std::vector<std::string> versions = {"2025-11-25", "2025-06-18", "2025-03-26",
                                                      "2024-11-05"};
    return versions;
}

std::string Session::handleLine(std::string_view line) {
    // A blank line is not a message. Skipping it keeps a stray newline from
    // producing a parse error nobody asked about.
    if (line.find_first_not_of(" \t\r\n") == std::string_view::npos) return {};

    Json        message;
    std::string parse_error;
    if (!Json::parse(line, message, parse_error)) {
        return errorResponse(Json::null(), rpc::kParseError, "Parse error: " + parse_error);
    }
    if (!message.isObject()) {
        return errorResponse(Json::null(), rpc::kInvalidRequest,
                             "Invalid Request: expected a JSON object");
    }

    const Json* id_field = message.find("id");
    // JSON-RPC says a notification has no id; MCP additionally forbids a null
    // one on a request, so both spellings mean "do not answer this".
    const bool  is_notification = id_field == nullptr || id_field->isNull();
    const Json  id              = is_notification ? Json::null() : *id_field;

    const Json* method_field = message.find("method");
    if (method_field == nullptr || !method_field->isString()) {
        if (is_notification) return {};
        return errorResponse(id, rpc::kInvalidRequest, "Invalid Request: missing method");
    }
    const std::string& method = method_field->asString();
    const Json*        params = message.find("params");

    // Notifications: acknowledged by doing nothing, which is the whole of the
    // contract. `notifications/initialized` is the one a legacy client sends.
    if (is_notification) return {};

    // ------------------------------------------------------------------ era
    const Json* declared = metaField(params, kMetaProtocolVersion);
    const bool  modern   = declared != nullptr && declared->isString();

    if (modern) {
        const std::vector<std::string>& supported = modernProtocolVersions();
        if (std::find(supported.begin(), supported.end(), declared->asString()) ==
            supported.end()) {
            Json data = Json::object();
            data.set("supported", stringArray(supported));
            data.set("requested", *declared);
            return errorResponse(id, rpc::kUnsupportedProtocolVersion,
                                 "Unsupported protocol version", std::move(data));
        }
        // clientCapabilities is required on every modern request, and a server
        // must not assume a capability that was never declared.
        if (metaField(params, kMetaClientCaps) == nullptr) {
            return errorResponse(id, rpc::kInvalidParams,
                                 "Invalid params: _meta is missing " +
                                     std::string(kMetaClientCaps));
        }
    }

    // -------------------------------------------------------------- methods
    if (method == "initialize") {
        // Legacy only, and deliberately so: `initialize` is what selects legacy
        // semantics for the rest of the process.
        const Json*        asked = params != nullptr ? params->find("protocolVersion") : nullptr;
        const std::vector<std::string>& supported = legacyProtocolVersions();
        std::string version = supported.front();
        if (asked != nullptr && asked->isString() &&
            std::find(supported.begin(), supported.end(), asked->asString()) != supported.end()) {
            version = asked->asString();  // echo what the client can speak
        }

        Json result = Json::object();
        result.set("protocolVersion", Json::string(version));
        result.set("capabilities", toolCapabilities());
        result.set("serverInfo", serverInfo());
        result.set("instructions", Json::string(std::string(kInstructions)));
        return resultResponse(id, std::move(result), false);
    }

    if (method == "server/discover") {
        std::vector<std::string> all = modernProtocolVersions();
        const std::vector<std::string>& legacy = legacyProtocolVersions();
        all.insert(all.end(), legacy.begin(), legacy.end());

        Json result = Json::object();
        result.set("supportedVersions", stringArray(all));
        result.set("capabilities", toolCapabilities());
        result.set("instructions", Json::string(std::string(kInstructions)));
        // Discovery is the modern probe, so it always answers in modern shape
        // even when the probe itself carried no _meta.
        return resultResponse(id, std::move(result), true);
    }

    if (method == "ping") {
        return resultResponse(id, Json::object(), modern);
    }

    if (method == "tools/list") {
        Json result = Json::object();
        result.set("tools", tools_.listTools());
        return resultResponse(id, std::move(result), modern);
    }

    if (method == "tools/call") {
        const Json* name = params != nullptr ? params->find("name") : nullptr;
        if (name == nullptr || !name->isString()) {
            return errorResponse(id, rpc::kInvalidParams, "Invalid params: missing tool name");
        }
        if (!tools_.hasTool(name->asString())) {
            return errorResponse(id, rpc::kInvalidParams,
                                 "Invalid params: unknown tool '" + name->asString() + "'");
        }

        const Json* arguments = params->find("arguments");
        const Json  args      = arguments != nullptr && arguments->isObject() ? *arguments
                                                                             : Json::object();
        const ToolResult call = tools_.callTool(name->asString(), args);

        Json block = Json::object();
        block.set("type", Json::string("text"));
        block.set("text", Json::string(call.text));

        Json content = Json::array();
        content.push(std::move(block));

        Json result = Json::object();
        result.set("content", std::move(content));
        result.set("isError", Json::boolean(call.is_error));
        return resultResponse(id, std::move(result), modern);
    }

    return errorResponse(id, rpc::kMethodNotFound, "Method not found: " + method);
}

}  // namespace mnemos::mcp
