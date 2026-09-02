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

#include "core/strings.h"
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

// The commands a RESP2 subscriber may still issue. Matched by canonical name,
// so the client's spelling of it does not matter.
bool isAllowedInSubscriberMode(std::string_view name) {
    return name == "subscribe" || name == "unsubscribe" ||
           name == "psubscribe" || name == "punsubscribe" ||
           name == "ssubscribe" || name == "sunsubscribe" ||
           name == "ping" || name == "quit" || name == "reset";
}

// Commands that steer a transaction rather than belonging to one, so they run
// immediately instead of being queued. QUIT is in the list for a blunter
// reason: a connection has to be able to end mid-transaction.
bool isTransactionControl(std::string_view name) {
    return name == "multi" || name == "exec" || name == "discard" ||
           name == "watch" || name == "quit" || name == "reset";
}

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
        // Two events no command handler is in a position to raise: a key that
        // died of its TTL, and a key that came into existence. Both are noticed
        // by the database itself, whichever command happened to touch it.
        databases_.back()->setExpiredKeyCallback([this, i](const std::string& key) {
            notifyKeyspaceEvent(notify::kExpired, "expired", key, i);
        });
        databases_.back()->setNewKeyCallback([this, i](const std::string& key) {
            // Loading an RDB fills the keyspace without any of it being news:
            // Redis raises no events for keys that come off disk.
            if (!loading_) notifyKeyspaceEvent(notify::kNew, "new", key, i);
        });
    }
    watched_keys_.resize(databases_.size());
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

    start_time_ms_  = net::EventLoop::currentTimeMs();
    last_save_time_ = start_time_ms_ / 1000;
    if (!loadRdbAtStartup()) return false;
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

    // Every rejection below shares a consequence: a client inside MULTI has now
    // sent a command that will never run, so its EXEC must refuse the whole
    // queue. Redis flags that here, at the point the bad command arrived,
    // rather than discovering it at EXEC.
    const auto reject = [&](std::string_view code, const std::string& text) {
        if (client.inMulti()) client.flagMultiError();
        // Rejecting EXEC *is* aborting the transaction, so the refusal takes
        // the shape of an abort rather than the shape of an error -- which is
        // why `EXEC x` reports EXECABORT and not a plain arity error.
        if (spec && spec->name == "exec") {
            discardTransaction(client);
            writer.error("EXECABORT Transaction discarded because of: " + text);
            return;
        }
        writer.error(std::string(code) + " " + text);
    };

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
        reject("ERR", "unknown command '" + argv[0] +
                          "', with args beginning with: " + args);
        return;
    }

    if (!arityOk(*spec, argv.size())) {
        // The canonical (lower-case) name is used here, not the client's
        // spelling -- `GeT` and `get` both report "'get'".
        reject("ERR", replies::wrongArgsText(spec->name));
        return;
    }

    if (!config_.requirepass.empty() && !client.authenticated() &&
        !(spec->flags & flags::kNoAuth)) {
        reject("NOAUTH", "Authentication required.");
        return;
    }

    // RESP2 has no way to tell a push apart from a reply, so a subscribed
    // client on that protocol is restricted to the handful of commands whose
    // replies cannot be confused for a message. RESP3 tags pushes and is free.
    if (client.protocolVersion() == 2 && client.inSubscriberMode() &&
        !isAllowedInSubscriberMode(spec->name)) {
        std::string named(spec->name);
        if ((spec->flags & flags::kContainer) && argv.size() > 1) {
            named += "|" + core::toLower(argv[1]);
        }
        reject("ERR", replies::subscriberModeText(named));
        return;
    }

    // Past every check a command has to pass, and inside a transaction: hold it
    // instead of running it. Nothing is validated beyond this point at EXEC
    // time, which is exactly why the checks above have to happen now.
    if (client.inMulti() && !isTransactionControl(spec->name)) {
        client.queueCommand(argv);
        writer.simpleString("QUEUED");
        return;
    }

    callCommand(client, *spec, argv, writer);

    // After the whole command, and so after every command of a transaction: a
    // message the client caused itself arrives behind the reply, never inside
    // the array EXEC is still building.
    if (!self_pending_.empty()) {
        client.outputBuffer().append(self_pending_);
        self_pending_.clear();
    }
}

void Server::callCommand(Client& client, const CommandSpec& spec,
                         const std::vector<std::string>& argv,
                         net::ReplyWriter& writer) {
    client.setLastCommand(std::string(spec.name));

    // Resolved per call rather than once: SELECT inside a transaction moves the
    // commands that follow it to another database, as it does outside one.
    CommandContext ctx{
        .server = *this,
        .client = client,
        .db     = db(client.dbIndex()),
        .argv   = argv,
        .reply  = writer,
    };

    const int previous_executing = executing_fd_;
    executing_fd_ = client.fd();
    spec.handler(ctx);
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

Client* Server::clientByFd(int fd) {
    auto it = clients_.find(fd);
    return it == clients_.end() ? nullptr : it->second.get();
}

void Server::queueWrite(int fd, std::string_view bytes) {
    if (fd == executing_fd_) {
        // A client can be a subscriber of what its own command publishes. Redis
        // sends it the reply first and the message after, so park the frame
        // until dispatch has finished writing the reply.
        self_pending_.append(bytes);
        return;
    }
    Client* client = clientByFd(fd);
    if (!client) return;
    client->outputBuffer().append(bytes);
    // The recipient is idle as far as the loop knows, so nothing would other-
    // wise come back to flush this. Asking for writability does.
    loop_.modFd(fd, net::Ev::Read | net::Ev::Write);
}

namespace {

// One message, encoded for one recipient. The frame is built against the
// *recipient's* protocol version: the same message is a plain array to a RESP2
// subscriber and a tagged push to a RESP3 one.
void deliverMessage(Server& server, int fd, std::string_view kind,
                    const std::string* pattern, const std::string& channel,
                    const std::string& payload) {
    Client* recipient = server.clientByFd(fd);
    if (!recipient) return;

    std::string      frame;
    net::ReplyWriter writer(frame, recipient->protocolVersion());
    writer.pushHeader(pattern ? 4 : 3);
    writer.bulk(kind);
    if (pattern) writer.bulk(*pattern);
    writer.bulk(channel);
    writer.bulk(payload);
    server.queueWrite(fd, frame);
}

// The subscriber list is copied before delivery: writing to a peer can close
// it, and closing unsubscribes it, which would invalidate an iterator over the
// live set.
std::vector<int> subscribersOf(PubSub& pubsub, ChannelKind kind, const std::string& channel) {
    const std::set<int>* subscribers = pubsub.channelSubscribers(kind, channel);
    if (!subscribers) return {};
    return std::vector<int>(subscribers->begin(), subscribers->end());
}

}  // namespace

std::int64_t Server::publishMessage(const std::string& channel, const std::string& payload) {
    std::int64_t receivers = 0;
    for (int fd : subscribersOf(pubsub_, ChannelKind::Global, channel)) {
        deliverMessage(*this, fd, "message", nullptr, channel, payload);
        ++receivers;
    }

    // No index for patterns: every one of them is tested against the channel.
    std::vector<std::pair<std::string, std::vector<int>>> pattern_targets;
    for (const auto& [pattern, subscribers] : pubsub_.patterns()) {
        if (!core::globMatch(pattern, channel)) continue;
        pattern_targets.emplace_back(pattern,
                                     std::vector<int>(subscribers.begin(), subscribers.end()));
    }
    for (const auto& [pattern, fds] : pattern_targets) {
        for (int fd : fds) {
            deliverMessage(*this, fd, "pmessage", &pattern, channel, payload);
            ++receivers;
        }
    }
    return receivers;
}

std::int64_t Server::publishShardMessage(const std::string& channel,
                                         const std::string& payload) {
    std::int64_t receivers = 0;
    for (int fd : subscribersOf(pubsub_, ChannelKind::Shard, channel)) {
        deliverMessage(*this, fd, "smessage", nullptr, channel, payload);
        ++receivers;
    }
    return receivers;
}

void Server::notifyKeyspaceEvent(std::uint32_t event_class, std::string_view event,
                                 const std::string& key, int db_index) {
    // Every mutation in the server already passes through here, which makes
    // this the one place a WATCH needs to be invalidated from: the set of
    // changes worth announcing and the set a transaction can lose a race to are
    // the same set. It is signalled before the enabled check below -- whether
    // anyone asked to be *told* about the change has no bearing on whether the
    // change happened.
    if (event_class != notify::kKeyMiss) {
        signalModifiedKey(db_index, key, event == "expired");
    }

    const std::uint32_t enabled = config_.notify_flags;
    if (!(enabled & event_class)) return;

    const std::string db = std::to_string(db_index);
    // Keyspace first, then keyevent: a subscriber to both sees them in this
    // order, and the differential suite pins it.
    if (enabled & notify::kKeyspace) {
        publishMessage("__keyspace@" + db + "__:" + key, std::string(event));
    }
    if (enabled & notify::kKeyevent) {
        publishMessage("__keyevent@" + db + "__:" + std::string(event), key);
    }
}

void Server::watchKey(Client& client, const std::string& key) {
    const int db_index = client.dbIndex();
    for (const Client::WatchedKey& watched : client.watchedKeys()) {
        // WATCH is idempotent: watching the same key twice is one watch, and
        // one UNWATCH has to be enough to undo it.
        if (watched.db == db_index && watched.key == key) return;
    }
    const std::int64_t expires_at = db(db_index).expireAtMs(key);
    client.watchedKeys().push_back(
        {db_index, key, expires_at >= 0 && expires_at <= nowMs()});
    watched_keys_[static_cast<std::size_t>(db_index)][key].insert(client.fd());
}

void Server::unwatchAllKeys(Client& client) {
    for (const Client::WatchedKey& watched : client.watchedKeys()) {
        auto& keys = watched_keys_[static_cast<std::size_t>(watched.db)];
        const auto it = keys.find(watched.key);
        if (it == keys.end()) continue;
        it->second.erase(client.fd());
        // The map is expected to be empty on almost every server, so an entry
        // nobody watches any more is removed rather than left behind.
        if (it->second.empty()) keys.erase(it);
    }
    client.watchedKeys().clear();
}

void Server::discardTransaction(Client& client) {
    client.resetMultiState();
    unwatchAllKeys(client);
}

void Server::signalModifiedKey(int db_index, const std::string& key, bool from_expiry) {
    auto& keys = watched_keys_[static_cast<std::size_t>(db_index)];
    const auto it = keys.find(key);
    if (it == keys.end()) return;

    for (const int fd : it->second) {
        Client* watcher = clientByFd(fd);
        if (!watcher) continue;
        if (from_expiry) {
            // The key died of a TTL it already had when this client watched it.
            // Nothing the client could observe has changed -- it read the key as
            // gone then and reads it as gone now -- so the watch survives, but
            // only once: a second deletion would be a real change.
            bool seen_expired = false;
            for (Client::WatchedKey& watched : watcher->watchedKeys()) {
                if (watched.db == db_index && watched.key == key && watched.expired) {
                    watched.expired = false;
                    seen_expired    = true;
                }
            }
            if (seen_expired) continue;
        }
        watcher->setDirtyCas();
    }
}

void Server::touchWatchedKeysOnFlush(int db_index) {
    Database& database = db(db_index);
    // Collected first: signalling walks the same map, and a key that is not
    // there is not a change -- watching a key in a database that never held it
    // survives that database being emptied.
    std::vector<std::string> present;
    for (const auto& [key, watchers] : watched_keys_[static_cast<std::size_t>(db_index)]) {
        (void)watchers;
        if (database.raw().find(key) != nullptr) present.push_back(key);
    }
    for (const std::string& key : present) signalModifiedKey(db_index, key, false);
}

void Server::clearSubscriptions(Client& client) {
    for (const std::string& channel : client.channels()) {
        pubsub_.unsubscribeChannel(ChannelKind::Global, channel, client.fd());
    }
    for (const std::string& pattern : client.patterns()) {
        pubsub_.unsubscribePattern(pattern, client.fd());
    }
    for (const std::string& channel : client.shardChannels()) {
        pubsub_.unsubscribeChannel(ChannelKind::Shard, channel, client.fd());
    }
    client.channels().clear();
    client.patterns().clear();
    client.shardChannels().clear();
}

void Server::closeClient(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;

    // Done before the deferred-close branch below: a connection on its way out
    // must stop receiving messages immediately, not once the loop unwinds.
    clearSubscriptions(*it->second);
    unwatchAllKeys(*it->second);

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

    reapBackgroundSave();

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
