// The server: listener, per-connection state, and the command dispatch loop.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/event_loop.h"
#include "net/resp.h"
#include "server/db.h"

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
    Stats                                           stats_;
    std::string                                     run_id_;
    int                                             executing_fd_ = -1;
};

}  // namespace mnemos::server
