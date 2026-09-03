// JSON-RPC 2.0 framing and the MCP method handlers.
//
// The transport is stdio, one JSON message per line -- not LSP's Content-Length
// framing. Nothing in this file touches a socket or knows what redis is; it
// takes a line in and hands a line back, which is what makes the whole protocol
// layer testable without a server.
//
// The server is *dual-era*. MCP revision 2026-07-28 replaced the `initialize`
// handshake with per-request metadata: every request declares its version in
// `params._meta`, and `server/discover` reports what the server supports.
// Older revisions ("legacy", 2025-11-25 and earlier) open with `initialize`.
// A request that carries a modern `_meta` protocol version is served under the
// modern rules; anything else is served as legacy. The spec's compatibility
// matrix has a dual-era server working against both kinds of client, which is
// the only combination that does.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mcp/json.h"

namespace mnemos::mcp {

// What a tool call produced. A tool that *ran* and failed is still a successful
// JSON-RPC result -- `is_error` rides inside it, so the model sees the failure
// text and can adapt. Only a malformed request is a JSON-RPC error.
struct ToolResult {
    std::string text;
    bool        is_error = false;
};

// Everything the protocol layer needs from the tool layer.
class ToolProvider {
public:
    virtual ~ToolProvider() = default;

    // A JSON array of {name, description, inputSchema}.
    virtual Json       listTools() const                                          = 0;
    virtual bool       hasTool(std::string_view name) const                       = 0;
    virtual ToolResult callTool(const std::string& name, const Json& arguments)   = 0;
};

// JSON-RPC and MCP error codes. The -32020 block is reserved to the MCP spec;
// -32022 is the one a version mismatch uses.
namespace rpc {
inline constexpr int kParseError     = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams  = -32602;
inline constexpr int kUnsupportedProtocolVersion = -32022;
}  // namespace rpc

inline constexpr std::string_view kServerName    = "mnemos-mcp";
inline constexpr std::string_view kServerVersion = "0.1.0";

// Newest first, which is also the order `server/discover` reports them in and
// the order `initialize` falls back through.
const std::vector<std::string>& modernProtocolVersions();
const std::vector<std::string>& legacyProtocolVersions();

class Session {
public:
    explicit Session(ToolProvider& tools) : tools_(tools) {}

    // One inbound line in, one outbound line out (without its newline). An
    // empty return means write nothing: the message was a notification, and a
    // notification must never be answered.
    std::string handleLine(std::string_view line);

private:
    ToolProvider& tools_;
};

}  // namespace mnemos::mcp
