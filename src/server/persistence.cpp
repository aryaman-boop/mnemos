// The server's side of RDB: taking a snapshot, forking for one, and loading one
// back. The format itself lives in src/persist/rdb.cpp; nothing here knows a
// single byte of it.
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "persist/rdb.h"
#include "server/server.h"

namespace mnemos::server {

std::string Server::rdbPath() const {
    if (config_.dir.empty() || config_.dir == ".") return config_.dbfilename;
    return config_.dir + "/" + config_.dbfilename;
}

bool Server::writeRdbFile(std::string& error) {
    auto writer = persist::FileWriter::create(rdbPath());
    if (!writer) {
        error = std::strerror(errno);
        return false;
    }

    const std::int64_t now = net::EventLoop::currentTimeMs();
    for (std::size_t i = 0; i < databases_.size(); ++i) {
        Database& database = *databases_[i];
        if (database.size() == 0) continue;
        writer->selectDb(static_cast<int>(i), database.size(), database.expiresSize());
        for (const std::string& key : database.keys()) {
            // Deliberately not lookupRead: a key already past its TTL is simply
            // left out of the snapshot. Deleting it here would publish an
            // `expired` event, and in the BGSAVE child that means writing to
            // sockets the parent owns.
            const std::int64_t at = database.expireAtMs(key);
            if (at != -1 && at <= now) continue;
            Value* value = database.lookupRead(key, now);
            if (!value) continue;
            writer->writeEntry(key, *value, at);
        }
    }
    if (!writer->finish()) {
        error = "write failed";
        return false;
    }
    return true;
}

bool Server::saveRdb(std::string& error) {
    if (!writeRdbFile(error)) return false;
    dirty_          = 0;
    last_save_time_ = static_cast<std::int64_t>(std::time(nullptr));
    ++rdb_saves_;
    return true;
}

bool Server::startBackgroundSave(std::string& error) {
    if (bgsave_pid_ != -1) {
        error = "Background save already in progress";
        return false;
    }
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        error           = std::strerror(errno);
        last_bgsave_ok_ = false;
        return false;
    }
    if (pid == 0) {
        std::string child_error;
        const bool ok = writeRdbFile(child_error);
        // _exit, not exit: the child shares the parent's stdio buffers and its
        // listening socket, and must run neither destructors nor atexit hooks.
        ::_exit(ok ? 0 : 1);
    }
    bgsave_pid_    = pid;
    dirty_at_fork_ = dirty_;
    return true;
}

void Server::restoreLoadedKey(int db_index, std::string key, Value value,
                              std::int64_t expire_at_ms) {
    if (db_index < 0 || static_cast<std::size_t>(db_index) >= databases_.size()) {
        load_failed_ = true;
        return;
    }
    // A key whose deadline has already passed is dropped rather than loaded:
    // storing it would only mean deleting it again on the first lookup.
    if (expire_at_ms >= 0 && expire_at_ms <= net::EventLoop::currentTimeMs()) return;
    Database& database = *databases_[static_cast<std::size_t>(db_index)];
    database.setKey(key, std::move(value));
    if (expire_at_ms >= 0) database.setExpireAt(key, expire_at_ms);
}

bool Server::reloadRdb(std::string& error) {
    if (!saveRdb(error)) return false;

    // Emptying the keyspace is a change every watcher of a present key can see,
    // exactly as FLUSHDB is -- and Redis's DEBUG RELOAD signals it the same way.
    for (std::size_t i = 0; i < databases_.size(); ++i) {
        touchWatchedKeysOnFlush(static_cast<int>(i));
    }
    for (auto& database : databases_) database->flush();

    bool existed = false;
    loading_     = true;
    load_failed_ = false;
    const bool ok = persist::loadFile(
        rdbPath(),
        [this](int db, std::string key, Value value, std::int64_t at) {
            restoreLoadedKey(db, std::move(key), std::move(value), at);
        },
        existed, error);
    loading_ = false;
    if (!ok) return false;
    if (load_failed_) {
        error = "database index out of range";
        return false;
    }
    return true;
}

bool Server::loadRdbAtStartup() {
    bool        existed = false;
    std::string error;
    loading_     = true;
    load_failed_ = false;
    const bool ok = persist::loadFile(
        rdbPath(),
        [this](int db, std::string key, Value value, std::int64_t at) {
            restoreLoadedKey(db, std::move(key), std::move(value), at);
        },
        existed, error);
    loading_ = false;
    if (!ok || load_failed_) {
        std::fprintf(stderr, "mnemos: error loading %s: %s\n", rdbPath().c_str(),
                     ok ? "database index out of range" : error.c_str());
        return false;
    }
    if (existed) {
        std::size_t keys = 0;
        for (auto& database : databases_) keys += database->size();
        std::printf("mnemos: DB loaded from disk: %zu keys\n", keys);
    }
    last_save_time_ = static_cast<std::int64_t>(std::time(nullptr));
    return true;
}

void Server::reapBackgroundSave() {
    if (bgsave_pid_ == -1) return;
    int         status = 0;
    const pid_t done   = ::waitpid(bgsave_pid_, &status, WNOHANG);
    if (done == 0) return;  // still running
    if (done < 0) {
        bgsave_pid_     = -1;
        last_bgsave_ok_ = false;
        return;
    }
    bgsave_pid_     = -1;
    last_bgsave_ok_ = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!last_bgsave_ok_) return;
    // Only the writes the child could not have seen are still unsaved.
    dirty_          = dirty_ >= dirty_at_fork_ ? dirty_ - dirty_at_fork_ : 0;
    last_save_time_ = static_cast<std::int64_t>(std::time(nullptr));
    ++rdb_saves_;
}

}  // namespace mnemos::server
