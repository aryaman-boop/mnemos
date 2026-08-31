#include "server/server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#include "server/command_table.h"

namespace mnemos::server {
namespace {

constexpr std::size_t kReadChunkSize = 16 * 1024;
// Once the parsed prefix exceeds this, compact the buffer. Deferring the
// memmove keeps a deep pipeline at one copy rather than one copy per command.
constexpr std::size_t kQueryBufferTrimThreshold = 32 * 1024;
constexpr int kServerCronIntervalMs = 100;  // Redis's default hz = 10
// How many bytes of arguments to quote back in an unknown-command error.
constexpr std::size_t kUnknownCommandArgsLimit = 128;

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags != -1 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

void setTcpNoDelay(int fd) {
    // Redis disables Nagle: request/response traffic is latency-sensitive and
    // coalescing small replies would add up to 40ms per round trip.
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

std::string generateRunId() {
    static const char* kHex = "0123456789abcdef";
    std::mt19937_64 gen{std::random_device{}()};
    std::string id;
    id.reserve(40);
    for (int i = 0; i < 40; ++i) id.push_back(kHex[gen() & 0xF]);
    return id;
}

}  // namespace

void Client::trimQueryBuffer() {
    if (query_pos_ == 0) return;
    if (query_pos_ == query_buffer_.size()) {
        query_buffer_.clear();
        query_pos_ = 0;
        return;
    }
    if (query_pos_ >= kQueryBufferTrimThreshold) {
        query_buffer_.erase(0, query_pos_);
        query_pos_ = 0;
    }
}

Server::Server(Config config) : config_(std::move(config)) {
    databases_.reserve(static_cast<std::size_t>(config_.databases));
    for (int i = 0; i < config_.databases; ++i) {
        databases_.push_back(std::make_unique<Database>(i));
    }
    run_id_ = generateRunId();
}

Server::~Server() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    for (auto& [fd, client] : clients_) ::close(fd);
}

bool Server::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "mnemos: socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<std::uint16_t>(config_.port));
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "mnemos: invalid bind address '%s'\n", config_.bind_address.c_str());
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "mnemos: bind %s:%d failed: %s\n", config_.bind_address.c_str(),
                     config_.port, std::strerror(errno));
        return false;
    }
    if (::listen(listen_fd_, 511) < 0) {  // 511: Redis's default tcp-backlog
        std::fprintf(stderr, "mnemos: listen() failed: %s\n", std::strerror(errno));
        return false;
    }
    if (!setNonBlocking(listen_fd_)) {
        std::fprintf(stderr, "mnemos: could not set listener non-blocking\n");
        return false;
    }

    loop_.addFd(listen_fd_, net::Ev::Read, [this](int, net::Ev) { onAcceptable(); });
    loop_.addTimer(std::chrono::milliseconds(kServerCronIntervalMs), [this] { serverCron(); });

    start_time_ms_ = net::EventLoop::currentTimeMs();
    std::printf("mnemos %s ready to accept connections on %s:%d\n", "0.1.0",
                config_.bind_address.c_str(), config_.port);
    std::fflush(stdout);
    return true;
}

void Server::run() { loop_.run(); }
void Server::stop() { loop_.stop(); }

void Server::onAcceptable() {
    // Drain the backlog: level-triggered polling would re-arm anyway, but
    // accepting in a loop avoids one wakeup per pending connection.
    while (true) {
        sockaddr_in peer{};
        socklen_t   peer_len = sizeof(peer);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }

        if (clients_.size() >= config_.max_clients) {
            const char* msg = "-ERR max number of clients reached\r\n";
            ::send(fd, msg, std::strlen(msg), 0);
            ::close(fd);
            ++stats_.rejected_connections;
            continue;
        }

        setNonBlocking(fd);
        setTcpNoDelay(fd);

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::string addr = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

        auto client = std::make_unique<Client>(fd, next_client_id_++, std::move(addr));
        client->setCreatedAtMs(loop_.nowMs());
        client->setLastInteractionMs(loop_.nowMs());
        client->setAuthenticated(config_.requirepass.empty());
        clients_.emplace(fd, std::move(client));
        ++stats_.connections_received;

        loop_.addFd(fd, net::Ev::Read, [this](int cfd, net::Ev fired) {
            onClientEvent(cfd, fired);
        });
    }
}

void Server::onClientEvent(int fd, net::Ev fired) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    Client& client = *it->second;

    if (any(fired & net::Ev::Write)) {
        flushOutput(client);
        if (clients_.find(fd) == clients_.end()) return;  // flush closed it
    }
    if (any(fired & net::Ev::Read)) {
        readFromClient(client);
    }

    // Deferred closes: a handler may have asked to drop a connection that is
    // still on the stack above us, so we only actually free it here.
    for (int doomed : pending_close_) {
        auto doomed_it = clients_.find(doomed);
        if (doomed_it == clients_.end()) continue;
        loop_.removeFd(doomed);
        ::close(doomed);
        clients_.erase(doomed_it);
    }
    pending_close_.clear();
}

void Server::readFromClient(Client& client) {
    const int fd = client.fd();
    char buffer[kReadChunkSize];

    while (true) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            client.queryBuffer().append(buffer, static_cast<std::size_t>(n));
            stats_.total_net_input += static_cast<std::uint64_t>(n);
            client.setLastInteractionMs(loop_.nowMs());
            processInputBuffer(client);
            if (clients_.find(fd) == clients_.end()) return;
            // A short read means the socket buffer is drained; going round again
            // would just cost a syscall that returns EAGAIN.
            if (static_cast<std::size_t>(n) < sizeof(buffer)) break;
            continue;
        }
        if (n == 0) {  // orderly shutdown by the peer
            closeClient(fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        closeClient(fd);
        return;
    }

    client.trimQueryBuffer();
    flushOutput(client);
}

void Server::processInputBuffer(Client& client) {
    std::vector<std::string> argv;
    std::string              error;

    while (client.queryPos() < client.queryBuffer().size()) {
        argv.clear();
        const auto status = client.parser().parse(client.queryBuffer(), client.queryPos(),
                                                  argv, error);
        if (status == net::RequestParser::Status::Incomplete) break;

        if (status == net::RequestParser::Status::Error) {
            net::ReplyWriter writer(client.outputBuffer(), client.protocolVersion());
            writer.error(error);
            client.closeAfterReply();
            flushOutput(client);
            closeClient(client.fd());
            return;
        }

        if (argv.empty()) continue;  // blank inline line: ignore, as Redis does
        dispatch(client, argv);
        if (client.shouldClose()) {
            flushOutput(client);
            closeClient(client.fd());
            return;
        }
    }
}

void Server::dispatch(Client& client, std::vector<std::string>& argv) {
    net::ReplyWriter writer(client.outputBuffer(), client.protocolVersion());

    const CommandSpec* spec = lookupCommand(argv[0]);
    if (!spec) {
        // Redis echoes the command *as the client spelled it*, then quotes the
        // leading arguments until roughly 128 bytes have been emitted. Matching
        // this exactly matters: client libraries parse these strings.
        std::string args;
        for (std::size_t i = 1; i < argv.size() && args.size() < kUnknownCommandArgsLimit; ++i) {
            args += "'";
            args.append(argv[i], 0, kUnknownCommandArgsLimit - args.size());
            args += "' ";
        }
        writer.error("ERR unknown command '" + argv[0] +
                     "', with args beginning with: " + args);
        return;
    }

    if (!arityOk(*spec, argv.size())) {
        // The canonical (lower-case) name is used here, not the client's
        // spelling -- `GeT` and `get` both report "'get'".
        replies::wrongArgs(writer, spec->name);
        return;
    }

    if (!config_.requirepass.empty() && !client.authenticated() &&
        !(spec->flags & flags::kNoAuth)) {
        writer.error("NOAUTH Authentication required.");
        return;
    }

    client.setLastCommand(std::string(spec->name));

    CommandContext ctx{
        .server = *this,
        .client = client,
        .db     = db(client.dbIndex()),
        .argv   = argv,
        .reply  = writer,
    };

    const int previous_executing = executing_fd_;
    executing_fd_ = client.fd();
    spec->handler(ctx);
    executing_fd_ = previous_executing;

    ++stats_.commands_processed;
}

void Server::flushOutput(Client& client) {
    const int fd = client.fd();

    while (client.hasPendingOutput()) {
        const char*       data = client.outputBuffer().data() + client.outputSent();
        const std::size_t len  = client.outputBuffer().size() - client.outputSent();
        const ssize_t     n    = ::write(fd, data, len);

        if (n > 0) {
            client.outputSent() += static_cast<std::size_t>(n);
            stats_.total_net_output += static_cast<std::uint64_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Kernel buffer is full. Ask to be told when it drains rather than
            // spinning -- this is what stops a slow consumer from burning CPU.
            loop_.modFd(fd, net::Ev::Read | net::Ev::Write);
            return;
        }
        if (n < 0 && errno == EINTR) continue;
        closeClient(fd);
        return;
    }

    client.outputBuffer().clear();
    client.outputSent() = 0;
    loop_.modFd(fd, net::Ev::Read);

    if (client.shouldClose()) closeClient(fd);
}

void Server::closeClient(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    if (fd == executing_fd_) {
        // We are inside this client's own command handler; tearing the object
        // down now would leave the caller holding a dangling reference.
        it->second->closeAfterReply();
        pending_close_.push_back(fd);
        return;
    }

    loop_.removeFd(fd);
    ::close(fd);
    clients_.erase(it);
}

void Server::serverCron() {
    const std::int64_t now = loop_.nowMs();

    for (auto& database : databases_) {
        const int expired = database->activeExpireCycle(now);
        stats_.expired_keys += static_cast<std::uint64_t>(expired);
    }

    // Keep migrating buckets even when no commands are arriving, so an idle
    // server finishes a resize instead of leaving two tables live indefinitely.
    for (auto& database : databases_) {
        database->incrementalRehash(100);
    }
}

}  // namespace mnemos::server
