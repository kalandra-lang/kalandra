#include "Tasks.h"
#include "NativeCancellation.h"
#include "OwnedResources.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#ifdef KALANDRA_TASK_PROFILE
#include <chrono>
#define TASK_PROBE(field, amount) profile.field += (amount)
#else
#define TASK_PROBE(field, amount) ((void)0)
#endif
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace kalandra::runtime {
namespace {
enum class State {
  Free,
  Prepared,
  Runnable,
  Running,
  WaitingTask,
  WaitingGroup,
  WaitingExternal,
  WaitingTimer,
  WaitingOwned,
  Settling,
  Settled
};
struct CallFrame {
  CallFrame *parent = nullptr;
  const TaskEntry *entry = nullptr, *returnEntry = nullptr;
  void *frame = nullptr, *value = nullptr;
  std::size_t bytes = 0;
  GroupHandle lexical;
  bool initialized = false, valueInitialized = false, borrowed = false;
  Invocation execution, outcome;
};
struct BorrowedCall {
  const BorrowedTaskEntry *entry;
  AllocationLease pin;
  CallFrame *previousPersistent = nullptr;
};
constexpr std::size_t callAlignment = alignof(std::max_align_t);
constexpr std::size_t callPadded(std::size_t n) {
  return (n + callAlignment - 1) & ~(callAlignment - 1);
}
BorrowedCall &borrowedCall(CallFrame &node) {
  return *reinterpret_cast<BorrowedCall *>(reinterpret_cast<char *>(&node) +
                                           callPadded(sizeof(CallFrame)));
}
struct Record {
  std::uint64_t generation = 1;
  State state = State::Free;
  GroupHandle group;
  const TaskEntry *entry = nullptr;
  void *frame = nullptr, *value = nullptr;
  bool initialized = false, valueInitialized = false, observed = false, queued = false;
  bool pending = false;
  std::uint32_t pendingPrevious = UINT32_MAX, pendingNext = UINT32_MAX;
  bool counted = false, executing = false, cleaning = false;
  std::uint32_t readers = 0;
  bool externalPending = false;
  std::chrono::steady_clock::time_point wakeAt{};
  std::uint64_t externalEpoch = 0;
  std::atomic<bool> cancel{false};
  std::thread::id runner{}, lastRunner{};
  std::uintptr_t interruptThread = 0;
  Invocation execution, outcome;
  CallFrame *call = nullptr, *returned = nullptr;
  CallFrame *persistent = nullptr;
  bool callPushed = false, callCompleted = false, callTail = false;
  bool cleanupPending = false;
  Failure cleanupFailure;
  Failure failure;
  TaskHandle dependency;
  GroupHandle groupDependency;
};
struct GroupHold {
  Invocation ownership;
};
struct Group {
  std::uint64_t generation = 1;
  bool used = false, closed = false, settled = false, observed = false, disposing = false;
  TaskHandle owner;
  CallFrame *callOwner = nullptr;
  BorrowedGroupOwner *persistentOwner = nullptr;
  GroupHold *ownerHold = nullptr;
  bool detached = false;
  GroupHandle parent;
  std::uint32_t depth = 0, outstanding = 0;
  Failure failure;
};
// Fixed storage, allocated once with the core. Read the current bitmap on every
// advance: callbacks can unlock the core and allow other slots to be recycled.
// Increasing slot order preserves the old sweep's settlement/failure ordering.
class Slots {
  std::uint32_t size_;
  std::vector<std::uint64_t> words_;

public:
  explicit Slots(std::uint32_t size) : size_(size), words_((std::size_t(size) + 63) / 64) {}
  void insert(std::uint32_t slot) { words_[slot / 64] |= std::uint64_t{1} << (slot % 64); }
  void erase(std::uint32_t slot) { words_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64)); }
  std::uint32_t next(std::uint32_t start) const {
    if (start >= size_)
      return size_;
    auto word = start / 64;
    auto bits = words_[word] >> (start % 64);
    // Contiguous occupied slots (and a lone group) need no bit-position decode.
    if (bits & 1)
      return start;
    if (bits)
      return start + std::countr_zero(bits);
    while (!bits) {
      if (++word == words_.size())
        return size_;
      bits = words_[word];
    }
    return word * 64 + std::countr_zero(bits);
  }
  std::uint32_t size() const { return size_; }
};
bool aligned(const void *pointer, std::size_t alignment) {
  return pointer && reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}
bool validAlignment(std::size_t alignment) {
  return alignment && !(alignment & (alignment - 1)) && alignment <= alignof(std::max_align_t);
}
Failure invocationFailure(const Invocation &context) {
  return {TaskFailure::Fault, context.status() ? context.status() : 3, context.line(),
          context.column()};
}
} // namespace
struct TaskCore::Impl : std::enable_shared_from_this<TaskCore::Impl> {
  TaskLimits limits;
  TaskStats stats;
#ifdef KALANDRA_TASK_PROFILE
  TaskProfile profile;
  using ProfileClock = std::chrono::steady_clock;
  static std::uint64_t nanos(ProfileClock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - start)
        .count();
  }
#endif
  void acquire(std::unique_lock<std::mutex> &lock) {
#ifdef KALANDRA_TASK_PROFILE
    bool contended = !lock.try_lock();
    std::uint64_t elapsed = 0;
    if (contended) {
      const auto start = ProfileClock::now();
      lock.lock();
      elapsed = nanos(start);
    }
    ++profile.locks;
    profile.contendedLocks += contended;
    profile.lockWaitNs += elapsed;
#else
    lock.lock();
#endif
  }
  template <class Predicate>
  void wait(std::unique_lock<std::mutex> &lock, std::condition_variable &condition, Predicate ready,
            bool controller = false) {
#ifndef KALANDRA_TASK_PROFILE
    (void)controller;
#endif
#ifdef KALANDRA_TASK_PROFILE
    while (!ready()) {
      ++profile.waits;
      const auto start = ProfileClock::now();
      condition.wait(lock);
      ++profile.waitReturns;
      profile.controllerWaitReturns += controller;
      profile.workerWaitReturns += !controller;
      profile.waitNs += nanos(start);
    }
#else
    condition.wait(lock, ready);
#endif
  }
  std::chrono::steady_clock::time_point nextTimer() const {
    auto deadline = std::chrono::steady_clock::time_point::max();
    for (auto slot = unfinishedTasks.next(0); slot < unfinishedTasks.size();
         slot = unfinishedTasks.next(slot + 1))
      if (tasks[slot].state == State::WaitingTimer)
        deadline = std::min(deadline, tasks[slot].wakeAt);
    return deadline;
  }
  template <class Predicate>
  void waitForProgress(std::unique_lock<std::mutex> &lock, std::condition_variable &condition,
                       Predicate ready, bool controller = false) {
    while (!ready()) {
      if (!stats.timers) {
        wait(
            lock, condition, [&] { return ready() || stats.timers; }, controller);
        continue;
      }
      const auto deadline = nextTimer();
#ifdef KALANDRA_TASK_PROFILE
      ++profile.waits;
      const auto start = ProfileClock::now();
#endif
      const auto status = condition.wait_until(lock, deadline);
#ifdef KALANDRA_TASK_PROFILE
      ++profile.waitReturns;
      profile.controllerWaitReturns += controller;
      profile.workerWaitReturns += !controller;
      profile.waitNs += nanos(start);
#endif
      if (status == std::cv_status::timeout)
        return;
    }
  }
  std::unique_ptr<Record[]> tasks;
  std::unique_ptr<Group[]> groups;
  std::unique_ptr<TaskHandle[]> queue;
  Slots liveTasks, unfinishedTasks, liveGroups;
  std::uint32_t queueHead = 0, queueCount = 0;
  std::uint32_t pendingHead = UINT32_MAX, pendingTail = UINT32_MAX, pendingCount = 0;
  std::uint64_t attempts = 0, failAt = 0;
  BorrowedGroupOwner *cleanupHead = nullptr, *cleanupTail = nullptr;
  std::uint64_t cleanupJobs = 0;
  std::mutex mutex;
  std::condition_variable workerChanged, controllerChanged;
  std::vector<std::jthread> workers;
  std::jthread interruptWorker;
  std::condition_variable interruptChanged;
  bool nativeInterrupts = false;
  std::uint64_t revision = 0;
  std::size_t remaining = 0;
  std::uint32_t running = 0, pumping = 0, preparing = 0;
  bool draining = false, controlling = false, stopping = false;
  bool externalControllerWaiting = false;
  // Set before workers start; opaque ClosedTaskRunner is the only owner.
  bool closedCaller = false;
  struct Guard {
    Impl &core;
    std::unique_lock<std::mutex> lock;
    explicit Guard(Impl &core) : core(core), lock(core.mutex, std::defer_lock) {
      core.acquire(lock);
    }
    ~Guard() { core.signal(); }
  };
  void signal(bool activate = false) {
    ++revision;
    if (limits.workers > 1 && (stopping || (draining && activate))) {
      TASK_PROBE(workerBroadcasts, 1);
      TASK_PROBE(notifications, 1);
      workerChanged.notify_all();
    } else if (limits.workers > 1 && draining) {
      // One progress event needs one waiter to re-check ready/dependency state.
      // The draining worker keeps making progress too. Drain activation wakes
      // the whole pool so long numerical callbacks can overlap immediately.
      TASK_PROBE(workerSignals, 1);
      TASK_PROBE(notifications, 1);
      workerChanged.notify_one();
    }
    // A worker notification cannot be stolen by the controller (or vice versa).
    // In particular, budget exhaustion may stop draining before leases unwind.
    if ((limits.workers == 1 && controlling && externalControllerWaiting) ||
        (limits.workers > 1 && controlling && !draining && !pumping && !preparing)) {
      TASK_PROBE(controllerSignals, 1);
      TASK_PROBE(notifications, 1);
      controllerChanged.notify_one();
    }
  }
  ~Impl() { stop(); }
  void stop() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      signal();
      interruptChanged.notify_all();
    }
    workers.clear();
    if (interruptWorker.joinable())
      interruptWorker.join();
  }
  bool start() noexcept {
    try {
      nativeInterrupts = initializeNativeCancellation();
      if (nativeInterrupts)
        interruptWorker = std::jthread([this] {
          std::unique_lock lock(mutex);
          auto pending = [&] {
            for (auto i = unfinishedTasks.next(0); i < unfinishedTasks.size();
                 i = unfinishedTasks.next(i + 1))
              if (tasks[i].executing && tasks[i].interruptThread && tasks[i].cancel.load())
                return true;
            return false;
          };
          while (!stopping) {
            interruptChanged.wait(lock, [&] { return stopping || pending(); });
            if (stopping)
              break;
            // Execution leases and pthread identities remain protected by the
            // core mutex. Late delivery reads the CURRENT boundary/token, never
            // a recycled task slot or a pointer supplied by the cancelling host.
            for (auto i = unfinishedTasks.next(0); i < unfinishedTasks.size();
                 i = unfinishedTasks.next(i + 1))
              if (tasks[i].executing && tasks[i].cancel.load())
                interruptNativeCancellation(tasks[i].interruptThread);
            interruptChanged.wait_for(lock, std::chrono::microseconds(100),
                                      [&] { return stopping; });
          }
        });
      if (limits.workers == 1)
        return true;
      workers.reserve(limits.workers);
      for (std::uint32_t i = 0; i < limits.workers; ++i)
        workers.emplace_back([this] {
          std::unique_lock lock(mutex, std::defer_lock);
          acquire(lock);
          while (!stopping) {
            wait(lock, workerChanged, [&] { return stopping || draining; });
            if (stopping)
              break;
            if (!remaining || !hasWork()) {
              draining = false;
              signal();
              continue;
            }
            if ((--remaining, step(lock))) {
              // The step's Pump signals after its lease has fully unwound.
              continue;
            }
            ++remaining;
            if (!pumping && !preparing && !stats.externalPending && !stats.timers) {
              draining = false;
              signal();
              continue;
            }
            const auto seen = revision;
            waitForProgress(lock, workerChanged,
                            [&] { return stopping || !draining || revision != seen; });
          }
        });
      return true;
    } catch (...) {
      stop();
      return false;
    }
  }
  explicit Impl(TaskLimits l)
      : limits(l), tasks(std::make_unique<Record[]>(std::size_t(l.records) + l.groups)),
        groups(std::make_unique<Group[]>(l.groups)), queue(std::make_unique<TaskHandle[]>(l.ready)),
        liveTasks(l.records + l.groups), unfinishedTasks(l.records), liveGroups(l.groups) {}
  TaskHandle handle(std::uint32_t index) const { return {index, tasks[index].generation}; }
  GroupHandle groupHandle(std::uint32_t index) const { return {index, groups[index].generation}; }
  Record *get(TaskHandle h) const {
    if (h.slot >= std::size_t(limits.records) + limits.groups)
      return nullptr;
    auto &record = tasks[h.slot];
    return record.state != State::Free && record.generation == h.generation ? &record : nullptr;
  }
  Group *get(GroupHandle h) const {
    if (h.slot >= limits.groups)
      return nullptr;
    auto &group = groups[h.slot];
    return group.used && !group.disposing && group.generation == h.generation ? &group : nullptr;
  }
  static bool reserveEnvironmentBytes(void *state, std::uint64_t bytes) noexcept {
    auto &core = *static_cast<Impl *>(state);
    Guard guard(core);
    if (bytes > core.limits.bytes - core.stats.retainedBytes)
      return false;
    core.stats.retainedBytes += static_cast<std::size_t>(bytes);
    core.stats.environmentBytes += static_cast<std::size_t>(bytes);
    core.stats.peakBytes = std::max(core.stats.peakBytes, core.stats.retainedBytes);
    return true;
  }
  static void releaseEnvironmentBytes(void *state, std::uint64_t bytes) noexcept {
    auto &core = *static_cast<Impl *>(state);
    Guard guard(core);
    core.stats.retainedBytes -= static_cast<std::size_t>(bytes);
    core.stats.environmentBytes -= static_cast<std::size_t>(bytes);
  }
  static bool reserveRecursiveBytes(void *state, std::uint64_t bytes) noexcept {
    auto &core = *static_cast<Impl *>(state);
    Guard guard(core);
    if (bytes > core.limits.bytes - core.stats.retainedBytes)
      return false;
    core.stats.retainedBytes += static_cast<std::size_t>(bytes);
    core.stats.recursiveBytes += static_cast<std::size_t>(bytes);
    core.stats.peakBytes = std::max(core.stats.peakBytes, core.stats.retainedBytes);
    return true;
  }
  static void releaseRecursiveBytes(void *state, std::uint64_t bytes) noexcept {
    auto &core = *static_cast<Impl *>(state);
    Guard guard(core);
    core.stats.retainedBytes -= static_cast<std::size_t>(bytes);
    core.stats.recursiveBytes -= static_cast<std::size_t>(bytes);
  }
  static Invocation &execution(Record &record) {
    if (!record.call)
      return record.execution;
    return record.call->borrowed ? *borrowedCall(*record.call).entry->execution
                                 : record.call->execution;
  }
  using CleanupPhase = BorrowedGroupOwner::CleanupPhase;
  bool hasWork() const { return stats.unfinished || cleanupJobs; }
  void readyCleanup(BorrowedGroupOwner &owner) {
    auto &job = owner.cleanup_;
    if (job.queued || job.busy || job.phase == CleanupPhase::Done)
      return;
    job.queued = true;
    job.next = nullptr;
    if (cleanupTail)
      cleanupTail->cleanup_.next = &owner;
    else
      cleanupHead = &owner;
    cleanupTail = &owner;
  }
  static bool within(const AllocationLease &pin, const void *pointer, std::size_t bytes) {
    const auto storage = pin.bytes();
    const auto base = reinterpret_cast<std::uintptr_t>(storage.data());
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return address >= base && bytes <= storage.size() && address - base <= storage.size() - bytes;
  }
  bool activeOwner(const BorrowedGroupOwner &owner, std::uint32_t settling = UINT32_MAX) const {
    for (auto i = liveTasks.next(0); i < liveTasks.size(); i = liveTasks.next(i + 1)) {
      const auto &record = tasks[i];
      // Failed/completed source frames are quiescent before maintenance starts.
      // Their own cleanup may settle backing, but another task's lease may not.
      if (i == settling && record.state == State::Settling && !record.executing)
        continue;
      if (persistentOwner(record.returned) == &owner)
        return true;
      if (!record.persistent)
        continue;
      for (auto *node = record.call; node; node = node->parent)
        if (persistentOwner(node) == &owner)
          return true;
    }
    return false;
  }
  bool queueCleanup(BorrowedGroupOwner &owner, AllocationLease &pin, BorrowedCleanup callbacks,
                    BorrowedGroupOwner &root, BorrowedGroupOwner *parent) {
    if (!pin || !callbacks.finish || !within(pin, &owner, sizeof(owner)))
      return false;
    if (owner.cleanupDone_.load(std::memory_order_acquire))
      return true;
    if (owner.cleanup_.phase != CleanupPhase::Idle ||
        activeOwner(owner, root.cleanup_.waiter.slot) || !bindOwner(owner))
      return false;
    auto &fence = root.cleanup_;
    if (cleanupJobs == UINT64_MAX || fence.pending == UINT64_MAX ||
        (parent && parent->cleanup_.children == UINT64_MAX))
      return false;
    owner.closing_.store(true, std::memory_order_release);
    owner.cleanupCore_ = this;
    auto &job = owner.cleanup_;
    job.parent = parent;
    job.root = &root;
    job.callbacks = callbacks;
    job.phase = CleanupPhase::Groups;
    if (&owner == &root)
      job.anchor = std::move(pin);
    else
      job.pin = std::move(pin);
    ++fence.pending;
    if (parent)
      ++parent->cleanup_.children;
    ++cleanupJobs;
    readyCleanup(owner);
    return true;
  }
  void finishFence(BorrowedGroupOwner &root, std::unique_lock<std::mutex> &lock) {
    auto &fence = root.cleanup_;
    if (fence.pending)
      return;
    const auto waiter = fence.waiter;
    const auto failure = fence.failure;
    auto anchor = std::move(fence.anchor);
    root.cleanupCore_ = nullptr;
    if (auto *record = get(waiter)) {
      record->cleanupPending = false;
      record->cleanupFailure = failure;
      wake(waiter.slot);
    }
    lock.unlock();
    anchor = {};
    acquire(lock);
  }
  void sealCleanup(BorrowedGroupOwner &root, std::unique_lock<std::mutex> &lock) {
    auto &fence = root.cleanup_;
    if (fence.sealed)
      return;
    fence.sealed = true;
    --fence.pending;
    finishFence(root, lock);
  }
  void processCleanups(std::unique_lock<std::mutex> &lock) {
    // Give ordinary runnable tasks a turn between bounded retirement batches.
    for (unsigned count = 0; cleanupHead && count < 64; ++count) {
      auto *owner = cleanupHead;
      auto &job = owner->cleanup_;
      cleanupHead = job.next;
      if (!cleanupHead)
        cleanupTail = nullptr;
      job.next = nullptr;
      job.queued = false;
      job.busy = true;
      auto &fence = job.root->cleanup_;
      if (job.phase == CleanupPhase::Groups || job.phase == CleanupPhase::WaitingGroups) {
        if (job.phase == CleanupPhase::Groups && job.callbacks.failure) {
          lock.unlock();
          const auto failure = job.callbacks.failure(job.callbacks.state);
          acquire(lock);
          if (failure.kind == TaskFailure::Fault && fence.failure.kind == TaskFailure::None)
            fence.failure = failure;
        }
        bool waiting = false;
        for (auto i = liveGroups.next(0); owner->groupCount_ && i < liveGroups.size();
             i = liveGroups.next(i + 1)) {
          auto &group = groups[i];
          if (group.persistentOwner != owner)
            continue;
          if (!group.settled)
            failGroup(groupHandle(i), {TaskFailure::Cancelled});
          waiting |= !group.settled;
        }
        if (waiting) {
          job.phase = CleanupPhase::WaitingGroups;
          job.busy = false;
          continue;
        }
        for (auto i = liveGroups.next(0); owner->groupCount_ && i < liveGroups.size();
             i = liveGroups.next(i + 1)) {
          auto &group = groups[i];
          if (group.persistentOwner != owner)
            continue;
          if (group.failure.kind == TaskFailure::Fault && fence.failure.kind == TaskFailure::None)
            fence.failure = group.failure;
          group.observed = true;
          waiting |= !dispose(groupHandle(i), lock);
        }
        if (waiting) {
          job.phase = CleanupPhase::WaitingGroups;
          job.busy = false;
          continue;
        }
        job.phase = CleanupPhase::Children;
        // Keep a sentinel until the callback finishes enqueuing children.
        ++job.children;
        if (job.callbacks.children) {
          lock.unlock();
          job.callbacks.children(*owner, job.callbacks.state);
          acquire(lock);
        }
        --job.children;
      }
      if (job.phase == CleanupPhase::Children && job.children) {
        job.busy = false;
        continue;
      }
      job.phase = CleanupPhase::Finish;
      lock.unlock();
      job.callbacks.finish(job.callbacks.state);
      acquire(lock);
      auto *parent = job.parent;
      auto *root = job.root;
      auto pin = std::move(job.pin);
      job.phase = CleanupPhase::Done;
      job.busy = false;
      owner->cleanupDone_.store(true, std::memory_order_release);
      --cleanupJobs;
      if (owner != root)
        owner->cleanupCore_ = nullptr;
      if (parent && !--parent->cleanup_.children && !parent->cleanup_.busy)
        readyCleanup(*parent);
      --root->cleanup_.pending;
      finishFence(*root, lock);
      lock.unlock();
      pin = {};
      acquire(lock);
    }
  }
  CleanupStart prepareScope(std::uint32_t slot, Invocation &ctx, ResourceControl &control,
                            bool terminal, std::unique_lock<std::mutex> &lock) {
    auto *scope = control.scope;
    if (!scope || scope->context != &ctx) {
      ctx.fault(3);
      return CleanupStart::Rejected;
    }
    if (!scope->hasDeferred.load(std::memory_order_acquire))
      return CleanupStart::Complete;
    auto &record = tasks[slot];
    if (scope->cleanupRoot)
      return scope->cleanupRoot->cleanup_.pending ? CleanupStart::Pending : CleanupStart::Complete;
    if ((!terminal && record.state != State::Running) ||
        (terminal && (record.state != State::Settling || record.executing)) ||
        record.externalPending || record.cleanupPending) {
      ctx.fault(3);
      return CleanupStart::Rejected;
    }
    // Admission is transactional: validate the entire sibling batch before
    // closing any producer. Scope ownership keeps its headers and payloads live.
    std::lock_guard resources(resourceMutex());
    if (!scope->active || scope->retiring) {
      ctx.fault(3);
      return CleanupStart::Rejected;
    }
    std::uint64_t count = 0;
    for (auto *value = scope->first; value; value = value->next) {
      auto *owner = value->deferredOwner;
      if (!owner || owner->cleanupFinished())
        continue;
      auto pin = value->pinForRetirement();
      if (!pin || !value->deferredCleanup || !value->deferredCleanup->finish ||
          !within(*pin, owner, sizeof(*owner)) ||
          owner->cleanup_.phase != BorrowedGroupOwner::CleanupPhase::Idle ||
          activeOwner(*owner, terminal ? slot : UINT32_MAX) || !bindOwner(*owner) ||
          count == UINT64_MAX - 1 || count == UINT64_MAX - cleanupJobs) {
        ctx.fault(3);
        return CleanupStart::Rejected;
      }
      ++count;
    }
    scope->retiring = true;
    if (!count)
      return CleanupStart::Complete;
    for (auto *value = scope->first; value; value = value->next) {
      auto *owner = value->deferredOwner;
      if (!owner || owner->cleanupFinished())
        continue;
      auto pin = value->pinForRetirement();
      if (!scope->cleanupRoot) {
        scope->cleanupRoot = owner;
        owner->cleanup_.pending = 1;
        owner->cleanup_.waiter = handle(slot);
      }
      // All failure conditions were checked under the same core/registry locks.
      const bool queued =
          queueCleanup(*owner, *pin, *value->deferredCleanup, *scope->cleanupRoot, nullptr);
      if (!queued)
        std::abort(); // violated trusted runtime admission invariant
    }
    record.cleanupPending = true;
    record.cleanupFailure = {};
    if (!terminal) {
      record.state = State::WaitingOwned;
      ++stats.maintenanceTasks;
    }
    sealCleanup(*scope->cleanupRoot, lock);
    return CleanupStart::Pending;
  }
  bool prepareInvocationScopes(std::uint32_t slot, Invocation &ctx,
                               std::unique_lock<std::mutex> &lock) {
    if (!ctx.hasDeferredOwnedResource())
      return true;
    auto &record = tasks[slot];
    for (auto *control = currentScope(ctx); control;) {
      const auto started = prepareScope(slot, ctx, *control, true, lock);
      if (started == CleanupStart::Pending)
        return false;
      if (started == CleanupStart::Rejected) {
        // A live foreign mutable lease cannot be retired. Keep the owning
        // scope intact until it becomes quiescent; never turn a protocol fault
        // into destruction of storage still used by another worker.
        if (record.failure.kind != TaskFailure::Fault)
          record.failure = {TaskFailure::Fault, 3};
        return false;
      }
      if (record.cleanupFailure.kind == TaskFailure::Fault &&
          record.failure.kind != TaskFailure::Fault)
        record.failure = record.cleanupFailure;
      auto *parent = control->scope->parent;
      control = parent ? parent->control : nullptr;
    }
    return true;
  }
  bool bindOwner(BorrowedGroupOwner &owner) {
    if (!owner.bound_.load(std::memory_order_acquire)) {
      owner.core_ = shared_from_this();
      owner.bound_.store(true, std::memory_order_release);
    }
    // Source continuations belong to one application runtime. Completed or
    // abandoned runtimes cannot be revived through a stale control identity.
    return owner.core_.lock().get() == this;
  }
  static CallFrame *persistentCall(Record &record) { return record.persistent; }
  static BorrowedGroupOwner *persistentOwner(CallFrame *node) {
    return node && node->borrowed ? borrowedCall(*node).entry->groups : nullptr;
  }
  void attachGroups(CallFrame &node, TaskHandle active, bool cancel = false) {
    auto *owner = persistentOwner(&node);
    if (!owner)
      return;
    for (auto i = liveGroups.next(0); i < liveGroups.size(); i = liveGroups.next(i + 1)) {
      auto &group = groups[i];
      if (group.persistentOwner == owner && group.detached) {
        group.owner = active;
        group.callOwner = &node;
        group.detached = false;
      }
      if (cancel && group.persistentOwner == owner && group.owner == active && !group.settled)
        failGroup(groupHandle(i), {TaskFailure::Cancelled});
    }
  }
  void detachGroups(CallFrame &node, TaskHandle active) {
    auto *owner = persistentOwner(&node);
    for (auto i = liveGroups.next(0); i < liveGroups.size(); i = liveGroups.next(i + 1)) {
      auto &group = groups[i];
      if (owner && group.persistentOwner == owner && group.owner == active &&
          group.callOwner == &node) {
        group.owner = {};
        group.callOwner = nullptr;
        group.detached = true;
        auto *parent = get(group.parent);
        if (!parent || parent->persistentOwner != owner)
          group.parent = {};
      }
    }
  }
  static bool reserveCallBytes(void *state, std::uint64_t bytes) noexcept {
    auto &core = *static_cast<Impl *>(state);
    Guard guard(core);
    return core.chargeCallBytes(bytes);
  }
  static void releaseCallBytes(void *state, std::uint64_t bytes) noexcept {
    auto &core = *static_cast<Impl *>(state);
    Guard guard(core);
    core.stats.callBytes -= bytes;
  }
  bool chargeCallBytes(std::uint64_t bytes) {
    if (bytes > limits.callBytes - stats.callBytes)
      return false;
    stats.callBytes += bytes;
    stats.peakCallBytes = std::max(stats.peakCallBytes, stats.callBytes);
    return true;
  }
  // Destructors and Invocation settlement may run arbitrary ownership cleanup;
  // keep them outside the scheduler mutex and never recurse through parents.
  void destroyCall(CallFrame *node, std::unique_lock<std::mutex> &lock) {
    const auto bytes = node->bytes;
    lock.unlock();
    if (node->valueInitialized)
      node->entry->result->destroy(node->value);
    node->outcome.settle();
    if (node->borrowed) {
      auto &borrow = borrowedCall(*node);
      if (node->initialized)
        borrow.entry->leave(borrow.entry->state);
      borrow.~BorrowedCall();
    } else {
      if (node->initialized)
        node->entry->destroy(node->frame);
      node->execution.settle();
    }
    node->~CallFrame();
    std::free(node);
    acquire(lock);
    stats.callBytes -= bytes;
    --stats.callFrames;
  }
  void destroyCalls(Record &record, std::unique_lock<std::mutex> &lock) {
    record.persistent = nullptr;
    if (record.returned) {
      auto *node = std::exchange(record.returned, nullptr);
      destroyCall(node, lock);
    }
    while (record.call) {
      auto *node = record.call;
      record.call = node->parent;
      destroyCall(node, lock);
    }
    record.callPushed = record.callCompleted = record.callTail = false;
  }
  void freeBytes(void *&pointer, std::size_t bytes) {
    if (pointer) {
#ifdef KALANDRA_TASK_PROFILE
      const auto start = ProfileClock::now();
#endif
      std::free(pointer);
      TASK_PROBE(frees, 1);
      TASK_PROBE(allocatorNs, nanos(start));
      pointer = nullptr;
      stats.retainedBytes -= bytes;
    }
  }
  void *allocate(std::size_t bytes) {
    TASK_PROBE(allocationAttempts, 1);
    if (++attempts == failAt || bytes > limits.bytes - stats.retainedBytes)
      return nullptr;
#ifdef KALANDRA_TASK_PROFILE
    const auto start = ProfileClock::now();
#endif
    auto *pointer = std::malloc(std::max(std::size_t(1), bytes));
    TASK_PROBE(allocatorNs, nanos(start));
    if (pointer) {
      stats.retainedBytes += bytes;
      stats.peakBytes = std::max(stats.peakBytes, stats.retainedBytes);
    }
    return pointer;
  }
  void unlinkPending(std::uint32_t index) {
    auto &record = tasks[index];
    if (!record.pending)
      return;
    if (record.pendingPrevious != UINT32_MAX)
      tasks[record.pendingPrevious].pendingNext = record.pendingNext;
    else
      pendingHead = record.pendingNext;
    if (record.pendingNext != UINT32_MAX)
      tasks[record.pendingNext].pendingPrevious = record.pendingPrevious;
    else
      pendingTail = record.pendingPrevious;
    record.pending = false;
    record.pendingPrevious = record.pendingNext = UINT32_MAX;
    --pendingCount;
  }
  void pushReady(std::uint32_t index) {
    queue[(queueHead + queueCount) % limits.ready] = handle(index);
    ++queueCount;
    tasks[index].queued = true;
    TASK_PROBE(readyPushes, 1);
  }
  void fillReady() {
    while (queueCount < limits.ready && pendingHead != UINT32_MAX) {
      const auto index = pendingHead;
      auto &record = tasks[index];
      unlinkPending(index);
      // Membership is removed before settlement/reclamation, never recovered
      // by scanning reusable slots. Ring entries still carry generations.
      if (record.state != State::Runnable || record.executing || record.cleaning || record.queued)
        continue;
      pushReady(index);
      TASK_PROBE(pendingPromotions, 1);
    }
  }
  void enqueue(std::uint32_t index, bool admission = false) {
    auto &record = tasks[index];
    if (record.queued || record.pending || record.state != State::Runnable || record.executing ||
        record.cleaning)
      return;
    TASK_PROBE(runnableEvents, 1);
    if ((admission || pendingHead == UINT32_MAX) && queueCount < limits.ready) {
      pushReady(index);
      return;
    }
    // Existing admitted tasks may resume while the ready ring is full. Track
    // them once in a record-bounded FIFO, ahead of later deferred resumptions.
    record.pending = true;
    record.pendingPrevious = pendingTail;
    if (pendingTail != UINT32_MAX)
      tasks[pendingTail].pendingNext = index;
    else
      pendingHead = index;
    pendingTail = index;
    ++pendingCount;
#ifdef KALANDRA_TASK_PROFILE
    profile.peakPending = std::max<std::uint64_t>(profile.peakPending, pendingCount);
#endif
  }
  void wake(std::uint32_t index) {
    auto &record = tasks[index];
    if (record.externalPending)
      return;
    if (record.state == State::WaitingTask || record.state == State::WaitingGroup ||
        record.state == State::WaitingExternal || record.state == State::WaitingTimer ||
        record.state == State::WaitingOwned) {
      if (record.state == State::WaitingTimer)
        --stats.timers;
      record.state = State::Runnable;
      --stats.maintenanceTasks;
      record.dependency = {};
      record.groupDependency = {};
    }
    enqueue(index);
  }
  GroupHandle open(TaskHandle owner, GroupHandle lexical = {}, bool independent = false) {
    std::uint32_t depth = 0;
    GroupHandle parent;
    if (owner) {
      auto *task = get(owner);
      if (!task || task->state != State::Running)
        return {};
      if (independent && task->cancel.load())
        return {};
      parent = lexical ? lexical : independent ? GroupHandle{} : task->group;
      if ((!independent && !get(parent)) || (lexical && !owned(parent, owner))) {
        execution(*task).fault(3);
        return {};
      }
      depth = parent ? get(parent)->depth + 1 : 1;
      if (depth > limits.depth) {
        failGroup(task->group, {TaskFailure::AdmissionRejected});
        return {};
      }
    }
    for (std::uint32_t i = 0; i < limits.groups; ++i) {
      auto &group = groups[i];
      if (!group.used) {
        group.used = true;
        liveGroups.insert(i);
        group.owner = owner;
        group.parent = parent;
        if (auto *ancestor = get(parent); ancestor && ancestor->failure.kind != TaskFailure::None) {
          group.failure = {TaskFailure::Cancelled};
          group.closed = true;
        }
        group.depth = depth;
        ++stats.groups;
        return groupHandle(i);
      }
    }
    if (auto *task = get(owner))
      execution(*task).fault(2);
    return {};
  }
  void failGroup(GroupHandle h, Failure failure) {
    auto *group = get(h);
    if (!group || group->settled)
      return;
    if (group->failure.kind == TaskFailure::None)
      group->failure = failure;
    group->closed = true;
    // Mark descendants iteratively. No recursion proportional to nesting depth.
    bool changed;
    do {
      changed = false;
      for (auto i = unfinishedTasks.next(0); i < unfinishedTasks.size();
           i = unfinishedTasks.next(i + 1)) {
        auto &task = tasks[i];
        auto *parent = get(task.group);
        if (task.state != State::Free && task.state != State::Settled && parent &&
            parent->failure.kind != TaskFailure::None && !task.cancel) {
          task.cancel = true;
          if (nativeInterrupts) {
            if (task.executing)
              interruptNativeCancellation(task.interruptThread);
            interruptChanged.notify_one();
          }
          wake(i);
          changed = true;
        }
      }
      for (auto i = liveGroups.next(0); i < liveGroups.size(); i = liveGroups.next(i + 1)) {
        auto &nested = groups[i];
        auto *owner = get(nested.owner);
        auto *parent = get(nested.parent);
        if (nested.used && !nested.settled &&
            ((owner && owner->cancel) || (parent && parent->failure.kind != TaskFailure::None)) &&
            nested.failure.kind == TaskFailure::None) {
          nested.failure = {TaskFailure::Cancelled};
          nested.closed = true;
          changed = true;
        }
      }
    } while (changed);
  }
  TaskHandle rejected(GroupHandle h, Failure failure) {
    auto &record = tasks[limits.records + h.slot];
    if (record.state == State::Free) {
      record.state = State::Settled;
      liveTasks.insert(limits.records + h.slot);
      record.group = h;
      record.failure = failure;
      ++stats.records;
      ++stats.publications;
    }
    failGroup(h, failure);
    return handle(limits.records + h.slot);
  }
  TaskHandle submit(GroupHandle h, const TaskEntry &entry, Packet packet, TaskHandle owner,
                    std::unique_lock<std::mutex> &lock) {
    auto *group = get(h);
    if (!group || group->detached || group->owner != owner || group->settled)
      return {};
    if (group->closed || group->failure.kind != TaskFailure::None)
      return rejected(h, group->failure.kind == TaskFailure::None
                             ? Failure{TaskFailure::AdmissionRejected}
                             : group->failure);
    if (entry.revision != 1 || !entry.result || !entry.construct || !entry.destroy ||
        !entry.resume || !entry.result->copy || !entry.result->destroy || !entry.result->identity ||
        !entry.frameBytes || !entry.result->bytes || !validAlignment(entry.frameAlignment) ||
        !validAlignment(entry.packetAlignment) || !validAlignment(entry.result->alignment) ||
        packet.layout != entry.packetLayout || packet.bytes != entry.packetBytes ||
        (packet.bytes && !aligned(packet.data, entry.packetAlignment)) ||
        entry.frameBytes > limits.bytes || entry.result->bytes > limits.bytes - entry.frameBytes)
      return rejected(h, {TaskFailure::AdmissionRejected});
    if (stats.unfinished == limits.inFlight || queueCount == limits.ready ||
        entry.frameBytes + entry.result->bytes > limits.bytes - stats.retainedBytes)
      return rejected(h, {TaskFailure::AdmissionRejected});
    std::uint32_t slot = 0;
    while (slot < limits.records && tasks[slot].state != State::Free)
      ++slot;
    if (slot == limits.records)
      return rejected(h, {TaskFailure::AdmissionRejected});
    auto &task = tasks[slot];
    task.state = State::Prepared;
    liveTasks.insert(slot);
    unfinishedTasks.insert(slot);
    ++preparing;
    task.group = h;
    task.entry = &entry;
    task.counted = true;
    ++group->outstanding;
    ++stats.unfinished;
    ++stats.records;
    task.value = allocate(entry.result->bytes);
    task.execution.bindCancellation(&task.cancel);
    task.execution.bindRecursiveBudget(this, reserveRecursiveBytes, releaseRecursiveBytes);
    task.execution.bindEnvironmentBudget(this, reserveEnvironmentBytes, releaseEnvironmentBytes);
    task.outcome.bindCancellation(&task.cancel);
    task.outcome.bindRecursiveBudget(this, reserveRecursiveBytes, releaseRecursiveBytes);
    task.outcome.bindEnvironmentBudget(this, reserveEnvironmentBytes, releaseEnvironmentBytes);
    if (task.value)
      task.frame = allocate(entry.frameBytes);
    if (!task.value || !task.frame)
      task.execution.fault(2);
    else {
      task.runner = std::this_thread::get_id();
      lock.unlock();
      try {
        task.initialized = entry.construct(task.execution, task.frame, packet);
      } catch (...) {
        task.execution.fault(3);
      }
      // A checked constructor can stop before initializing its frame when its
      // cancellation token is raised. That is not malformed packet data. A
      // recorded allocation/value fault still takes precedence below.
      if (!task.initialized && !task.cancel.load(std::memory_order_acquire))
        task.execution.fault(3);
      acquire(lock);
      task.runner = {};
    }
    --preparing;
    if (task.execution.status() || !task.initialized) {
      task.failure = task.execution.status() ? invocationFailure(task.execution)
                                             : Failure{TaskFailure::Cancelled};
      task.state = State::Settling;
      ++stats.maintenanceTasks;
      failGroup(h, task.failure);
    } else {
      task.state = State::Runnable;
      enqueue(slot, true);
    }
    return handle(slot);
  }
  bool owned(GroupHandle h, TaskHandle owner) const {
    auto *group = get(h);
    return group && !group->detached && group->owner == owner;
  }
  bool close(GroupHandle h, TaskHandle owner) {
    auto *group = get(h);
    if (!group || group->detached || group->owner != owner)
      return false;
    group->closed = true;
    return true;
  }
  bool read(TaskHandle h, const ValueLayout &layout, Invocation &destination, void *output,
            TaskHandle owner, std::unique_lock<std::mutex> &lock) {
    auto *task = get(h);
    if (destination.executionStatus() || !task || !owned(task->group, owner) ||
        task->state != State::Settled || task->failure.kind != TaskFailure::None ||
        !task->valueInitialized || &layout != task->entry->result ||
        !aligned(output, layout.alignment)) {
      destination.fault(3);
      return false;
    }
    task->observed = true;
    ++task->readers;
    lock.unlock();
    const auto copied = layout.copy(destination, output, task->outcome, task->value);
    acquire(lock);
    --task->readers;
    if (auto *group = get(task->group);
        group && group->persistentOwner &&
        group->persistentOwner->cleanup_.phase == CleanupPhase::WaitingGroups)
      readyCleanup(*group->persistentOwner);
    if (!copied && !destination.poll())
      destination.fault(3);
    return copied;
  }
  bool dispose(GroupHandle h, std::unique_lock<std::mutex> &lock) {
    auto *group = get(h);
    if (!group || !group->settled)
      return false;
    for (auto i = liveTasks.next(0); i < liveTasks.size(); i = liveTasks.next(i + 1))
      if (tasks[i].state != State::Free && tasks[i].group == h && tasks[i].readers)
        return false;
    group->disposing = true;
    for (auto i = liveTasks.next(0); i < liveTasks.size(); i = liveTasks.next(i + 1)) {
      auto &task = tasks[i];
      if (task.state == State::Free || task.group != h)
        continue;
      lock.unlock();
      if (task.valueInitialized)
        task.entry->result->destroy(task.value);
      task.outcome.settle();
      acquire(lock);
      if (task.entry)
        freeBytes(task.value, task.entry->result->bytes);
      unlinkPending(i);
      const auto generation = task.generation + 1;
      liveTasks.erase(i);
      task.~Record();
      new (&task) Record;
      task.generation = generation;
      --stats.records;
    }
    auto *hold = std::exchange(group->ownerHold, nullptr);
    if (group->persistentOwner)
      --group->persistentOwner->groupCount_;
    const auto generation = group->generation + 1;
    liveGroups.erase(h.slot);
    *group = Group{};
    group->generation = generation;
    --stats.groups;
    if (hold) {
      lock.unlock();
      hold->~GroupHold();
      acquire(lock);
      void *memory = hold;
      freeBytes(memory, sizeof(GroupHold));
    }
    return true;
  }
  void maintain(std::unique_lock<std::mutex> &lock) {
    if (cleanupHead)
      processCleanups(lock);
    bool changed;
    do {
      changed = false;
      const auto now =
          stats.timers ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      TASK_PROBE(maintenancePasses, 1);
      for (auto i = liveGroups.next(0); i < liveGroups.size(); i = liveGroups.next(i + 1)) {
        TASK_PROBE(groupVisits, 1);
        auto &group = groups[i];
        if (!group.used || !group.closed || group.settled || group.outstanding)
          continue;
        bool pendingNested = false;
        for (auto j = liveGroups.next(0); j < liveGroups.size(); j = liveGroups.next(j + 1))
          pendingNested |=
              groups[j].used && groups[j].parent == groupHandle(i) && !groups[j].settled;
        if (pendingNested)
          continue;
        if (group.failure.kind == TaskFailure::None)
          for (auto j = liveTasks.next(0); j < liveTasks.size(); j = liveTasks.next(j + 1))
            if (tasks[j].state != State::Free && tasks[j].group == groupHandle(i) &&
                !tasks[j].observed) {
              group.failure = {TaskFailure::Fault, 3};
              break;
            }
        group.settled = true;
        if (group.persistentOwner &&
            group.persistentOwner->cleanup_.phase == CleanupPhase::WaitingGroups)
          readyCleanup(*group.persistentOwner);
        changed = true;
      }
      // Runnable, running and prepared records cannot advance maintenance.
      // Keep group settlement above independent of this task-only shortcut.
      if (!stats.maintenanceTasks) {
        TASK_PROBE(idleTaskPasses, 1);
        continue;
      }
      TASK_PROBE(taskPasses, 1);
      for (auto i = unfinishedTasks.next(0); i < unfinishedTasks.size();
           i = unfinishedTasks.next(i + 1)) {
        TASK_PROBE(recordVisits, 1);
        auto &task = tasks[i];
        if (task.state == State::WaitingTimer && now >= task.wakeAt) {
          wake(i);
          changed = true;
        }
        if (task.state == State::WaitingTask) {
          auto *dependency = get(task.dependency);
          if (dependency && dependency->state == State::Settled) {
            wake(i);
            changed = true;
          }
        } else if (task.state == State::WaitingGroup) {
          auto *dependency = get(task.groupDependency);
          if (dependency && dependency->settled) {
            wake(i);
            changed = true;
          }
        }
        if (task.state != State::Settling || task.executing || task.cleaning ||
            task.externalPending || task.cleanupPending)
          continue;
        if (task.returned)
          attachGroups(*task.returned, handle(i), true);
        for (auto *node = task.call; node; node = node->parent)
          attachGroups(*node, handle(i), true);
        bool pending = false;
        for (auto j = liveGroups.next(0); j < liveGroups.size(); j = liveGroups.next(j + 1)) {
          auto &nested = groups[j];
          if (!nested.used || nested.owner != handle(i))
            continue;
          if (!nested.closed) {
            if (task.failure.kind == TaskFailure::None)
              task.failure = {TaskFailure::Fault, 3};
            failGroup(groupHandle(j), {TaskFailure::Cancelled});
          }
          pending |= !nested.settled;
          if (nested.settled && !nested.observed && task.failure.kind == TaskFailure::None)
            task.failure = nested.failure.kind == TaskFailure::None ? Failure{TaskFailure::Fault, 3}
                                                                    : nested.failure;
        }
        if (pending)
          continue;
        // Source callbacks have returned and owned children have settled.
        // Preflight asynchronous lexical resources before any frame, file,
        // ownership ledger or native borrowed-call lease can be destroyed.
        if (task.returned) {
          auto &ctx = task.returned->borrowed ? *borrowedCall(*task.returned).entry->execution
                                              : task.returned->execution;
          if (ctx.hasDeferredOwnedResource() && !prepareInvocationScopes(i, ctx, lock))
            continue;
        }
        for (auto *node = task.call; node; node = node->parent) {
          auto &ctx = node->borrowed ? *borrowedCall(*node).entry->execution : node->execution;
          if (ctx.hasDeferredOwnedResource() && !prepareInvocationScopes(i, ctx, lock)) {
            pending = true;
            break;
          }
        }
        if (pending || (task.execution.hasDeferredOwnedResource() &&
                        !prepareInvocationScopes(i, task.execution, lock)))
          continue;
        task.cleaning = true;
        lock.unlock();
        if (task.failure.kind != TaskFailure::None) {
          if (task.valueInitialized) {
            task.entry->result->destroy(task.value);
            task.valueInitialized = false;
          }
          task.outcome.settle();
        }
        acquire(lock);
        destroyCalls(task, lock);
        lock.unlock();
        if (task.initialized) {
          task.entry->destroy(task.frame);
          task.initialized = false;
        }
        acquire(lock);
        freeBytes(task.frame, task.entry->frameBytes);
        for (auto j = liveGroups.next(0); j < liveGroups.size(); j = liveGroups.next(j + 1))
          if (groups[j].used && groups[j].owner == handle(i))
            dispose(groupHandle(j), lock);
        lock.unlock();
        task.execution.settle();
        acquire(lock);
        // A completion is not public until cleanup finishes. Cancellation can
        // arrive while destructors run outside the lock; acknowledge it before
        // publishing rather than expose a success that never became settled.
        if (task.cancel.load() && task.failure.kind == TaskFailure::None) {
          task.failure = {TaskFailure::Cancelled};
          lock.unlock();
          if (task.valueInitialized) {
            task.entry->result->destroy(task.value);
            task.valueInitialized = false;
          }
          task.outcome.settle();
          acquire(lock);
        }
        task.cleaning = false;
        unlinkPending(i);
        task.state = State::Settled;
        --stats.maintenanceTasks;
        unfinishedTasks.erase(i);
        if (task.counted) {
          --get(task.group)->outstanding;
          --stats.unfinished;
          task.counted = false;
        }
        ++stats.publications;
        if (task.failure.kind != TaskFailure::None)
          failGroup(task.group, task.failure);
        changed = true;
      }
    } while (changed);
  }
  void finish(std::uint32_t slot, Failure failure) {
    auto &task = tasks[slot];
    unlinkPending(slot);
    task.failure = failure;
    if (task.state == State::WaitingTimer)
      --stats.timers;
    if (task.state != State::Settling && task.state != State::WaitingTask &&
        task.state != State::WaitingGroup && task.state != State::WaitingExternal &&
        task.state != State::WaitingTimer && task.state != State::WaitingOwned)
      ++stats.maintenanceTasks;
    task.state = State::Settling;
    if (failure.kind != TaskFailure::None) {
      failGroup(task.group, failure);
      for (auto i = liveGroups.next(0); i < liveGroups.size(); i = liveGroups.next(i + 1))
        if (groups[i].used && groups[i].owner == handle(slot))
          failGroup(groupHandle(i), {TaskFailure::Cancelled});
    }
  }
  bool step(std::unique_lock<std::mutex> &lock) {
    TASK_PROBE(steps, 1);
    struct Pump {
      Impl &core;
      std::uint64_t publications;
      bool progressed = false;
      explicit Pump(Impl &core) : core(core), publications(core.stats.publications) {
        ++core.pumping;
      }
      ~Pump() {
        --core.pumping;
        if (progressed || publications != core.stats.publications)
          core.signal();
      }
    } pump(*this);
    maintain(lock);
    fillReady();
    if (!queueCount) {
      pump.progressed = cleanupHead != nullptr;
      return pump.progressed;
    }
    pump.progressed = true;
    const auto h = queue[queueHead];
    queueHead = (queueHead + 1) % limits.ready;
    --queueCount;
    // Keep the dispatch vacancy available during the callback. With a one-slot
    // ring a child must still be admissible while its parent is deferred.
    auto *record = get(h);
    if (!record)
      return true;
    record->queued = false;
    // A lease extends until resume actually returns, even after await/yield or
    // complete changes the scheduling state. Never re-enter a live callback.
    if (record->executing || record->cleaning || record->state != State::Runnable)
      return true;
    record->state = State::Running;
    if (record->cancel.load())
      finish(h.slot, {TaskFailure::Cancelled});
    else {
      record->executing = true;
      record->runner = std::this_thread::get_id();
      record->interruptThread = nativeInterrupts ? nativeCancellationThread() : 0;
      auto &activeContext = execution(*record);
      activeContext.allowNativeInterrupts(record->interruptThread != 0);
      auto *activeEntry = record->call ? record->call->entry : record->entry;
      if (record->lastRunner != std::thread::id{} && record->lastRunner != record->runner)
        ++stats.migrations;
      record->lastRunner = record->runner;
      ++running;
      stats.peakRunning = std::max(stats.peakRunning, running);
      ++stats.resumes;
      Task task(*this, h.slot);
      lock.unlock();
      try {
        activeEntry->resume(task);
      } catch (...) {
        activeContext.fault(3);
      }
      acquire(lock);
      record->executing = false;
      record->runner = {};
      record->interruptThread = 0;
      --running;
      if (activeContext.status())
        finish(h.slot, invocationFailure(activeContext));
      else if (record->cancel.load() &&
               (record->state != State::Settling || record->failure.kind == TaskFailure::None))
        finish(h.slot, {TaskFailure::Cancelled});
      else if (record->state == State::Running)
        finish(h.slot, {TaskFailure::Fault, 3});
      if (record->state == State::Runnable && record->callPushed) {
        auto *previous = record->call;
        record->call = std::exchange(record->returned, nullptr);
        if (persistentOwner(record->call))
          record->persistent = record->call;
        record->callPushed = false;
        if (record->callTail) {
          record->callTail = false;
          record->call->parent = previous->parent;
          record->call->returnEntry = previous->returnEntry;
          record->cleaning = true;
          destroyCall(previous, lock);
          record->cleaning = false;
        }
      } else if (record->state == State::Runnable && record->callCompleted) {
        record->returned = record->call;
        if (persistentOwner(record->call))
          record->persistent = borrowedCall(*record->call).previousPersistent;
        record->call = record->call->parent;
        record->callCompleted = false;
      }
      // Yield/dependency wakeups observed during a live callback cannot queue
      // its frame until this execution lease has actually returned.
      enqueue(h.slot);
    }
    maintain(lock);
    fillReady();
    return true;
  }
};
std::unique_ptr<TaskCore> TaskCore::create(TaskLimits limits) noexcept {
  return create(limits, false);
}
std::unique_ptr<TaskCore> TaskCore::create(TaskLimits limits, bool closedCaller) noexcept {
  if (!limits.workers || limits.workers > 64 || !limits.records || !limits.groups ||
      !limits.ready || !limits.inFlight ||
      std::uint64_t(limits.records) + limits.groups >= UINT32_MAX || !limits.bytes ||
      !limits.callBytes)
    return {};
  try {
    auto impl = std::make_shared<Impl>(limits);
    impl->closedCaller = closedCaller;
    if (!impl->start())
      return {};
    return std::unique_ptr<TaskCore>(new TaskCore(std::move(impl)));
  } catch (...) {
    return {};
  }
}
TaskCore::TaskCore(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
bool TaskCore::drainToCompletion() noexcept {
  if (drain())
    return true;
  for (;;) {
    const auto before = stats().resumes;
    if (drain())
      return true;
    const auto after = stats();
    // A scheduler step budget is not a language execution limit. Continue
    // productive batches; reject a deadlock with no external progress source.
    if (after.resumes == before && !after.externalPending && !after.timers)
      return false;
  }
}
struct ClosedTaskRunner::Impl {
  std::unique_ptr<TaskCore> core;
  std::mutex runMutex;
  explicit Impl(std::unique_ptr<TaskCore> value) : core(std::move(value)) {}
  bool drain() { return core->drainToCompletion(); }
};
std::unique_ptr<ClosedTaskRunner> ClosedTaskRunner::create(TaskLimits limits) noexcept {
  try {
    auto core = TaskCore::create(limits, true);
    if (!core)
      return {};
    return std::unique_ptr<ClosedTaskRunner>(
        new ClosedTaskRunner(std::make_unique<Impl>(std::move(core))));
  } catch (...) {
    return {};
  }
}
ClosedTaskRunner::ClosedTaskRunner(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ClosedTaskRunner::~ClosedTaskRunner() = default;
Failure ClosedTaskRunner::run(const TaskEntry &entry, Packet packet, const ValueLayout &layout,
                              Invocation &context, void *output) noexcept {
  std::lock_guard runLock(impl_->runMutex);
  if (context.executionStatus())
    return invocationFailure(context);
  auto &core = *impl_->core;
  const auto group = core.openRoot();
  if (!group)
    return {TaskFailure::AdmissionRejected};
  const auto child = core.submit(group, entry, packet);
  const bool drained = child && impl_->drain();
  auto failure = core.failure(group);
  if (failure.kind == TaskFailure::None)
    failure = child ? core.failure(child) : Failure{TaskFailure::AdmissionRejected};
  if (!drained && failure.kind == TaskFailure::None)
    failure = {TaskFailure::Fault, 3};
  bool copied = false;
  if (failure.kind == TaskFailure::None) {
    copied = core.observe(child, layout, context, output);
    if (!copied)
      failure = invocationFailure(context);
  }
  if (!drained)
    core.cancel(group);
  const bool closed = core.close(group);
  const bool settled = impl_->drain();
  const bool released = settled && core.release(group);
  if (!closed || !settled || !released) {
    if (copied)
      layout.destroy(output);
    if (failure.kind == TaskFailure::None)
      failure = {TaskFailure::Fault, 3};
  }
  return failure;
}
TaskStats ClosedTaskRunner::stats() const noexcept { return impl_->core->stats(); }
void ClosedTaskRunner::failAllocationAt(std::uint64_t attempt) noexcept {
  std::lock_guard runLock(impl_->runMutex);
  impl_->core->failAllocationAt(attempt);
}
TaskCore::~TaskCore() {
  {
    Impl::Guard guard(*impl_);
    for (std::uint32_t i = 0; i < impl_->limits.groups; ++i)
      if (impl_->groups[i].used)
        impl_->failGroup(impl_->groupHandle(i), {TaskFailure::Cancelled});
  }
  // Destruction is not concurrent with external host calls. Already running
  // kernels acknowledge cancellation before any of their storage is reclaimed.
  while (!drain()) {
  }
  impl_->stop();
  Impl::Guard guard(*impl_);
  for (std::uint32_t i = 0; i < impl_->limits.groups; ++i)
    if (impl_->groups[i].used)
      impl_->dispose(impl_->groupHandle(i), guard.lock);
}
GroupHandle TaskCore::openRoot() noexcept {
  Impl::Guard guard(*impl_);
  return impl_->open({});
}
TaskHandle TaskCore::submit(GroupHandle group, const TaskEntry &entry, Packet packet) noexcept {
  Impl::Guard guard(*impl_);
  return impl_->submit(group, entry, packet, {}, guard.lock);
}
bool TaskCore::close(GroupHandle group) noexcept {
  Impl::Guard guard(*impl_);
  return impl_->close(group, {});
}
bool TaskCore::cancel(GroupHandle group, bool deadline) noexcept {
  Impl::Guard guard(*impl_);
  if (!impl_->owned(group, {}))
    return false;
  impl_->failGroup(group, {deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled});
  return true;
}
bool TaskCore::settled(GroupHandle h) const noexcept {
  std::lock_guard lock(impl_->mutex);
  auto *group = impl_->get(h);
  return group && group->settled;
}
Failure TaskCore::failure(GroupHandle h) const noexcept {
  std::lock_guard lock(impl_->mutex);
  auto *group = impl_->get(h);
  return group ? group->failure : Failure{TaskFailure::Fault, 3};
}
bool TaskCore::settled(TaskHandle h) const noexcept {
  std::lock_guard lock(impl_->mutex);
  auto *task = impl_->get(h);
  return task && task->state == State::Settled;
}
Failure TaskCore::failure(TaskHandle h) const noexcept {
  std::lock_guard lock(impl_->mutex);
  auto *task = impl_->get(h);
  return task ? task->failure : Failure{TaskFailure::Fault, 3};
}
bool TaskCore::observe(TaskHandle h, const ValueLayout &layout, Invocation &context,
                       void *output) noexcept {
  Impl::Guard guard(*impl_);
  return impl_->read(h, layout, context, output, {}, guard.lock);
}
bool TaskCore::release(GroupHandle group) noexcept {
  Impl::Guard guard(*impl_);
  return impl_->owned(group, {}) && impl_->dispose(group, guard.lock);
}
bool TaskCore::step() noexcept {
  Impl::Guard guard(*impl_);
  if (impl_->controlling || impl_->pumping >= impl_->limits.workers)
    return false;
  for (std::uint32_t i = 0; i < impl_->limits.records; ++i)
    if (impl_->tasks[i].runner == std::this_thread::get_id())
      return false;
  return impl_->step(guard.lock);
}
bool TaskCore::drain(std::size_t steps) noexcept {
  Impl::Guard guard(*impl_);
  if (impl_->controlling || impl_->pumping)
    return false;
  for (std::uint32_t i = 0; i < impl_->limits.records; ++i)
    if (impl_->tasks[i].runner == std::this_thread::get_id())
      return false;
  impl_->controlling = true;
  impl_->maintain(guard.lock);
  if (!impl_->hasWork() && !impl_->pumping && !impl_->preparing) {
    impl_->controlling = false;
    return true;
  }
  if (impl_->limits.workers == 1) {
    impl_->draining = true;
    while (steps) {
      if (impl_->step(guard.lock)) {
        --steps;
      } else if (impl_->stats.externalPending || impl_->stats.timers) {
        const auto seen = impl_->revision;
        impl_->externalControllerWaiting = true;
        impl_->waitForProgress(
            guard.lock, impl_->controllerChanged, [&] { return impl_->revision != seen; }, true);
        impl_->externalControllerWaiting = false;
      } else {
        break;
      }
    }
    impl_->draining = false;
  } else {
    impl_->remaining = steps;
    impl_->draining = true;
    impl_->signal(true);
    impl_->wait(
        guard.lock, impl_->controllerChanged,
        [&] { return !impl_->draining && !impl_->pumping && !impl_->preparing; }, true);
  }
  impl_->controlling = false;
  return !impl_->hasWork();
}
TaskStats TaskCore::stats() const noexcept {
  std::lock_guard lock(impl_->mutex);
  auto stats = impl_->stats;
  stats.ready = impl_->queueCount;
  stats.pending = impl_->pendingCount;
  for (std::uint32_t i = 0; i < impl_->limits.records + impl_->limits.groups; ++i)
    stats.retainedOwners += impl_->tasks[i].execution.live() + impl_->tasks[i].outcome.live();
  return stats;
}
#ifdef KALANDRA_TASK_PROFILE
TaskProfile TaskCore::profile() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->profile;
}
#endif
void TaskCore::failAllocationAt(std::uint64_t attempt) noexcept {
  Impl::Guard guard(*impl_);
  impl_->attempts = 0;
  impl_->failAt = attempt;
}
Invocation &Task::context() noexcept { return TaskCore::Impl::execution(core_.tasks[slot_]); }
void *Task::frame() noexcept {
  auto &record = core_.tasks[slot_];
  return record.call ? record.call->frame : record.frame;
}
GroupHandle Task::openGroup(GroupHandle parent) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  if (!parent && record.call)
    parent = record.call->lexical;
  auto *persistent = TaskCore::Impl::persistentCall(record);
  auto *owner = TaskCore::Impl::persistentOwner(persistent);
  GroupHold *hold = nullptr;
  if (owner) {
    if (!core_.bindOwner(*owner)) {
      context().fault(3);
      return {};
    }
    auto retained = borrowedCall(*persistent).pin.retain();
    if (!retained) {
      context().fault(3);
      return {};
    }
    auto *memory = core_.allocate(sizeof(GroupHold));
    if (!memory) {
      context().fault(2);
      return {};
    }
    hold = new (memory) GroupHold;
    guard.lock.unlock();
    hold->ownership.bindRecursiveBudget(&core_, TaskCore::Impl::reserveRecursiveBytes,
                                        TaskCore::Impl::releaseRecursiveBytes);
    const auto copied = hold->ownership.retainLease(*retained);
    core_.acquire(guard.lock);
    if (!copied) {
      context().fault(hold->ownership.status() ? hold->ownership.status() : 2);
      guard.lock.unlock();
      hold->~GroupHold();
      core_.acquire(guard.lock);
      core_.freeBytes(memory, sizeof(GroupHold));
      return {};
    }
    if (parent && core_.get(parent) && core_.get(parent)->persistentOwner != owner)
      parent = {};
  }
  auto group = core_.open(core_.handle(slot_), parent, owner != nullptr);
  if (group) {
    auto *created = core_.get(group);
    created->callOwner = record.call;
    created->persistentOwner = owner;
    if (owner)
      ++owner->groupCount_;
    created->ownerHold = hold;
  } else if (hold) {
    guard.lock.unlock();
    hold->~GroupHold();
    core_.acquire(guard.lock);
    void *memory = hold;
    core_.freeBytes(memory, sizeof(GroupHold));
  }
  return group;
}
TaskHandle Task::spawn(GroupHandle group, const TaskEntry &entry, Packet packet) noexcept {
  TaskCore::Impl::Guard guard(core_);
  return core_.submit(group, entry, packet, core_.handle(slot_), guard.lock);
}
bool Task::call(const TaskEntry &entry, Packet packet, void *output, GroupHandle lexical,
                bool tail) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  if (record.state != State::Running || record.callPushed || record.callCompleted ||
      !entry.result || !validAlignment(entry.result->alignment) ||
      !aligned(output, entry.result->alignment)) {
    context().fault(3);
    return false;
  }
  if (record.returned) {
    auto *node = record.returned;
    if (node->borrowed || node->returnEntry != &entry || !node->valueInitialized) {
      context().fault(3);
      return false;
    }
    guard.lock.unlock();
    const auto copied = entry.result->copy(context(), output, node->outcome, node->value);
    core_.acquire(guard.lock);
    record.returned = nullptr;
    core_.destroyCall(node, guard.lock);
    if (!copied && !context().poll())
      context().fault(3);
    return copied;
  }
  if (record.cancel.load())
    return false;
  if (entry.revision != 1 || !entry.construct || !entry.destroy || !entry.resume ||
      !entry.result->copy || !entry.result->destroy || !entry.result->identity ||
      !entry.frameBytes || !entry.result->bytes || !validAlignment(entry.frameAlignment) ||
      !validAlignment(entry.result->alignment) || !validAlignment(entry.packetAlignment) ||
      packet.layout != entry.packetLayout || packet.bytes != entry.packetBytes ||
      (packet.bytes && !aligned(packet.data, entry.packetAlignment)) ||
      (lexical && !core_.owned(lexical, core_.handle(slot_)))) {
    context().fault(3);
    return false;
  }
  // Reserve one aligned allocation for descriptor, frame, and return storage.
  constexpr auto alignment = alignof(std::max_align_t);
  auto padded = [](std::size_t n) { return (n + alignment - 1) & ~(alignment - 1); };
  if (entry.frameBytes > core_.limits.callBytes || entry.result->bytes > core_.limits.callBytes) {
    context().fault(2);
    return false;
  }
  const auto headerBytes = padded(sizeof(CallFrame));
  if (entry.frameBytes > SIZE_MAX - (alignment - 1) || headerBytes > core_.limits.callBytes ||
      padded(entry.frameBytes) > core_.limits.callBytes - headerBytes ||
      entry.result->bytes > core_.limits.callBytes - headerBytes - padded(entry.frameBytes)) {
    context().fault(2);
    return false;
  }
  const auto bytes = headerBytes + padded(entry.frameBytes) + entry.result->bytes;
  if (++core_.attempts == core_.failAt || !core_.chargeCallBytes(bytes)) {
    context().fault(2);
    return false;
  }
  auto *memory = std::malloc(bytes);
  if (!memory) {
    core_.stats.callBytes -= bytes;
    context().fault(2);
    return false;
  }
  auto *node = new (memory) CallFrame;
  node->parent = record.call;
  node->entry = node->returnEntry = &entry;
  node->bytes = bytes;
  node->lexical = lexical ? lexical : record.call ? record.call->lexical : GroupHandle{};
  node->frame = static_cast<char *>(memory) + padded(sizeof(CallFrame));
  node->value = static_cast<char *>(node->frame) + padded(entry.frameBytes);
  for (auto *invocation : {&node->execution, &node->outcome}) {
    invocation->bindCancellation(&record.cancel);
    invocation->bindRecursiveBudget(&core_, TaskCore::Impl::reserveCallBytes,
                                    TaskCore::Impl::releaseCallBytes);
    invocation->bindEnvironmentBudget(&core_, TaskCore::Impl::reserveCallBytes,
                                      TaskCore::Impl::releaseCallBytes);
  }
  ++core_.stats.callFrames;
  core_.stats.peakCallFrames = std::max(core_.stats.peakCallFrames, core_.stats.callFrames);
  guard.lock.unlock();
  try {
    node->initialized = entry.construct(node->execution, node->frame, packet);
  } catch (...) {
    node->execution.fault(3);
  }
  core_.acquire(guard.lock);
  if (!node->initialized || node->execution.status()) {
    if (node->execution.status())
      context().fault(node->execution.status(), node->execution.line(), node->execution.column());
    else if (!record.cancel.load())
      context().fault(3);
    core_.destroyCall(node, guard.lock);
    return false;
  }
  record.callTail =
      tail && record.call && !record.call->borrowed && record.call->entry->result == entry.result;
  if (record.callTail)
    for (auto i = core_.liveGroups.next(0); i < core_.liveGroups.size();
         i = core_.liveGroups.next(i + 1))
      if (core_.groups[i].owner == core_.handle(slot_) && core_.groups[i].callOwner == record.call)
        record.callTail = false;
  record.returned = node;
  record.callPushed = true;
  record.state = State::Runnable;
  return false;
}
bool Task::callBorrowed(const BorrowedTaskEntry &borrowed, AllocationLease pin, void *output,
                        GroupHandle lexical) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  const auto *entry = borrowed.entry;
  if (record.state != State::Running || record.callPushed || record.callCompleted || !entry ||
      !entry->result || !validAlignment(entry->result->alignment) ||
      !aligned(output, entry->result->alignment)) {
    context().fault(3);
    return false;
  }
  if (record.returned) {
    auto *node = record.returned;
    if (!node->borrowed || borrowedCall(*node).entry != &borrowed || !node->valueInitialized) {
      context().fault(3);
      return false;
    }
    guard.lock.unlock();
    const auto copied = entry->result->copy(context(), output, node->outcome, node->value);
    core_.acquire(guard.lock);
    record.returned = nullptr;
    core_.destroyCall(node, guard.lock);
    if (!copied && !context().poll())
      context().fault(3);
    return copied;
  }
  if (record.cancel.load())
    return false;
  if (!pin || entry->revision != 1 || !entry->resume || !borrowed.execution ||
      borrowed.execution == &context() || !borrowed.enter || !borrowed.leave ||
      !validAlignment(entry->frameAlignment) || !aligned(borrowed.frame, entry->frameAlignment) ||
      !entry->result->copy || !entry->result->destroy || !entry->result->identity ||
      !entry->result->bytes || (lexical && !core_.owned(lexical, core_.handle(slot_)))) {
    context().fault(3);
    return false;
  }
  if (borrowed.groups) {
    const auto storage = pin.bytes();
    const auto base = reinterpret_cast<std::uintptr_t>(storage.data());
    const auto address = reinterpret_cast<std::uintptr_t>(borrowed.groups);
    if (address < base || storage.size() < sizeof(BorrowedGroupOwner) ||
        address - base > storage.size() - sizeof(BorrowedGroupOwner) ||
        borrowed.groups->storage_ != borrowed.execution ||
        borrowed.groups->closing_.load(std::memory_order_acquire)) {
      context().fault(3);
      return false;
    }
  }
  constexpr auto header = callPadded(sizeof(CallFrame));
  constexpr auto leaseBytes = callPadded(sizeof(BorrowedCall));
  if (header > core_.limits.callBytes || leaseBytes > core_.limits.callBytes - header ||
      entry->result->bytes > core_.limits.callBytes - header - leaseBytes) {
    context().fault(2);
    return false;
  }
  const auto bytes = header + leaseBytes + entry->result->bytes;
  if (++core_.attempts == core_.failAt || !core_.chargeCallBytes(bytes)) {
    context().fault(2);
    return false;
  }
  auto *memory = std::malloc(bytes);
  if (!memory) {
    core_.stats.callBytes -= bytes;
    context().fault(2);
    return false;
  }
  auto *node = new (memory) CallFrame;
  node->parent = record.call;
  node->entry = node->returnEntry = entry;
  node->bytes = bytes;
  node->lexical = lexical ? lexical : record.call ? record.call->lexical : GroupHandle{};
  node->frame = borrowed.frame;
  node->value = static_cast<char *>(memory) + header + leaseBytes;
  node->borrowed = true;
  new (&borrowedCall(*node)) BorrowedCall{&borrowed, std::move(pin), record.persistent};
  node->outcome.bindCancellation(&record.cancel);
  node->outcome.bindRecursiveBudget(&core_, TaskCore::Impl::reserveCallBytes,
                                    TaskCore::Impl::releaseCallBytes);
  node->outcome.bindEnvironmentBudget(&core_, TaskCore::Impl::reserveCallBytes,
                                      TaskCore::Impl::releaseCallBytes);
  ++core_.stats.callFrames;
  core_.stats.peakCallFrames = std::max(core_.stats.peakCallFrames, core_.stats.callFrames);
  guard.lock.unlock();
  node->initialized = borrowed.enter(borrowed.state, context());
  core_.acquire(guard.lock);
  if (!node->initialized) {
    // Failed acquisition may mean another task owns this execution context.
    // Do not inspect it before acquiring exclusive access.
    if (!record.cancel.load() && !context().poll())
      context().fault(3);
    core_.destroyCall(node, guard.lock);
    return false;
  }
  if (auto *owner = borrowed.groups) {
    if (!core_.bindOwner(*owner)) {
      context().fault(3);
      core_.destroyCall(node, guard.lock);
      return false;
    }
    core_.attachGroups(*node, core_.handle(slot_));
  }
  if (borrowed.execution->poll() || !borrowed.execution->usable() || context().poll()) {
    if (borrowed.execution->status())
      context().fault(borrowed.execution->status(), borrowed.execution->line(),
                      borrowed.execution->column());
    else if (!record.cancel.load() && !context().poll())
      context().fault(3);
    // Preserve the borrowed node until the caller's failure barrier has
    // cancelled and joined its reattached groups. No producer body executes.
    record.returned = node;
    return false;
  }
  record.returned = node;
  record.callPushed = true;
  record.callTail = false;
  record.state = State::Runnable;
  return false;
}
bool BorrowedGroupOwner::enqueueCleanupChild(BorrowedGroupOwner &child, AllocationLease pin,
                                             BorrowedCleanup callbacks) noexcept {
  auto *core = static_cast<TaskCore::Impl *>(cleanupCore_);
  if (!core)
    return false;
  TaskCore::Impl::Guard guard(*core);
  if (!cleanup_.busy || cleanup_.phase != CleanupPhase::Children)
    return false;
  return core->queueCleanup(child, pin, callbacks, *cleanup_.root, this);
}
bool BorrowedGroupOwner::enqueueCleanupSibling(BorrowedGroupOwner &sibling, AllocationLease pin,
                                               BorrowedCleanup callbacks) noexcept {
  auto *core = static_cast<TaskCore::Impl *>(cleanupCore_);
  if (!core)
    return false;
  TaskCore::Impl::Guard guard(*core);
  auto &root = *cleanup_.root;
  if (root.cleanup_.sealed)
    return false;
  return core->queueCleanup(sibling, pin, callbacks, root, nullptr);
}
bool BorrowedGroupOwner::enqueueOwnedScopeChildren(Invocation &storage) noexcept {
  auto *core = static_cast<TaskCore::Impl *>(cleanupCore_);
  if (!core)
    return false;
  TaskCore::Impl::Guard guard(*core);
  if (!cleanup_.busy || cleanup_.phase != CleanupPhase::Children || storage_ != &storage)
    return false;
  std::lock_guard resources(resourceMutex());
  // Validate every scope before enqueueing anything. Headers have strong scope
  // owners, so validating and later acquiring the queue pins cannot race a last
  // value release. No allocation is required even when memory admission failed.
  std::uint64_t count = 0;
  for (auto *control = currentScope(storage); control;) {
    auto *scope = control->scope;
    if (!scope || scope->context != &storage || !scope->active)
      return false;
    for (auto *value = scope->first; value; value = value->next) {
      auto *owner = value->deferredOwner;
      if (!owner || owner->cleanupFinished())
        continue;
      auto pin = value->pinForRetirement();
      if (owner == this || !pin || !value->deferredCleanup || !value->deferredCleanup->finish ||
          !TaskCore::Impl::within(*pin, owner, sizeof(*owner)) ||
          owner->cleanup_.phase != CleanupPhase::Idle ||
          core->activeOwner(*owner, cleanup_.root->cleanup_.waiter.slot) ||
          !core->bindOwner(*owner) || count == UINT64_MAX - core->cleanupJobs ||
          count == UINT64_MAX - cleanup_.children ||
          count == UINT64_MAX - cleanup_.root->cleanup_.pending)
        return false;
      ++count;
    }
    control = scope->parent ? scope->parent->control : nullptr;
  }
  for (auto *control = currentScope(storage); control;) {
    auto *scope = control->scope;
    scope->retiring = true;
    for (auto *value = scope->first; value; value = value->next) {
      auto *owner = value->deferredOwner;
      if (!owner || owner->cleanupFinished())
        continue;
      auto pin = value->pinForRetirement();
      if (!core->queueCleanup(*owner, *pin, *value->deferredCleanup, *cleanup_.root, this))
        std::abort();
    }
    control = scope->parent ? scope->parent->control : nullptr;
  }
  return true;
}
void BorrowedGroupOwner::sealCleanup() noexcept {
  auto *core = static_cast<TaskCore::Impl *>(cleanupCore_);
  if (!core)
    return;
  TaskCore::Impl::Guard guard(*core);
  core->sealCleanup(*cleanup_.root, guard.lock);
}
CleanupStart Task::beginOwnedCleanup(BorrowedGroupOwner &owner, AllocationLease pin,
                                     BorrowedCleanup callbacks, bool seal) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  if (record.state != State::Running || record.externalPending || record.cleanupPending || !pin ||
      !TaskCore::Impl::within(pin, &owner, sizeof(owner)) || !callbacks.finish) {
    context().fault(3);
    return CleanupStart::Rejected;
  }
  if (owner.cleanupFinished())
    return CleanupStart::Complete;
  // A source-level scope cannot retire an independently active mutable cursor.
  // Active terminal/failure finalization will be handled by the native bridge.
  if (core_.activeOwner(owner)) {
    context().fault(3);
    return CleanupStart::Rejected;
  }
  auto &fence = owner.cleanup_;
  if (fence.phase != BorrowedGroupOwner::CleanupPhase::Idle) {
    context().fault(3);
    return CleanupStart::Rejected;
  }
  fence.pending = 1; // batch admission sentinel
  fence.waiter = core_.handle(slot_);
  if (!core_.queueCleanup(owner, pin, callbacks, owner, nullptr)) {
    fence.pending = 0;
    context().fault(3);
    return CleanupStart::Rejected;
  }
  record.cleanupPending = true;
  record.cleanupFailure = {};
  record.state = State::WaitingOwned;
  ++core_.stats.maintenanceTasks;
  if (seal)
    core_.sealCleanup(owner, guard.lock);
  return CleanupStart::Pending;
}
Failure Task::ownedCleanupFailure() const noexcept {
  std::lock_guard lock(core_.mutex);
  return core_.tasks[slot_].cleanupFailure;
}
CleanupStart Task::prepareOwnedScope(ResourceControl &control) noexcept {
  TaskCore::Impl::Guard guard(core_);
  return core_.prepareScope(slot_, context(), control, false, guard.lock);
}
bool Task::leaveOwnedScope(ResourceControl *control, const OwnedDescriptor *escaping) noexcept {
  if (!control)
    return true;
  auto &ctx = context();
  if (!control->scope || control->scope->context != &ctx) {
    ctx.fault(3);
    return true;
  }
  if (!control->scope->escapePrepared) {
    if (escaping && !ctx.poll() && ctx.usable()) {
      auto *value = inspectOwned(ctx, *escaping);
      if (!value)
        return true;
      adopt(ctx, *value, control->scope->parent);
      if (ctx.status())
        return true;
    }
    control->scope->escapePrepared = true;
  }
  const auto started = prepareOwnedScope(*control);
  if (started == CleanupStart::Pending)
    return false;
  if (started == CleanupStart::Rejected)
    return true; // fault finalization retains/preflights the same control
  const auto failure = ownedCleanupFailure();
  if (failure.kind == TaskFailure::Fault)
    ctx.fault(failure.code, failure.line, failure.column);
  ctx.releaseControl(control);
  return true;
}

bool Task::settleBorrowedGroups() noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  auto *owner = TaskCore::Impl::persistentOwner(TaskCore::Impl::persistentCall(record));
  if (record.state != State::Running || !owner) {
    context().fault(3);
    return false;
  }
  GroupHandle waiting;
  for (auto i = core_.liveGroups.next(0); i < core_.liveGroups.size();
       i = core_.liveGroups.next(i + 1)) {
    auto &group = core_.groups[i];
    if (group.persistentOwner != owner)
      continue;
    if (group.owner != core_.handle(slot_) || group.detached) {
      context().fault(3);
      return false;
    }
    if (!group.settled) {
      core_.failGroup(core_.groupHandle(i), {TaskFailure::Cancelled});
      waiting = core_.groupHandle(i);
    }
  }
  if (waiting) {
    record.groupDependency = waiting;
    record.state = State::WaitingGroup;
    ++core_.stats.maintenanceTasks;
    return false;
  }
  Failure failure;
  for (auto i = core_.liveGroups.next(0); i < core_.liveGroups.size();
       i = core_.liveGroups.next(i + 1)) {
    auto &group = core_.groups[i];
    if (group.persistentOwner != owner)
      continue;
    if (failure.kind == TaskFailure::None && group.failure.kind == TaskFailure::Fault)
      failure = group.failure;
    group.observed = true;
    core_.dispose(core_.groupHandle(i), guard.lock);
  }
  if (failure.kind != TaskFailure::None) {
    context().fault(failure.code, failure.line, failure.column);
    return false;
  }
  return true;
}
bool Task::await(TaskHandle h) noexcept {
  TaskCore::Impl::Guard guard(core_);
  if (core_.tasks[slot_].state != State::Running) {
    context().fault(3);
    return false;
  }
  auto *dependency = core_.get(h);
  if (!dependency || !core_.owned(dependency->group, core_.handle(slot_))) {
    context().fault(3);
    return false;
  }
  if (dependency->state == State::Settled) {
    dependency->observed = true;
    return true;
  }
  auto &task = core_.tasks[slot_];
  task.dependency = h;
  task.state = State::WaitingTask;
  ++core_.stats.maintenanceTasks;
  return false;
}
bool Task::join(GroupHandle h) noexcept {
  TaskCore::Impl::Guard guard(core_);
  if (core_.tasks[slot_].state != State::Running) {
    context().fault(3);
    return false;
  }
  auto *group = core_.get(h);
  if (!group || group->owner != core_.handle(slot_) || !group->closed) {
    context().fault(3);
    return false;
  }
  if (group->settled) {
    group->observed = true;
    return true;
  }
  auto &task = core_.tasks[slot_];
  task.groupDependency = h;
  task.state = State::WaitingGroup;
  ++core_.stats.maintenanceTasks;
  return false;
}
bool Task::releaseGroup(GroupHandle h) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto *group = core_.get(h);
  if (core_.tasks[slot_].state != State::Running || !group || group->owner != core_.handle(slot_) ||
      !group->closed || !group->settled || !group->observed)
    return false;
  return core_.dispose(h, guard.lock);
}
bool Task::read(TaskHandle h, const ValueLayout &layout, void *output) noexcept {
  TaskCore::Impl::Guard guard(core_);
  return core_.read(h, layout, context(), output, core_.handle(slot_), guard.lock);
}
Failure Task::failure(GroupHandle h) const noexcept {
  std::lock_guard lock(core_.mutex);
  auto *group = core_.get(h);
  return group && group->owner == core_.handle(slot_) ? group->failure
                                                      : Failure{TaskFailure::Fault, 3};
}
Failure Task::failure(TaskHandle h) const noexcept {
  std::lock_guard lock(core_.mutex);
  auto *task = core_.get(h);
  return task && core_.owned(task->group, core_.handle(slot_)) ? task->failure
                                                               : Failure{TaskFailure::Fault, 3};
}
bool Task::close(GroupHandle group) noexcept {
  TaskCore::Impl::Guard guard(core_);
  return core_.close(group, core_.handle(slot_));
}
bool Task::cancel(GroupHandle group, bool deadline) noexcept {
  TaskCore::Impl::Guard guard(core_);
  if (!core_.owned(group, core_.handle(slot_)))
    return false;
  core_.failGroup(group, {deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled});
  return true;
}
bool Task::failGroup(GroupHandle group, Failure failure) noexcept {
  TaskCore::Impl::Guard guard(core_);
  if (!core_.owned(group, core_.handle(slot_)) || failure.kind == TaskFailure::None)
    return false;
  core_.failGroup(group, failure);
  return true;
}
bool Task::stopGroup(GroupHandle group) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto *record = core_.get(group);
  if (!record || record->owner != core_.handle(slot_) || record->settled ||
      record->failure.kind != TaskFailure::None)
    return false;
  // This cancellation marker is consumed by the owning frame, which keeps the
  // original typed application outcome across suspension and group settlement.
  core_.failGroup(group, {TaskFailure::Cancelled});
  return true;
}
bool Task::cancelled() const noexcept {
  return core_.tasks[slot_].cancel.load(std::memory_order_acquire);
}
bool Task::canExecuteSynchronously() const noexcept {
  if (!core_.closedCaller)
    return false;
  std::lock_guard lock(core_.mutex);
  auto &self = core_.tasks[slot_];
  if (!self.executing || self.state != State::Running ||
      self.runner != std::this_thread::get_id() || self.cancel.load() ||
      TaskCore::Impl::execution(self).executionStatus() || core_.running != 1 || core_.preparing ||
      core_.stats.externalPending || core_.stats.timers || core_.cleanupJobs || core_.stopping)
    return false;
  for (auto i = core_.liveTasks.next(0); i < core_.liveTasks.size();
       i = core_.liveTasks.next(i + 1)) {
    if (i == slot_)
      continue;
    const auto &other = core_.tasks[i];
    if (other.executing || other.cleaning || other.readers || other.externalPending)
      return false;
    if (other.state != State::WaitingTask && other.state != State::WaitingGroup &&
        other.state != State::Settled)
      return false;
  }
  // The owner exposes no host capability, pending timer or other source that
  // could require independent progress while this callback runs.
  return true;
}
ExternalCompletion::ExternalCompletion(const std::shared_ptr<TaskCore::Impl> &core,
                                       TaskHandle handle, std::uint64_t epoch) noexcept
    : core_(core), handle_(handle), epoch_(epoch) {}
bool ExternalCompletion::cancelled() const noexcept {
  auto core = core_.lock();
  if (!core)
    return true;
  TaskCore::Impl::Guard guard(*core);
  auto *record = core->get(handle_);
  return !record || !record->externalPending || record->externalEpoch != epoch_ ||
         record->cancel.load(std::memory_order_acquire);
}
bool ExternalCompletion::signal() const noexcept {
  auto core = core_.lock();
  if (!core)
    return false;
  TaskCore::Impl::Guard guard(*core);
  auto *record = core->get(handle_);
  if (!record || !record->externalPending || record->externalEpoch != epoch_)
    return false;
  record->externalPending = false;
  --core->stats.externalPending;
  core->wake(handle_.slot);
  return true;
}
ExternalCompletion Task::suspendExternal() noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  if (record.state != State::Running || record.externalPending ||
      record.externalEpoch == UINT64_MAX) {
    context().fault(3);
    return {};
  }
  // This reservation is the external submission decision. A cancellation
  // already recorded wins; after reservation, completion must acknowledge
  // that external access has ended even if cancellation arrives immediately.
  if (record.cancel.load()) {
    core_.finish(slot_, {TaskFailure::Cancelled});
    return {};
  }
  record.externalPending = true;
  record.state = State::WaitingExternal;
  ++record.externalEpoch;
  ++core_.stats.externalPending;
  ++core_.stats.maintenanceTasks;
  return ExternalCompletion(core_.shared_from_this(), core_.handle(slot_), record.externalEpoch);
}
void Task::suspendUntil(std::chrono::steady_clock::time_point deadline) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  if (record.state != State::Running) {
    context().fault(3);
    return;
  }
  if (record.cancel.load()) {
    core_.finish(slot_, {TaskFailure::Cancelled});
    return;
  }
  if (deadline <= std::chrono::steady_clock::now()) {
    record.state = State::Runnable;
    return;
  }
  record.wakeAt = deadline;
  record.state = State::WaitingTimer;
  ++core_.stats.timers;
  ++core_.stats.maintenanceTasks;
}
void Task::yield() noexcept {
  TaskCore::Impl::Guard guard(core_);
  if (core_.tasks[slot_].state != State::Running)
    context().fault(3);
  else
    core_.tasks[slot_].state = State::Runnable;
}
void Task::complete(const void *value) noexcept { completeFrom(context(), value); }
void Task::completeFrom(const Invocation &source, const void *value) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &task = core_.tasks[slot_];
  if (context().status() || task.state != State::Running) {
    context().fault(3);
    core_.finish(slot_, invocationFailure(context()));
    return;
  }
  if (task.cancel.load()) {
    core_.finish(slot_, {TaskFailure::Cancelled});
    return;
  }
  auto *node = task.call;
  if (node && TaskCore::Impl::persistentOwner(node))
    core_.detachGroups(*node, core_.handle(slot_));
  if (node)
    for (auto i = core_.liveGroups.next(0); i < core_.liveGroups.size();
         i = core_.liveGroups.next(i + 1))
      if (core_.groups[i].owner == core_.handle(slot_) && core_.groups[i].callOwner == node) {
        context().fault(3);
        core_.finish(slot_, invocationFailure(context()));
        return;
      }
  const auto &layout = *(node ? node->entry : task.entry)->result;
  auto &outcome = node ? node->outcome : task.outcome;
  auto *output = node ? node->value : task.value;
  bool copied = false;
  if (aligned(value, layout.alignment)) {
    guard.lock.unlock();
    copied = layout.copy(outcome, output, source, value);
    core_.acquire(guard.lock);
  }
  if (!copied) {
    if (task.cancel.load() && !outcome.status()) {
      core_.finish(slot_, {TaskFailure::Cancelled});
      return;
    }
    context().fault(outcome.status() ? outcome.status() : 3);
    core_.finish(slot_, invocationFailure(context()));
    return;
  }
  if (node) {
    node->valueInitialized = true;
    task.callCompleted = true;
    task.state = State::Runnable;
  } else {
    task.valueInitialized = true;
    core_.finish(slot_, task.cancel.load() ? Failure{TaskFailure::Cancelled} : Failure{});
  }
}
void Task::fail(Failure failure) noexcept {
  TaskCore::Impl::Guard guard(core_);
  auto &record = core_.tasks[slot_];
  if (record.state != State::Running || failure.kind < TaskFailure::Fault ||
      failure.kind > TaskFailure::AdmissionRejected)
    context().fault(3);
  if (context().status())
    failure = invocationFailure(context());
  else if (record.cancel.load())
    failure = {TaskFailure::Cancelled};
  core_.finish(slot_, failure);
}
void Task::fault(std::int64_t code, std::int64_t line, std::int64_t column) noexcept {
  TaskCore::Impl::Guard guard(core_);
  context().fault(code, line, column);
  core_.finish(slot_, invocationFailure(context()));
}
} // namespace kalandra::runtime
