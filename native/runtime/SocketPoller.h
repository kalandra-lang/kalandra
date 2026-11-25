// Event-driven readiness poller for nonblocking sockets and bounded I/O timers.
// Include after TcpSocket.h so Winsock2 precedes any transitive windows.h import.
#pragma once
#include "TcpSocket.h"
#include "Tasks.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif

namespace kalandra::runtime::tcp {

inline void waitReady(Native fd, bool write, std::chrono::nanoseconds maxWait) noexcept {
  if (fd == invalid || maxWait <= std::chrono::nanoseconds::zero())
    return;
  const auto capped = std::min<std::chrono::nanoseconds>(maxWait, std::chrono::milliseconds(1));
  const int ms = std::max(1, static_cast<int>((capped.count() + 999'999) / 1'000'000));
#ifdef _WIN32
  WSAPOLLFD pfd{fd, static_cast<SHORT>(write ? POLLWRNORM : POLLRDNORM), 0};
  (void)WSAPoll(&pfd, 1, ms);
#else
  pollfd pfd{fd, static_cast<short>(write ? POLLOUT : POLLIN), 0};
  (void)::poll(&pfd, 1, ms);
#endif
}

struct PollerTicket {
  std::uint32_t slot = UINT32_MAX;
  std::uint64_t generation = 0;
  explicit operator bool() const noexcept { return slot != UINT32_MAX; }
};

class SocketPoller final {
  static constexpr std::size_t capacity = 1024;
  using Clock = std::chrono::steady_clock;

  struct Slot {
    std::shared_ptr<Socket> socket;
    ExternalCompletion completion;
    Clock::time_point deadline{};
    std::uint64_t generation = 0;
    std::uint16_t activePos = 0;
    bool write = false;
    bool active = false;
  };

  struct ReadyItem {
    std::shared_ptr<Socket> socket;
    ExternalCompletion completion;
  };

  struct WatchCount {
    unsigned readers = 0;
    unsigned writers = 0;
  };

  std::mutex mutex_;
  std::condition_variable cv_;
  std::array<Slot, capacity> slots_{};
  std::array<std::uint16_t, capacity> freeSlots_{};
  std::array<std::uint16_t, capacity> active_{};
  std::size_t freeCount_ = capacity;
  std::size_t activeCount_ = 0;
  Clock::time_point nextWakeup_ = Clock::time_point::max();
  bool waiting_ = false;
#if defined(__APPLE__)
  static constexpr uintptr_t wakeIdent = 1;
  int kq_ = -1;
#elif defined(__linux__)
  int ep_ = -1;
  int wakeFd_ = -1;
#endif

  WatchCount countWatchingLocked(Native fd) const noexcept {
    WatchCount c{};
    for (std::size_t i = 0; i < activeCount_; ++i) {
      const auto &s = slots_[active_[i]];
      if (s.socket && s.socket->fd == fd) {
        if (s.write)
          ++c.writers;
        else
          ++c.readers;
      }
    }
    return c;
  }

  bool registerKernelLocked(Native fd, bool write) noexcept {
#if defined(__APPLE__)
    if (kq_ < 0)
      return false;
    const auto before = countWatchingLocked(fd);
    if ((write ? before.writers : before.readers) != 0)
      return true;
    struct kevent ev{};
    EV_SET(&ev, static_cast<uintptr_t>(fd), write ? EVFILT_WRITE : EVFILT_READ, EV_ADD, 0, 0,
           nullptr);
    return ::kevent(kq_, &ev, 1, nullptr, 0, nullptr) == 0;
#elif defined(__linux__)
    if (ep_ < 0)
      return false;
    const auto before = countWatchingLocked(fd);
    const bool wasWatched = (before.readers + before.writers) > 0;
    if (wasWatched && (write ? before.writers : before.readers) != 0)
      return true;
    epoll_event ev{};
    ev.events = EPOLLRDHUP | ((before.readers || !write) ? EPOLLIN : 0u) |
                ((before.writers || write) ? EPOLLOUT : 0u);
    ev.data.fd = fd;
    return ::epoll_ctl(ep_, wasWatched ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev) == 0;
#else
    (void)fd;
    (void)write;
    return true;
#endif
  }

  void unregisterKernelLocked(Native fd, bool write) noexcept {
#if defined(__APPLE__)
    if (kq_ < 0)
      return;
    const auto after = countWatchingLocked(fd);
    if ((write ? after.writers : after.readers) != 0)
      return;
    struct kevent ev{};
    EV_SET(&ev, static_cast<uintptr_t>(fd), write ? EVFILT_WRITE : EVFILT_READ, EV_DELETE, 0, 0,
           nullptr);
    (void)::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
#elif defined(__linux__)
    if (ep_ < 0)
      return;
    const auto after = countWatchingLocked(fd);
    if (after.readers == 0 && after.writers == 0) {
      (void)::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
    } else if ((write ? after.writers : after.readers) == 0) {
      epoll_event ev{};
      ev.events = EPOLLRDHUP | (after.readers ? EPOLLIN : 0u) | (after.writers ? EPOLLOUT : 0u);
      ev.data.fd = fd;
      (void)::epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
    }
#else
    (void)fd;
    (void)write;
#endif
  }

  void triggerWakeup() noexcept {
#if defined(__APPLE__)
    if (kq_ >= 0) {
      struct kevent ev{};
      EV_SET(&ev, wakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
      (void)::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }
#elif defined(__linux__)
    if (wakeFd_ >= 0) {
      const std::uint64_t one = 1;
      (void)::write(wakeFd_, &one, sizeof(one));
    }
#else
    cv_.notify_one();
#endif
  }

  void removeAtLocked(std::size_t pos, Native fd, bool write) noexcept {
    const auto idx = active_[pos];
    const auto lastIdx = active_[--activeCount_];
    if (pos != activeCount_) {
      active_[pos] = lastIdx;
      slots_[lastIdx].activePos = static_cast<std::uint16_t>(pos);
    }
    slots_[idx].active = false;
    slots_[idx].socket.reset();
    slots_[idx].completion = {};
    freeSlots_[freeCount_++] = idx;
    if (fd != invalid)
      unregisterKernelLocked(fd, write);
  }

  void collectReadyFdLocked(Native fd, bool readable, bool writable,
                            std::vector<ReadyItem> &ready) noexcept {
    for (std::size_t i = 0; i < activeCount_;) {
      const auto idx = active_[i];
      auto &s = slots_[idx];
      if (s.socket && s.socket->fd == fd && (s.write ? writable : readable)) {
        ready.push_back({std::move(s.socket), std::exchange(s.completion, {})});
        removeAtLocked(i, fd, s.write);
      } else {
        ++i;
      }
    }
  }

  void collectExpiredLocked(Clock::time_point now, std::vector<ReadyItem> &ready) noexcept {
    for (std::size_t i = 0; i < activeCount_;) {
      const auto idx = active_[i];
      auto &s = slots_[idx];
      if (s.deadline <= now) {
        const Native fd = s.socket ? s.socket->fd : invalid;
        const bool write = s.write;
        ready.push_back({std::move(s.socket), std::exchange(s.completion, {})});
        removeAtLocked(i, fd, write);
      } else {
        ++i;
      }
    }
  }

  std::optional<Clock::time_point> earliestDeadlineLocked() const noexcept {
    if (!activeCount_)
      return std::nullopt;
    auto earliest = slots_[active_[0]].deadline;
    for (std::size_t i = 1; i < activeCount_; ++i)
      earliest = std::min(earliest, slots_[active_[i]].deadline);
    return earliest;
  }

  static void flushReady(std::vector<ReadyItem> &ready) noexcept {
    for (auto &item : ready)
      if (item.completion)
        (void)item.completion.signal();
    ready.clear();
  }

  void worker() noexcept {
#ifndef _WIN32
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif
    std::vector<ReadyItem> ready;
    ready.reserve(capacity);

#if defined(__APPLE__)
    std::array<struct kevent, 64> events{};
    for (;;) {
      std::optional<Clock::time_point> deadline;
      {
        std::lock_guard lock(mutex_);
        collectExpiredLocked(Clock::now(), ready);
        if (ready.empty()) {
          deadline = earliestDeadlineLocked();
          waiting_ = true;
          nextWakeup_ = deadline.value_or(Clock::time_point::max());
        }
      }
      if (!ready.empty()) {
        flushReady(ready);
        continue;
      }
      timespec ts{};
      const timespec *timeoutPtr = nullptr;
      if (deadline) {
        const auto now = Clock::now();
        if (*deadline > now) {
          const auto ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now).count();
          ts.tv_sec = static_cast<time_t>(ns / 1'000'000'000);
          ts.tv_nsec = static_cast<long>(ns % 1'000'000'000);
        }
        timeoutPtr = &ts;
      }
      const int n = ::kevent(kq_, nullptr, 0, events.data(), static_cast<int>(events.size()),
                             timeoutPtr);
      {
        std::lock_guard lock(mutex_);
        waiting_ = false;
        for (int i = 0; i < n; ++i) {
          if (events[static_cast<std::size_t>(i)].filter == EVFILT_USER)
            continue;
          const auto &ev = events[static_cast<std::size_t>(i)];
          const auto fd = static_cast<Native>(ev.ident);
          const bool errored = (ev.flags & EV_ERROR) != 0;
          const bool readable = errored || ev.filter == EVFILT_READ;
          const bool writable = errored || ev.filter == EVFILT_WRITE;
          collectReadyFdLocked(fd, readable, writable, ready);
        }
        collectExpiredLocked(Clock::now(), ready);
      }
      flushReady(ready);
    }
#elif defined(__linux__)
    std::array<epoll_event, 64> events{};
    for (;;) {
      std::optional<Clock::time_point> deadline;
      {
        std::lock_guard lock(mutex_);
        collectExpiredLocked(Clock::now(), ready);
        if (ready.empty()) {
          deadline = earliestDeadlineLocked();
          waiting_ = true;
          nextWakeup_ = deadline.value_or(Clock::time_point::max());
        }
      }
      if (!ready.empty()) {
        flushReady(ready);
        continue;
      }
      int timeoutMs = -1;
      if (deadline) {
        const auto now = Clock::now();
        if (*deadline <= now) {
          timeoutMs = 0;
        } else {
          const auto ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now).count();
          const auto roundedMs = (ns + 999'999) / 1'000'000;
          timeoutMs = static_cast<int>(std::min<long long>(roundedMs, 60'000));
        }
      }
      const int n = ::epoll_wait(ep_, events.data(), static_cast<int>(events.size()), timeoutMs);
      {
        std::lock_guard lock(mutex_);
        waiting_ = false;
        for (int i = 0; i < n; ++i) {
          const auto &ev = events[static_cast<std::size_t>(i)];
          if (ev.data.fd == wakeFd_) {
            std::uint64_t drained = 0;
            while (::read(wakeFd_, &drained, sizeof(drained)) > 0) {
            }
            continue;
          }
          const Native fd = ev.data.fd;
          const auto mask = ev.events;
          const bool readable = (mask & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) != 0;
          const bool writable = (mask & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0;
          collectReadyFdLocked(fd, readable, writable, ready);
        }
        collectExpiredLocked(Clock::now(), ready);
      }
      flushReady(ready);
    }
#else
    struct PollEntry {
      std::shared_ptr<Socket> socket;
      std::uint16_t slot = 0;
      std::uint64_t generation = 0;
    };
#ifdef _WIN32
    using NativePollFd = WSAPOLLFD;
#else
    using NativePollFd = pollfd;
#endif
    std::vector<PollEntry> entries;
    std::vector<NativePollFd> pfds;
    entries.reserve(capacity);
    pfds.reserve(capacity);
    for (;;) {
      entries.clear();
      pfds.clear();
      int timeoutMs = 1;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return activeCount_ != 0; });
        const auto now = Clock::now();
        collectExpiredLocked(now, ready);
        if (!ready.empty()) {
          lock.unlock();
          flushReady(ready);
          continue;
        }
        const auto deadline = earliestDeadlineLocked();
        if (!deadline)
          continue;
        for (std::size_t i = 0; i < activeCount_; ++i) {
          const auto idx = active_[i];
          const auto &s = slots_[idx];
          if (!s.socket)
            continue;
          entries.push_back({s.socket, idx, s.generation});
#ifdef _WIN32
          pfds.push_back({s.socket->fd, static_cast<SHORT>(s.write ? POLLWRNORM : POLLRDNORM), 0});
#else
          pfds.push_back({s.socket->fd, static_cast<short>(s.write ? POLLOUT : POLLIN), 0});
#endif
        }
        const auto remaining = *deadline > now
                                   ? std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now)
                                   : std::chrono::nanoseconds::zero();
        if (pfds.empty()) {
          waiting_ = true;
          nextWakeup_ = *deadline;
          cv_.wait_for(lock, remaining);
          waiting_ = false;
          collectExpiredLocked(Clock::now(), ready);
          lock.unlock();
          flushReady(ready);
          continue;
        }
        const auto capped = std::min<std::chrono::nanoseconds>(remaining, std::chrono::milliseconds(1));
        timeoutMs = capped <= std::chrono::nanoseconds::zero()
                        ? 0
                        : std::max(1, static_cast<int>((capped.count() + 999'999) / 1'000'000));
      }
#ifdef _WIN32
      const int pollStatus = WSAPoll(pfds.data(), static_cast<ULONG>(pfds.size()), timeoutMs);
#else
      const int pollStatus = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeoutMs);
#endif
      {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < pfds.size(); ++i) {
          if (pollStatus < 0 || pfds[i].revents != 0) {
            auto &s = slots_[entries[i].slot];
            if (s.active && s.generation == entries[i].generation) {
              const Native fd = s.socket ? s.socket->fd : invalid;
              const bool write = s.write;
              ready.push_back({std::move(s.socket), std::exchange(s.completion, {})});
              removeAtLocked(s.activePos, fd, write);
            }
          }
        }
        collectExpiredLocked(Clock::now(), ready);
      }
      entries.clear();
      flushReady(ready);
    }
#endif
  }

  PollerTicket enqueue(std::shared_ptr<Socket> socket, bool write, std::chrono::nanoseconds maxWait,
                       const ExternalCompletion &completion, bool trackTicket) {
    if (!completion)
      return {};
    if (maxWait <= std::chrono::nanoseconds::zero() || (socket && socket->fd == invalid)) {
      (void)completion.signal();
      return {};
    }
#if defined(__APPLE__)
    if (kq_ < 0) {
      (void)completion.signal();
      return {};
    }
#elif defined(__linux__)
    if (ep_ < 0 || wakeFd_ < 0) {
      (void)completion.signal();
      return {};
    }
#endif
    const auto deadline = Clock::now() + maxWait;
    bool wake = false;
    PollerTicket ticket{};
    {
      std::lock_guard lock(mutex_);
      if (freeCount_ == 0) {
        socket.reset();
        // Saturation preserves progress and bounded memory, matching Console notifications.
      } else {
        const Native fd = socket ? socket->fd : invalid;
        if (fd == invalid || registerKernelLocked(fd, write)) {
          const auto idx = freeSlots_[--freeCount_];
          auto &s = slots_[idx];
          ++s.generation;
          s.socket = std::move(socket);
          s.completion = completion;
          s.deadline = deadline;
          s.activePos = static_cast<std::uint16_t>(activeCount_);
          s.write = write;
          s.active = true;
          active_[activeCount_++] = idx;
          if (trackTicket)
            ticket = {idx, s.generation};
          if (waiting_ && deadline < nextWakeup_) {
            nextWakeup_ = deadline;
            wake = true;
          }
#if !defined(__APPLE__) && !defined(__linux__)
          if (activeCount_ == 1)
            wake = true;
#endif
          if (wake)
            triggerWakeup();
          return ticket;
        }
      }
    }
    (void)completion.signal();
    return {};
  }

public:
  SocketPoller() {
    for (std::size_t i = 0; i < capacity; ++i)
      freeSlots_[i] = static_cast<std::uint16_t>(capacity - 1 - i);
#ifdef _WIN32
    (void)initialize();
#elif defined(__APPLE__)
    kq_ = ::kqueue();
    if (kq_ >= 0) {
      (void)fcntl(kq_, F_SETFD, FD_CLOEXEC);
      struct kevent ev{};
      EV_SET(&ev, wakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
      if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) != 0) {
        ::close(kq_);
        kq_ = -1;
      }
    }
    if (kq_ < 0)
      return;
#elif defined(__linux__)
    ep_ = ::epoll_create1(EPOLL_CLOEXEC);
    wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ep_ >= 0 && wakeFd_ >= 0) {
      epoll_event ev{};
      ev.events = EPOLLIN;
      ev.data.fd = wakeFd_;
      if (::epoll_ctl(ep_, EPOLL_CTL_ADD, wakeFd_, &ev) != 0) {
        ::close(wakeFd_);
        ::close(ep_);
        wakeFd_ = -1;
        ep_ = -1;
      }
    } else {
      if (wakeFd_ >= 0)
        ::close(wakeFd_);
      if (ep_ >= 0)
        ::close(ep_);
      wakeFd_ = -1;
      ep_ = -1;
    }
    if (ep_ < 0)
      return;
#endif
    std::thread([this] { worker(); }).detach();
  }

  void waitSocket(std::shared_ptr<Socket> socket, bool write, std::chrono::nanoseconds maxWait,
                  const ExternalCompletion &completion) {
    (void)enqueue(std::move(socket), write, maxWait, completion, false);
  }

  PollerTicket addTimer(std::chrono::nanoseconds maxWait, const ExternalCompletion &completion) {
    return enqueue(nullptr, false, maxWait, completion, true);
  }

  void cancel(PollerTicket ticket) noexcept {
    if (!ticket || ticket.slot >= capacity)
      return;
    std::shared_ptr<Socket> keepAlive;
    {
      std::lock_guard lock(mutex_);
      auto &s = slots_[ticket.slot];
      if (!s.active || s.generation != ticket.generation)
        return;
      const Native fd = s.socket ? s.socket->fd : invalid;
      const bool write = s.write;
      keepAlive = std::move(s.socket);
      removeAtLocked(s.activePos, fd, write);
    }
  }
};

inline SocketPoller &poller() {
  static auto *instance = new SocketPoller;
  return *instance;
}

} // namespace kalandra::runtime::tcp
