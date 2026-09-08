// mnemos-kafka entry point: flags, the listener, and the connection loop.
//
// Like mnemos-mcp, this is a *client* of mnemos-server rather than an embedding
// of it -- two processes must be running. The gain is the same one: the storage
// layer talks RESP through src/client/, so pointing this at a real redis-server
// works and costs nothing, and the log substrate can be swapped for streams
// without the protocol layer noticing.
//
// One thread, one event loop, and the RESP link underneath is blocking. A slow
// keyspace call therefore stalls the other connections for its duration, which
// is the same trade mnemos-mcp makes and is bounded by the socket timeout.
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "kafka/broker.h"
#include "kafka/log.h"
#include "net/event_loop.h"

namespace {

using mnemos::kafka::Broker;

// Kafka's own default cap on a request frame. A larger one is refused before a
// byte of it is buffered, so a bad length prefix cannot drive an allocation.
constexpr std::size_t kMaxFrameSize = 100u * 1024u * 1024u;
constexpr std::size_t kFrameHeader  = 4;

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// Per-connection state: an input buffer with a parse cursor, an output buffer
// with a flush cursor, and at most one parked fetch.
struct Conn {
    int          fd = -1;
    std::string  in;
    std::size_t  in_pos = 0;
    std::string  out;
    std::size_t  out_sent = 0;
    bool         close_after_write = false;

    // A fetch that asked to wait and found nothing. The frame is kept whole and
    // simply re-run: a fetch is a read, so replaying it is free of consequence,
    // which is what makes this a dozen lines instead of a saved-state machine.
    bool         parked = false;
    std::string  parked_frame;
    std::int64_t parked_deadline_ms = 0;

    bool hasPendingOutput() const { return out_sent < out.size(); }
};

class KafkaServer {
public:
    KafkaServer(mnemos::kafka::BrokerConfig config, mnemos::kafka::PartitionLog& log,
                std::string bind_address)
        : broker_(std::move(config), log), bind_address_(std::move(bind_address)) {}

    bool start();
    void run() { loop_.run(); }

private:
    void onAcceptable();
    void onReadable(Conn& conn);
    void onWritable(Conn& conn);
    void drainRequests(Conn& conn);
    void pollParked();
    // Applies one outcome to the connection: queues bytes, parks, or marks it
    // for closing. Returns false when the connection is gone.
    bool applyOutcome(Conn& conn, Broker::Outcome outcome, std::string frame);
    void queue(Conn& conn, std::string_view bytes);
    void closeConn(Conn& conn);

    Broker                              broker_;
    mnemos::net::EventLoop              loop_;
    std::string                         bind_address_;
    int                                 listen_fd_ = -1;
    std::unordered_map<int, Conn>       conns_;
};

bool KafkaServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "mnemos-kafka: socket() failed: %s\n", std::strerror(errno));
        return false;
    }
    const int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<std::uint16_t>(broker_.config().advertised_port));
    if (::inet_pton(AF_INET, bind_address_.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "mnemos-kafka: invalid bind address '%s'\n", bind_address_.c_str());
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "mnemos-kafka: bind %s:%d failed: %s\n", bind_address_.c_str(),
                     broker_.config().advertised_port, std::strerror(errno));
        return false;
    }
    if (::listen(listen_fd_, 511) < 0) {
        std::fprintf(stderr, "mnemos-kafka: listen() failed: %s\n", std::strerror(errno));
        return false;
    }
    if (!setNonBlocking(listen_fd_)) return false;

    loop_.addFd(listen_fd_, mnemos::net::Ev::Read, [this](int, mnemos::net::Ev) { onAcceptable(); });
    // One timer for every parked fetch rather than one per connection: the work
    // per tick is proportional to the parked set, which is usually empty.
    loop_.addTimer(std::chrono::milliseconds(10), [this] { pollParked(); });
    return true;
}

void KafkaServer::onAcceptable() {
    // Accept in a loop: one wakeup can cover several pending connections.
    while (true) {
        sockaddr_in peer{};
        socklen_t   peer_len = sizeof(peer);
        const int   fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            return;
        }
        setNonBlocking(fd);
        const int on = 1;
        // Kafka is request/response over a long-lived connection; Nagle would
        // add up to 40ms to every small reply for no gain in throughput.
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        Conn& conn = conns_[fd];
        conn.fd    = fd;
        loop_.addFd(fd, mnemos::net::Ev::Read, [this, fd](int, mnemos::net::Ev fired) {
            const auto it = conns_.find(fd);
            if (it == conns_.end()) return;
            if (any(fired & mnemos::net::Ev::Read)) onReadable(it->second);
            const auto still = conns_.find(fd);
            if (still != conns_.end() && any(fired & mnemos::net::Ev::Write)) {
                onWritable(still->second);
            }
        });
    }
}

void KafkaServer::onReadable(Conn& conn) {
    char buffer[65536];
    while (true) {
        const ssize_t n = ::read(conn.fd, buffer, sizeof(buffer));
        if (n > 0) {
            conn.in.append(buffer, static_cast<std::size_t>(n));
            if (static_cast<std::size_t>(n) < sizeof(buffer)) break;
            continue;
        }
        if (n == 0) {  // peer closed
            closeConn(conn);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        closeConn(conn);
        return;
    }
    drainRequests(conn);
}

void KafkaServer::drainRequests(Conn& conn) {
    // A parked fetch holds the pipeline: responses must come back in the order
    // the requests arrived, so nothing behind it may be answered first.
    while (!conn.parked && !conn.close_after_write) {
        if (conn.in.size() - conn.in_pos < kFrameHeader) break;

        const auto* p = reinterpret_cast<const unsigned char*>(conn.in.data() + conn.in_pos);
        const auto  size = (static_cast<std::uint32_t>(p[0]) << 24) |
                          (static_cast<std::uint32_t>(p[1]) << 16) |
                          (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        if (size == 0 || size > kMaxFrameSize) {
            std::fprintf(stderr, "mnemos-kafka: refusing a %u byte frame\n", size);
            closeConn(conn);
            return;
        }
        if (conn.in.size() - conn.in_pos - kFrameHeader < size) break;

        std::string frame(conn.in, conn.in_pos + kFrameHeader, size);
        conn.in_pos += kFrameHeader + size;

        // Sequenced deliberately: writing this as one call would leave the
        // order of `handleRequest(frame)` and the move of `frame` into the
        // parameter unspecified, and a compiler that evaluates right to left
        // then hands the broker an empty frame.
        Broker::Outcome outcome = broker_.handleRequest(frame, true);
        if (!applyOutcome(conn, std::move(outcome), std::move(frame))) return;
    }

    // Compact once the cursor has run well ahead, rather than on every frame.
    if (conn.in_pos > 0 && (conn.in_pos == conn.in.size() || conn.in_pos > 65536)) {
        conn.in.erase(0, conn.in_pos);
        conn.in_pos = 0;
    }
}

bool KafkaServer::applyOutcome(Conn& conn, Broker::Outcome outcome, std::string frame) {
    switch (outcome.kind) {
        case Broker::Outcome::Kind::Respond:
            queue(conn, outcome.response);
            return true;
        case Broker::Outcome::Kind::Silence:
            return true;
        case Broker::Outcome::Kind::Retry:
            conn.parked       = true;
            conn.parked_frame = std::move(frame);
            conn.parked_deadline_ms = mnemos::net::EventLoop::currentTimeMs() +
                                      outcome.max_wait_ms;
            return true;
        case Broker::Outcome::Kind::Close:
            closeConn(conn);
            return false;
    }
    return true;
}

void KafkaServer::pollParked() {
    const std::int64_t now = mnemos::net::EventLoop::currentTimeMs();

    // Collect first: answering a parked fetch can close its connection, and
    // erasing from conns_ while iterating it would invalidate the cursor.
    std::vector<int> ready;
    for (const auto& [fd, conn] : conns_) {
        if (conn.parked) ready.push_back(fd);
    }

    for (const int fd : ready) {
        const auto it = conns_.find(fd);
        if (it == conns_.end()) continue;
        Conn& conn = it->second;

        // Past the deadline the fetch is answered whatever it found, which for
        // a caught-up consumer is an empty batch -- exactly what it expects.
        const bool allow_wait = now < conn.parked_deadline_ms;
        auto outcome = broker_.handleRequest(conn.parked_frame, allow_wait);
        if (outcome.kind == Broker::Outcome::Kind::Retry && allow_wait) continue;

        conn.parked = false;
        std::string frame = std::move(conn.parked_frame);
        conn.parked_frame.clear();
        if (!applyOutcome(conn, std::move(outcome), std::move(frame))) continue;
        // Requests that queued up behind the parked one can now be answered.
        drainRequests(conn);
    }
}

void KafkaServer::queue(Conn& conn, std::string_view bytes) {
    conn.out.append(bytes);
    onWritable(conn);
}

void KafkaServer::onWritable(Conn& conn) {
    while (conn.hasPendingOutput()) {
        const ssize_t n = ::write(conn.fd, conn.out.data() + conn.out_sent,
                                  conn.out.size() - conn.out_sent);
        if (n > 0) {
            conn.out_sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            loop_.modFd(conn.fd, mnemos::net::Ev::Read | mnemos::net::Ev::Write);
            return;
        }
        closeConn(conn);
        return;
    }

    conn.out.clear();
    conn.out_sent = 0;
    loop_.modFd(conn.fd, mnemos::net::Ev::Read);
    if (conn.close_after_write) closeConn(conn);
}

void KafkaServer::closeConn(Conn& conn) {
    const int fd = conn.fd;
    if (fd < 0) return;
    loop_.removeFd(fd);
    ::close(fd);
    conns_.erase(fd);
}

void printUsage() {
    std::fprintf(stderr,
        "mnemos-kafka 0.1.0 -- a Kafka broker over a mnemos or redis keyspace\n"
        "\n"
        "Usage: mnemos-kafka [options]\n"
        "\n"
        "  --bind <addr>       Address to listen on (default 127.0.0.1)\n"
        "  --port <n>          Kafka listener port (default 9092)\n"
        "  --advertised <addr> Host given to clients in Metadata (default --bind)\n"
        "  --node-id <n>       Broker node id (default 1)\n"
        "  --cluster-id <s>    Cluster id reported in Metadata\n"
        "  --partitions <n>    Partitions for an auto-created topic (default 1)\n"
        "  --no-auto-create    Refuse to create a topic on first produce\n"
        "  --redis-host <addr> mnemos/redis server (default 127.0.0.1)\n"
        "  --redis-port <n>    mnemos/redis port (default 6380)\n"
        "  --db <n>            Database to SELECT after connecting (default 0)\n"
        "  --timeout <ms>      Keyspace socket timeout (default 5000)\n"
        "  --help              Show this message\n"
        "\n"
        "Speaks the Kafka protocol: Produce, Fetch, ListOffsets, Metadata,\n"
        "ApiVersions. Consumer groups are not implemented -- assign partitions\n"
        "and track offsets client-side.\n");
}

bool takeValue(int argc, char** argv, int& i, const char* flag, std::string& out) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "mnemos-kafka: %s requires a value\n", flag);
        return false;
    }
    out = argv[++i];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    mnemos::kafka::BrokerConfig config;
    std::string                 bind_address = "127.0.0.1";
    std::string                 advertised;
    std::string                 redis_host = "127.0.0.1";
    int                         redis_port = 6380;
    int                         db         = 0;
    int                         timeout    = 5000;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string            value;

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--bind") {
            if (!takeValue(argc, argv, i, "--bind", value)) return 1;
            bind_address = value;
        } else if (arg == "--advertised") {
            if (!takeValue(argc, argv, i, "--advertised", value)) return 1;
            advertised = value;
        } else if (arg == "--port") {
            if (!takeValue(argc, argv, i, "--port", value)) return 1;
            config.advertised_port = std::atoi(value.c_str());
            if (config.advertised_port <= 0 || config.advertised_port > 65535) {
                std::fprintf(stderr, "mnemos-kafka: invalid port '%s'\n", value.c_str());
                return 1;
            }
        } else if (arg == "--node-id") {
            if (!takeValue(argc, argv, i, "--node-id", value)) return 1;
            config.node_id = std::atoi(value.c_str());
        } else if (arg == "--cluster-id") {
            if (!takeValue(argc, argv, i, "--cluster-id", value)) return 1;
            config.cluster_id = value;
        } else if (arg == "--partitions") {
            if (!takeValue(argc, argv, i, "--partitions", value)) return 1;
            config.default_partitions = std::atoi(value.c_str());
            if (config.default_partitions <= 0) {
                std::fprintf(stderr, "mnemos-kafka: --partitions must be positive\n");
                return 1;
            }
        } else if (arg == "--no-auto-create") {
            config.auto_create_topics = false;
        } else if (arg == "--redis-host") {
            if (!takeValue(argc, argv, i, "--redis-host", value)) return 1;
            redis_host = value;
        } else if (arg == "--redis-port") {
            if (!takeValue(argc, argv, i, "--redis-port", value)) return 1;
            redis_port = std::atoi(value.c_str());
            if (redis_port <= 0 || redis_port > 65535) {
                std::fprintf(stderr, "mnemos-kafka: invalid redis port '%s'\n", value.c_str());
                return 1;
            }
        } else if (arg == "--db") {
            if (!takeValue(argc, argv, i, "--db", value)) return 1;
            db = std::atoi(value.c_str());
            if (db < 0) {
                std::fprintf(stderr, "mnemos-kafka: --db must not be negative\n");
                return 1;
            }
        } else if (arg == "--timeout") {
            if (!takeValue(argc, argv, i, "--timeout", value)) return 1;
            timeout = std::atoi(value.c_str());
            if (timeout <= 0) {
                std::fprintf(stderr, "mnemos-kafka: --timeout must be positive\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "mnemos-kafka: unknown option '%s'\n", argv[i]);
            printUsage();
            return 1;
        }
    }

    // A client that hangs up while a response is being written would otherwise
    // take the broker down with it: SIGPIPE's default action is to terminate.
    // mnemos-server does the same, in src/main.cpp.
    std::signal(SIGPIPE, SIG_IGN);

    config.advertised_host = advertised.empty() ? bind_address : advertised;

    mnemos::kafka::PartitionLog log(redis_host, redis_port, db, timeout);

    // A keyspace that is not up yet is not fatal: every request reconnects, in
    // the same spirit as mnemos-mcp's warm-up.
    std::string error;
    if (!log.ensureConnected(error)) {
        std::fprintf(stderr, "mnemos-kafka: %s (will retry on each request)\n", error.c_str());
    }

    KafkaServer server(config, log, bind_address);
    if (!server.start()) return 1;

    std::fprintf(stderr,
                 "mnemos-kafka 0.1.0 listening on %s:%d, keyspace %s:%d db %d\n"
                 "  advertised as %s:%d, cluster %s, node %d\n",
                 bind_address.c_str(), config.advertised_port, redis_host.c_str(), redis_port, db,
                 config.advertised_host.c_str(), config.advertised_port, config.cluster_id.c_str(),
                 config.node_id);
    server.run();
    return 0;
}
