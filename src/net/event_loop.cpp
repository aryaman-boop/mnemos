#include "net/event_loop.h"

#include <algorithm>
#include <cerrno>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#    define MNEMOS_USE_KQUEUE 1
#    include <sys/event.h>
#    include <sys/time.h>
#    include <sys/types.h>
#elif defined(__linux__)
#    define MNEMOS_USE_EPOLL 1
#    include <sys/epoll.h>
#else
#    error "mnemos requires kqueue (macOS/BSD) or epoll (Linux)"
#endif

namespace mnemos::net {
namespace {
// Upper bound on how long the loop will block when no timer is closer. Keeps
// stop() responsive even on an idle server.
constexpr int kMaxPollWaitMs = 100;
constexpr int kMaxEventsPerIteration = 1024;
}  // namespace

std::int64_t EventLoop::currentTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

EventLoop::EventLoop() {
#ifdef MNEMOS_USE_KQUEUE
    backend_fd_ = ::kqueue();
#else
    backend_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
#endif
    handlers_.resize(64);
    cached_now_ms_ = currentTimeMs();
}

EventLoop::~EventLoop() {
    if (backend_fd_ >= 0) ::close(backend_fd_);
}

void EventLoop::ensureCapacity(int fd) {
    if (static_cast<std::size_t>(fd) >= handlers_.size()) {
        handlers_.resize(static_cast<std::size_t>(fd) * 2 + 1);
    }
}

bool EventLoop::addFd(int fd, Ev interest, IoCallback cb) {
    if (fd < 0 || backend_fd_ < 0) return false;
    ensureCapacity(fd);

    Handler& h = handlers_[static_cast<std::size_t>(fd)];
    h.cb        = std::move(cb);
    h.interest  = Ev::None;
    h.active    = true;

    return modFd(fd, interest);
}

bool EventLoop::modFd(int fd, Ev interest) {
    if (fd < 0 || static_cast<std::size_t>(fd) >= handlers_.size()) return false;
    Handler& h = handlers_[static_cast<std::size_t>(fd)];
    if (!h.active) return false;

    const Ev previous = h.interest;
    if (previous == interest) return true;

#ifdef MNEMOS_USE_KQUEUE
    // kqueue tracks each filter independently, so we only submit the delta:
    // whichever of read/write just turned on gets EV_ADD, whichever turned off
    // gets EV_DELETE.
    struct kevent changes[2];
    int n = 0;

    const bool want_read  = any(interest & Ev::Read);
    const bool had_read   = any(previous & Ev::Read);
    const bool want_write = any(interest & Ev::Write);
    const bool had_write  = any(previous & Ev::Write);

    if (want_read != had_read) {
        EV_SET(&changes[n++], fd, EVFILT_READ, want_read ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    }
    if (want_write != had_write) {
        EV_SET(&changes[n++], fd, EVFILT_WRITE, want_write ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    }
    if (n > 0 && ::kevent(backend_fd_, changes, n, nullptr, 0, nullptr) == -1) {
        // ENOENT on delete just means the filter was already gone.
        if (errno != ENOENT) return false;
    }
#else
    struct epoll_event ev{};
    ev.data.fd = fd;
    if (any(interest & Ev::Read))  ev.events |= EPOLLIN;
    if (any(interest & Ev::Write)) ev.events |= EPOLLOUT;

    const int op = (previous == Ev::None) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (interest == Ev::None) {
        if (::epoll_ctl(backend_fd_, EPOLL_CTL_DEL, fd, nullptr) == -1 && errno != ENOENT) {
            return false;
        }
    } else if (::epoll_ctl(backend_fd_, op, fd, &ev) == -1) {
        return false;
    }
#endif

    h.interest = interest;
    return true;
}

void EventLoop::removeFd(int fd) {
    if (fd < 0 || static_cast<std::size_t>(fd) >= handlers_.size()) return;
    Handler& h = handlers_[static_cast<std::size_t>(fd)];
    if (!h.active) return;

    modFd(fd, Ev::None);
    h.cb       = nullptr;
    h.interest = Ev::None;
    h.active   = false;
}

EventLoop::TimerId EventLoop::addTimer(std::chrono::milliseconds period, TimerCallback cb) {
    const auto period_ms = static_cast<std::int64_t>(period.count());
    const TimerId id = next_timer_id_++;
    timers_.push_back(Timer{
        .id           = id,
        .next_fire_ms = currentTimeMs() + period_ms,
        .period_ms    = period_ms,
        .cb           = std::move(cb),
        .cancelled    = false,
    });
    return id;
}

void EventLoop::cancelTimer(TimerId id) {
    for (Timer& t : timers_) {
        if (t.id == id) {
            // Flagged rather than erased: cancelTimer() may be called from inside
            // a timer callback while processTimers() is iterating the vector.
            t.cancelled = true;
            return;
        }
    }
}

int EventLoop::processTimers() {
    const std::int64_t now = cached_now_ms_;
    std::int64_t next_deadline = now + kMaxPollWaitMs;

    // Index-based: a callback may append to timers_ and invalidate iterators.
    for (std::size_t i = 0; i < timers_.size(); ++i) {
        if (timers_[i].cancelled) continue;
        if (timers_[i].next_fire_ms <= now) {
            auto cb = timers_[i].cb;
            // Re-arm from `now` rather than the old deadline so a slow callback
            // cannot build up a backlog of immediately-due firings.
            timers_[i].next_fire_ms = now + timers_[i].period_ms;
            cb();
            if (i >= timers_.size()) break;
        }
        if (!timers_[i].cancelled) {
            next_deadline = std::min(next_deadline, timers_[i].next_fire_ms);
        }
    }

    std::erase_if(timers_, [](const Timer& t) { return t.cancelled; });

    const std::int64_t wait = next_deadline - now;
    if (wait <= 0) return 0;
    return static_cast<int>(std::min<std::int64_t>(wait, kMaxPollWaitMs));
}

void EventLoop::run() {
    running_ = true;

#ifdef MNEMOS_USE_KQUEUE
    std::vector<struct kevent> events(kMaxEventsPerIteration);
#else
    std::vector<struct epoll_event> events(kMaxEventsPerIteration);
#endif

    while (running_) {
        cached_now_ms_ = currentTimeMs();
        const int wait_ms = processTimers();
        if (!running_) break;

#ifdef MNEMOS_USE_KQUEUE
        struct timespec ts;
        ts.tv_sec  = wait_ms / 1000;
        ts.tv_nsec = static_cast<long>(wait_ms % 1000) * 1000000L;
        const int n = ::kevent(backend_fd_, nullptr, 0, events.data(),
                               static_cast<int>(events.size()), &ts);
#else
        const int n = ::epoll_wait(backend_fd_, events.data(),
                                   static_cast<int>(events.size()), wait_ms);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Refresh the cached clock: the poll above may have blocked for a while.
        cached_now_ms_ = currentTimeMs();

        for (int i = 0; i < n; ++i) {
#ifdef MNEMOS_USE_KQUEUE
            const int fd = static_cast<int>(events[i].ident);
            Ev fired = Ev::None;
            if (events[i].filter == EVFILT_READ)  fired = Ev::Read;
            if (events[i].filter == EVFILT_WRITE) fired = Ev::Write;
            // EV_EOF on a socket still deserves a read callback so the handler
            // can drain what is buffered and observe the clean close itself.
            if (events[i].flags & EV_ERROR) fired = fired | Ev::Read;
#else
            const int fd = events[i].data.fd;
            Ev fired = Ev::None;
            if (events[i].events & EPOLLIN)  fired = fired | Ev::Read;
            if (events[i].events & EPOLLOUT) fired = fired | Ev::Write;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) fired = fired | Ev::Read;
#endif
            if (fd < 0 || static_cast<std::size_t>(fd) >= handlers_.size()) continue;
            Handler& h = handlers_[static_cast<std::size_t>(fd)];
            // A previous callback in this same batch may have closed this fd.
            if (!h.active || !h.cb) continue;
            h.cb(fd, fired);
        }
    }
}

void EventLoop::stop() { running_ = false; }

}  // namespace mnemos::net
