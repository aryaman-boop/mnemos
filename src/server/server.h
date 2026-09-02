// The server: listener, per-connection state, and the command dispatch loop.
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "net/event_loop.h"
#include "net/resp.h"
#include "server/command_table.h"
#include "server/db.h"
#include "server/notify.h"
#include "server/pubsub.h"

namespace mnemos::server {

struct Config {
    std::string  bind_address = "127.0.0.1";
    int          port         = 6380;  // not 6379, so we can run beside real Redis
    int          databases    = 16;
    std::string  logfile;               // empty => stdout
    bool         daemonize    = false;
    std::size_t  max_clients  = 10000;
    std::string  requirepass;           // empty => no auth
    std::string  dir          = ".";
    std::string  dbfilename   = "dump.rdb";
    // notify-keyspace-events, as a mask of notify:: class bits. Zero -- no
    // notifications at all -- is Redis's default, and deliberately so: the
    // events cost a publish on every write.
    std::uint32_t notify_flags = 0;
};

// Per-connection state. Mirrors Redis's client struct: an input buffer with a
// parse cursor, an output buffer with a flush cursor, and the negotiated
// protocol version, which every reply is encoded against.
class Client {
public:
    Client(int fd, std::uint64_t id, std::string addr)
        : fd_(fd), id_(id), addr_(std::move(addr)) {}

    int           fd() const { return fd_; }
    std::uint64_t id() const { return id_; }
    const std::string& addr() const { return addr_; }

    int  protocolVersion() const { return protover_; }
    void setProtocolVersion(int v) { protover_ = v; }

    int  dbIndex() const { return db_index_; }
    void setDbIndex(int i) { db_index_ = i; }

    const std::string& name() const { return name_; }
    void setName(std::string n) { name_ = std::move(n); }

    bool authenticated() const { return authenticated_; }
    void setAuthenticated(bool a) { authenticated_ = a; }

    bool shouldClose() const { return close_after_reply_; }
    void closeAfterReply() { close_after_reply_ = true; }

    std::string&       queryBuffer() { return query_buffer_; }
    std::size_t&       queryPos() { return query_pos_; }
    net::RequestParser& parser() { return parser_; }

    std::string& outputBuffer() { return output_buffer_; }
    std::size_t& outputSent() { return output_sent_; }
    bool hasPendingOutput() const { return output_sent_ < output_buffer_.size(); }

    std::int64_t createdAtMs() const { return created_at_ms_; }
    void setCreatedAtMs(std::int64_t t) { created_at_ms_ = t; }
    std::int64_t lastInteractionMs() const { return last_interaction_ms_; }
    void setLastInteractionMs(std::int64_t t) { last_interaction_ms_ = t; }

    const std::string& lastCommand() const { return last_command_; }
    void setLastCommand(std::string c) { last_command_ = std::move(c); }

    // The client's own view of its subscriptions. The server keeps the inverse
    // index; this side is what UNSUBSCRIBE with no arguments enumerates, and
    // what disconnect cleanup walks.
    std::set<std::string>&       channels() { return channels_; }
    const std::set<std::string>& channels() const { return channels_; }
    std::set<std::string>&       patterns() { return patterns_; }
    const std::set<std::string>& patterns() const { return patterns_; }
    std::set<std::string>&       shardChannels() { return shard_channels_; }
    const std::set<std::string>& shardChannels() const { return shard_channels_; }

    // Channels and patterns together, which is the number an ordinary subscribe
    // or unsubscribe confirmation reports back. Shard subscriptions are counted
    // apart from it: Redis holds them in their own dictionary, so SSUBSCRIBE
    // reports how many shard channels the client has and nothing else.
    std::size_t subscriptionCount() const { return channels_.size() + patterns_.size(); }
    // Every namespace together, which is what decides subscriber mode.
    bool inSubscriberMode() const {
        return subscriptionCount() + shard_channels_.size() > 0;
    }

    // MULTI state. The queue belongs to the connection, not to the server:
    // two clients can be mid-transaction at the same time with nothing shared
    // between them, because nothing is executed until one of them says EXEC.
    bool inMulti() const { return in_multi_; }
    void beginMulti() { in_multi_ = true; }
    void queueCommand(std::vector<std::string> argv) {
        multi_queue_.push_back(std::move(argv));
    }
    std::vector<std::vector<std::string>>& multiQueue() { return multi_queue_; }
    // A command that could not even be queued -- unknown, or called with the
    // wrong number of arguments. EXEC then refuses to run any of the queue.
    void flagMultiError() { multi_error_ = true; }
    bool multiError() const { return multi_error_; }
    // Everything MULTI set up, undone. The watches go too, but they live in the
    // server's index as well, so Server::discardTransaction is what to call.
    void resetMultiState() {
        in_multi_    = false;
        multi_error_ = false;
        dirty_cas_   = false;
        multi_queue_.clear();
    }

    // One WATCHed key. The database is recorded because a watch outlives the
    // SELECT that was in force when it was made: watching `k` in db 1 is not
    // invalidated by someone writing `k` in db 0.
    struct WatchedKey {
        int         db;
        std::string key;
        // Whether the key was already past its TTL when WATCH ran. The lazy
        // delete that happens the next time anyone looks at it is then not a
        // change this client can observe -- it already saw the key as gone.
        bool        expired;
    };
    std::vector<WatchedKey>& watchedKeys() { return watched_keys_; }
    bool dirtyCas() const { return dirty_cas_; }
    void setDirtyCas() { dirty_cas_ = true; }
    // UNWATCH clears the flag as well as the watches: a client that gives up on
    // its old assumptions is not still failing because of them.
    void clearDirtyCas() { dirty_cas_ = false; }

    // Discards the already-parsed prefix of the query buffer. Called once per
    // read cycle rather than per command, so a pipeline of N commands costs one
    // memmove instead of N.
    void trimQueryBuffer();

private:
    int                 fd_;
    std::uint64_t       id_;
    std::string         addr_;
    int                 protover_   = 2;
    int                 db_index_   = 0;
    std::string         name_;
    bool                authenticated_     = false;
    bool                close_after_reply_ = false;

    std::string         query_buffer_;
    std::size_t         query_pos_ = 0;
    net::RequestParser  parser_;

    std::string         output_buffer_;
    std::size_t         output_sent_ = 0;

    bool                                  in_multi_    = false;
    bool                                  multi_error_ = false;
    bool                                  dirty_cas_   = false;
    std::vector<std::vector<std::string>> multi_queue_;
    std::vector<WatchedKey>               watched_keys_;

    std::set<std::string> channels_;
    std::set<std::string> patterns_;
    std::set<std::string> shard_channels_;

    std::int64_t        created_at_ms_        = 0;
    std::int64_t        last_interaction_ms_  = 0;
    std::string         last_command_;
};

struct Stats {
    std::uint64_t connections_received = 0;
    std::uint64_t commands_processed   = 0;
    std::uint64_t expired_keys         = 0;
    std::uint64_t keyspace_hits        = 0;
    std::uint64_t keyspace_misses      = 0;
    std::uint64_t rejected_connections = 0;
    std::uint64_t total_net_input      = 0;
    std::uint64_t total_net_output     = 0;
};

class Server {
public:
    explicit Server(Config config);
    ~Server();

    // Binds and listens. Returns false with a message on stderr on failure.
    bool start();
    void run();
    void stop();

    Config&       config() { return config_; }
    const Config& config() const { return config_; }
    net::EventLoop& loop() { return loop_; }
    Stats&        stats() { return stats_; }

    Database& db(int index) { return *databases_.at(static_cast<std::size_t>(index)); }
    std::size_t databaseCount() const { return databases_.size(); }

    std::int64_t nowMs() const { return loop_.nowMs(); }
    std::int64_t startTimeMs() const { return start_time_ms_; }
    std::uint64_t dirty() const { return dirty_; }
    void markDirty(std::uint64_t n = 1) { dirty_ += n; }

    const std::unordered_map<int, std::unique_ptr<Client>>& clients() const { return clients_; }

    // Null when the fd has already been closed -- which is exactly what a
    // publish walking a stale subscriber list needs to be able to discover.
    Client* clientByFd(int fd);

    PubSub& pubsub() { return pubsub_; }

    // Sends `payload` to everyone subscribed to `channel`, and to every pattern
    // that matches it. Returns how many deliveries that was, which is what
    // PUBLISH reports. Keyspace notifications go out through this same path --
    // they are ordinary messages, distinguished only by their channel names.
    std::int64_t publishMessage(const std::string& channel, const std::string& payload);
    // The shard namespace, which patterns deliberately do not reach.
    std::int64_t publishShardMessage(const std::string& channel, const std::string& payload);

    // Publishes the __keyspace@<db>__ and __keyevent@<db>__ pair for one event,
    // when `event_class` and the K/E flags say to.
    void notifyKeyspaceEvent(std::uint32_t event_class, std::string_view event,
                             const std::string& key, int db_index);

    // Appends an out-of-band reply to another connection and asks the loop to
    // drain it. Command handlers write to their own client through ReplyWriter;
    // this is the only way bytes reach a client that did not ask for them.
    void queueWrite(int fd, std::string_view bytes);

    // Drops every subscription held by a connection. Called on disconnect and
    // by RESET, both of which must leave no entry pointing at the fd.
    void clearSubscriptions(Client& client);

    // WATCH, from the server's side: the inverse index, key -> the connections
    // watching it, one map per database. It lives here rather than in Database
    // because a watch is a fact about a connection, and a Database knows
    // nothing about connections.
    void watchKey(Client& client, const std::string& key);
    void unwatchAllKeys(Client& client);
    // Ends a transaction: the queue, the flags and the watches all go.
    void discardTransaction(Client& client);
    // Marks every connection watching `key` so that its EXEC will refuse to
    // run. `from_expiry` says the change was a key dying of its own TTL, which
    // is not news to a watcher that had already seen it as expired.
    void signalModifiedKey(int db_index, const std::string& key, bool from_expiry);
    // FLUSHDB and FLUSHALL raise no keyspace event, so they signal by hand --
    // and only for keys that were actually there. Emptying a database that a
    // watcher's key was never in changes nothing for that watcher.
    void touchWatchedKeysOnFlush(int db_index);

    // Runs one command's handler and nothing else: no arity check, no queueing,
    // no flush of pending pushes. Public because EXEC drives it -- a
    // transaction is this call in a loop over a queue instead of over a socket.
    void callCommand(Client& client, const CommandSpec& spec,
                     const std::vector<std::string>& argv, net::ReplyWriter& writer);

    const std::string& runId() const { return run_id_; }

    // Closes a connection and frees its state. Safe to call from a command
    // handler acting on *another* client; deferred when it targets the client
    // whose command is currently executing.
    void closeClient(int fd);

private:
    void onAcceptable();
    void onClientEvent(int fd, net::Ev fired);
    void readFromClient(Client& client);
    void flushOutput(Client& client);
    void processInputBuffer(Client& client);
    void dispatch(Client& client, std::vector<std::string>& argv);
    void serverCron();

    Config                                          config_;
    net::EventLoop                                  loop_;
    int                                             listen_fd_ = -1;
    std::vector<std::unique_ptr<Database>>          databases_;
    std::unordered_map<int, std::unique_ptr<Client>> clients_;
    std::vector<int>                                pending_close_;
    std::uint64_t                                   next_client_id_ = 1;
    std::int64_t                                    start_time_ms_  = 0;
    std::uint64_t                                   dirty_          = 0;
    PubSub                                          pubsub_;
    // Indexed by database, so a lookup on the write path is one hash of the key
    // in a map that is empty on almost every server.
    std::vector<std::unordered_map<std::string, std::set<int>>> watched_keys_;
    // Frames a command owes its own connection. Redis answers a command before
    // delivering any message it caused to the client that ran it, so these are
    // held back until the handler's reply has been written.
    std::string                                     self_pending_;
    Stats                                           stats_;
    std::string                                     run_id_;
    int                                             executing_fd_ = -1;
};

}  // namespace mnemos::server
