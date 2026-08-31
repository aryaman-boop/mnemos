// Portable readiness-based event loop: kqueue on macOS/BSD, epoll on Linux.
//
// Mirrors the concurrency model of Redis itself: one thread owns the entire
// keyspace, so command handlers never need a lock. Everything that must happen
// off the hot path (expiry cycles, RDB flushing, replication pings) is driven
// from this same loop via timers.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace mnemos::net {

// Bitmask of readiness a caller is interested in / that fired.
enum class Ev : std::uint8_t {
    None  = 0,
    Read  = 1 << 0,
    Write = 1 << 1,
};

constexpr Ev operator|(Ev a, Ev b) {
    return static_cast<Ev>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr Ev operator&(Ev a, Ev b) {
    return static_cast<Ev>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
constexpr bool any(Ev e) { return static_cast<std::uint8_t>(e) != 0; }

class EventLoop {
public:
    using IoCallback    = std::function<void(int fd, Ev fired)>;
    using TimerCallback = std::function<void()>;
    using TimerId       = std::uint64_t;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Register `fd`. The callback is owned by the loop until removeFd().
    bool addFd(int fd, Ev interest, IoCallback cb);
    // Change which events `fd` is watched for, leaving its callback in place.
    bool modFd(int fd, Ev interest);
    void removeFd(int fd);

    // Repeating timer. Returns an id usable with cancelTimer().
    TimerId addTimer(std::chrono::milliseconds period, TimerCallback cb);
    void cancelTimer(TimerId id);

    void run();
    void stop();

    // Wall-clock milliseconds, cached once per loop iteration. Command handlers
    // read this instead of calling the clock directly -- exactly as Redis caches
    // server.mstime -- so every key touched in one iteration agrees on "now".
    std::int64_t nowMs() const { return cached_now_ms_; }

    static std::int64_t currentTimeMs();

private:
    struct Timer {
        TimerId       id;
        std::int64_t  next_fire_ms;
        std::int64_t  period_ms;
        TimerCallback cb;
        bool          cancelled = false;
    };

    struct Handler {
        IoCallback cb;
        Ev         interest = Ev::None;
        bool       active   = false;
    };

    // Fires every timer whose deadline has passed; returns ms until the next one
    // (or a default poll interval when nothing is pending).
    int processTimers();
    void ensureCapacity(int fd);

    int                  backend_fd_ = -1;  // kqueue() or epoll_create1() handle
    std::vector<Handler> handlers_;         // indexed by fd -- dense and cache-friendly
    std::vector<Timer>   timers_;
    TimerId              next_timer_id_ = 1;
    std::int64_t         cached_now_ms_ = 0;
    bool                 running_       = false;
};

}  // namespace mnemos::net
