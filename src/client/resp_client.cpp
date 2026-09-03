#include "client/resp_client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mnemos::client {
namespace {

// The read buffer is compacted only once a whole reply has been consumed, so a
// reply split across reads is never rescanned from a moved base.
constexpr std::size_t kReadChunk = 16 * 1024;

std::string systemError(std::string_view what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

RespClient::~RespClient() { close(); }

void RespClient::adopt(int fd) {
    close();
    fd_ = fd;
    buf_.clear();
    pos_      = 0;
    protover_ = 2;
}

void RespClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buf_.clear();
    pos_ = 0;
}

bool RespClient::connect(const std::string& host, int port, std::string& error) {
    close();

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string service = std::to_string(port);
    addrinfo*         list    = nullptr;
    const int         rc      = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &list);
    if (rc != 0) {
        error = "cannot resolve " + host + ": " + ::gai_strerror(rc);
        return false;
    }

    // Try every address the resolver offered: a host with both an AAAA and an
    // A record fails on the first when the machine has no IPv6 route.
    for (addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        timeval tv{};
        tv.tv_sec  = timeout_ms_ / 1000;
        tv.tv_usec = (timeout_ms_ % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            ::freeaddrinfo(list);
            fd_ = fd;
            return true;
        }
        error = systemError("cannot connect to " + host + ":" + service);
        ::close(fd);
    }

    ::freeaddrinfo(list);
    if (error.empty()) error = "cannot connect to " + host + ":" + service;
    return false;
}

bool RespClient::sendAll(std::string_view bytes, std::string& error) {
    while (!bytes.empty()) {
        const ssize_t n = ::send(fd_, bytes.data(), bytes.size(), 0);
        if (n > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        error = n == 0 ? "connection closed while sending" : systemError("send failed");
        close();
        return false;
    }
    return true;
}

bool RespClient::readReply(net::Reply& out, std::string& error) {
    if (fd_ < 0) {
        error = "not connected";
        return false;
    }

    for (;;) {
        std::string parse_error;
        if (net::parseReply(buf_, pos_, out, parse_error)) {
            // A whole reply is consumed, so the prefix can go. Pipelined bytes
            // that arrived with it are kept for the next call.
            buf_.erase(0, pos_);
            pos_ = 0;
            return true;
        }
        if (!parse_error.empty()) {
            error = parse_error;
            close();
            return false;
        }

        char          chunk[kReadChunk];
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buf_.append(chunk, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        error = n == 0 ? "connection closed by peer" : systemError("read failed");
        close();
        return false;
    }
}

bool RespClient::command(const std::vector<std::string>& argv, net::Reply& out,
                         std::string& error) {
    if (fd_ < 0) {
        error = "not connected";
        return false;
    }
    if (!sendAll(net::encodeCommand(argv), error)) return false;
    return readReply(out, error);
}

bool RespClient::negotiateResp3(std::string& error) {
    net::Reply reply;
    if (!command({"HELLO", "3"}, reply, error)) return false;
    // An error reply here is not a failure of the link -- it means RESP2 is all
    // this peer has, which is a perfectly usable outcome.
    protover_ = reply.isError() ? 2 : 3;
    return true;
}

}  // namespace mnemos::client
