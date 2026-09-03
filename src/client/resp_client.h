// A blocking RESP client -- the other half of src/net/resp.h.
//
// The server parses requests and writes replies; this writes requests and
// parses replies, reusing net::parseReply and net::encodeCommand rather than
// carrying a second copy of the framing. It lives in src/client/ (inside the
// mnemos_core glob) rather than inside mnemos-mcp because the replica link
// needs exactly this, and a second copy would be a second place for a framing
// bug to live.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "net/resp.h"

namespace mnemos::client {

class RespClient {
public:
    RespClient() = default;
    ~RespClient();

    RespClient(const RespClient&)            = delete;
    RespClient& operator=(const RespClient&) = delete;

    // Resolves `host` and connects. Sets `error` and returns false on failure.
    bool connect(const std::string& host, int port, std::string& error);

    // Takes ownership of an already-connected socket. This is what lets the
    // unit tests drive both ends of a socketpair without a server or a thread.
    void adopt(int fd);

    void close();
    bool connected() const { return fd_ >= 0; }

    // Applies to both directions, and must be set before connect() to bound
    // the connect itself. A read that times out closes the connection: a
    // half-read reply cannot be resumed on the next command without desyncing
    // the stream permanently, and there is no way to tell "slow" from "stuck".
    void setTimeoutMs(int ms) { timeout_ms_ = ms; }

    // Sends argv and reads exactly one reply. An *error reply* is a successful
    // call -- `out.isError()` is true and this still returns true. False means
    // the link is unusable, and it has been closed.
    bool command(const std::vector<std::string>& argv, net::Reply& out, std::string& error);
    bool readReply(net::Reply& out, std::string& error);

    // HELLO 3, falling back to RESP2 when the peer rejects it -- a redis older
    // than 6 has no HELLO at all, and answers with an unknown-command error.
    bool negotiateResp3(std::string& error);
    int  protocolVersion() const { return protover_; }

private:
    bool sendAll(std::string_view bytes, std::string& error);

    int         fd_         = -1;
    int         timeout_ms_ = 5000;
    int         protover_   = 2;
    std::string buf_;      // bytes read but not yet consumed by a reply
    std::size_t pos_ = 0;  // parse cursor within buf_
};

}  // namespace mnemos::client
