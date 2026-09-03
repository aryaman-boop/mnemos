// The tools mnemos-mcp exposes, and the RESP-to-JSON mapping underneath them.
//
// Six: a generic escape hatch plus five curated ones. The split is deliberate
// -- the curated tools are what a model should reach for, and the escape hatch
// means the surface does not have to grow every time the command table does.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "client/resp_client.h"
#include "mcp/json.h"
#include "mcp/protocol.h"

namespace mnemos::mcp {

// Maps one reply onto JSON. RESP3 carries real types, so the mapping is close
// to free; under RESP2 everything arrives flattened and the result is poorer
// but still correct. `binary` is set when any string in the tree was not valid
// UTF-8 and had to be replaced with U+FFFD -- the caller surfaces that as a
// flag rather than emitting invalid JSON or silently lying about the bytes.
Json replyToJson(const net::Reply& reply, bool& binary);

class RedisTools : public ToolProvider {
public:
    RedisTools(std::string host, int port, int db, int timeout_ms, bool read_only);

    Json       listTools() const override;
    bool       hasTool(std::string_view name) const override;
    ToolResult callTool(const std::string& name, const Json& arguments) override;

    // Connects and negotiates RESP3 up front so a misconfigured host is
    // reported at startup rather than on the first tool call. A failure here is
    // not fatal: every call retries, so the server survives redis restarting.
    bool warmUp(std::string& error);

private:
    // Runs one command. False means the *link* failed and `error` says how; an
    // error reply from the server is a true return with `out.isError()`.
    bool run(const std::vector<std::string>& argv, net::Reply& out, std::string& error);
    bool ensureConnected(std::string& error);

    ToolResult redisCommand(const Json& args);
    ToolResult get(const Json& args);
    ToolResult set(const Json& args);
    ToolResult scanKeys(const Json& args);
    ToolResult describeKey(const Json& args);
    ToolResult serverInfo(const Json& args);

    std::string        host_;
    int                port_;
    int                db_;
    int                timeout_ms_;
    bool               read_only_;
    client::RespClient conn_;
};

}  // namespace mnemos::mcp
