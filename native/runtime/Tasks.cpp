#include "Tasks.h"
#include "NativeCancellation.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <chrono>
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
  Settling,
  Settled
};
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
  std::uint64_t externalEpoch = 0;
  std::atomic<bool> cancel{false};
  std::thread::id runner{}, lastRunner{};
  std::uintptr_t interruptThread = 0;
  Invocation execution, outcome;
  Failure failure;
  TaskHandle dependency;
  GroupHandle groupDependency;
};
struct Group {
  std::uint64_t generation = 1;
  bool used = false, closed = false, settled = false, observed = false, disposing = false;
  TaskHandle owner;
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
  std::unique_ptr<Record[]> tasks;
  std::unique_ptr<Group[]> groups;
  std::unique_ptr<TaskHandle[]> queue;
  Slots liveTasks, unfinishedTasks, liveGroups;
  std::uint32_t queueHead = 0, queueCount = 0;
  std::uint32_t pendingHead = UINT32_MAX, pendingTail = UINT32_MAX, pendingCount = 0;
  std::uint64_t attempts = 0, failAt = 0;
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
    if (interruptWorker.joinable()) interruptWorker.join();
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
            if (stopping) break;
            // Execution leases and pthread identities remain protected by the
            // core mutex. Late delivery reads the CURRENT boundary/token, never
            // a recycled task slot or a pointer supplied by the cancelling host.
            for (auto i = unfinishedTasks.next(0); i < unfinishedTasks.size();
                 i = unfinishedTasks.next(i + 1))
              if (tasks[i].executing && tasks[i].cancel.load())
                interruptNativeCancellation(tasks[i].interruptThread);
            interruptChanged.wait_for(lock, std::chrono::microseconds(100), [&] { return stopping; });
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
            if (!remaining || !stats.unfinished) {
              draining = false;
              signal();
              continue;
            }
            if ((--remaining, step(lock))) {
              // The step's Pump signals after its lease has fully unwound.
              continue;
            }
            ++remaining;
            if (!pumping && !preparing && !stats.externalPending) {
              draining = false;
              signal();
              continue;
            }
            const auto seen = revision;
            wait(lock, workerChanged, [&] { return stopping || !draining || revision != seen; });
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
      if (record.state != State::Runnable || record.executing || record.queued)
        continue;
      pushReady(index);
      TASK_PROBE(pendingPromotions, 1);
    }
  }
  void enqueue(std::uint32_t index, bool admission = false) {
    auto &record = tasks[index];
    if (record.queued || record.pending || record.state != State::Runnable || record.executing)
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
        record.state == State::WaitingExternal) {
      record.state = State::Runnable;
      --stats.maintenanceTasks;
      record.dependency = {};
      record.groupDependency = {};
    }
    enqueue(index);
  }
  GroupHandle open(TaskHandle owner, GroupHandle lexical = {}) {
    std::uint32_t depth = 0;
    GroupHandle parent;
    if (owner) {
      auto *task = get(owner);
      if (!task || task->state != State::Running)
        return {};
      parent = lexical ? lexical : task->group;
      if (!get(parent) || (lexical && get(parent)->owner != owner)) {
        task->execution.fault(3);
        return {};
      }
      depth = get(parent)->depth + 1;
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
      task->execution.fault(2);
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
            if (task.executing) interruptNativeCancellation(task.interruptThread);
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
    if (!group || group->owner != owner || group->settled)
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
      if (!task.initialized)
        task.execution.fault(3);
      acquire(lock);
      task.runner = {};
    }
    --preparing;
    if (task.execution.status()) {
      task.failure = invocationFailure(task.execution);
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
    return group && group->owner == owner;
  }
  bool close(GroupHandle h, TaskHandle owner) {
    auto *group = get(h);
    if (!group || group->owner != owner)
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
    const auto generation = group->generation + 1;
    liveGroups.erase(h.slot);
    *group = Group{};
    group->generation = generation;
    --stats.groups;
    return true;
  }
  void maintain(std::unique_lock<std::mutex> &lock) {
    bool changed;
    do {
      changed = false;
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
            task.externalPending)
          continue;
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
        task.cleaning = true;
        lock.unlock();
        if (task.failure.kind != TaskFailure::None) {
          if (task.valueInitialized) {
            task.entry->result->destroy(task.value);
            task.valueInitialized = false;
          }
          task.outcome.settle();
        }
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
    if (task.state != State::Settling && task.state != State::WaitingTask &&
        task.state != State::WaitingGroup && task.state != State::WaitingExternal)
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
    if (!queueCount)
      return false;
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
    if (record->executing || record->state != State::Runnable)
      return true;
    record->state = State::Running;
    if (record->cancel.load())
      finish(h.slot, {TaskFailure::Cancelled});
    else {
      record->executing = true;
      record->runner = std::this_thread::get_id();
      record->interruptThread = nativeInterrupts ? nativeCancellationThread() : 0;
      record->execution.allowNativeInterrupts(record->interruptThread != 0);
      if (record->lastRunner != std::thread::id{} && record->lastRunner != record->runner)
        ++stats.migrations;
      record->lastRunner = record->runner;
      ++running;
      stats.peakRunning = std::max(stats.peakRunning, running);
      ++stats.resumes;
      Task task(*this, h.slot);
      lock.unlock();
      try {
        record->entry->resume(task);
      } catch (...) {
        record->execution.fault(3);
      }
      acquire(lock);
      record->executing = false;
      record->runner = {};
      record->interruptThread = 0;
      --running;
      if (record->execution.status())
        finish(h.slot, invocationFailure(record->execution));
      else if (record->cancel.load() &&
               (record->state != State::Settling || record->failure.kind == TaskFailure::None))
        finish(h.slot, {TaskFailure::Cancelled});
      else if (record->state == State::Running)
        finish(h.slot, {TaskFailure::Fault, 3});
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
  if (!limits.workers || limits.workers > 64 || !limits.records || !limits.groups ||
      !limits.ready || !limits.inFlight ||
      std::uint64_t(limits.records) + limits.groups >= UINT32_MAX || !limits.bytes)
    return {};
  try {
    auto impl = std::make_shared<Impl>(limits);
    if (!impl->start())
      return {};
    return std::unique_ptr<TaskCore>(new TaskCore(std::move(impl)));
  } catch (...) {
    return {};
  }
}
TaskCore::TaskCore(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
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
  if (!impl_->stats.unfinished && !impl_->pumping && !impl_->preparing) {
    impl_->controlling = false;
    return true;
  }
  if (impl_->limits.workers == 1) {
    impl_->draining = true;
    while (steps) {
      if (impl_->step(guard.lock)) {
        --steps;
      } else if (impl_->stats.externalPending) {
        const auto seen = impl_->revision;
        impl_->externalControllerWaiting = true;
        impl_->wait(
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
  return !impl_->stats.unfinished;
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
Invocation &Task::context() noexcept { return core_.tasks[slot_].execution; }
void *Task::frame() noexcept { return core_.tasks[slot_].frame; }
GroupHandle Task::openGroup(GroupHandle parent) noexcept {
  TaskCore::Impl::Guard guard(core_);
  return core_.open(core_.handle(slot_), parent);
}
TaskHandle Task::spawn(GroupHandle group, const TaskEntry &entry, Packet packet) noexcept {
  TaskCore::Impl::Guard guard(core_);
  return core_.submit(group, entry, packet, core_.handle(slot_), guard.lock);
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
ExternalCompletion::ExternalCompletion(const std::shared_ptr<TaskCore::Impl> &core,
                                       TaskHandle handle, std::uint64_t epoch) noexcept
    : core_(core), handle_(handle), epoch_(epoch) {}
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
void Task::yield() noexcept {
  TaskCore::Impl::Guard guard(core_);
  if (core_.tasks[slot_].state != State::Running)
    context().fault(3);
  else
    core_.tasks[slot_].state = State::Runnable;
}
void Task::complete(const void *value) noexcept {
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
  const auto &layout = *task.entry->result;
  bool copied = false;
  if (aligned(value, layout.alignment)) {
    guard.lock.unlock();
    copied = layout.copy(task.outcome, task.value, context(), value);
    core_.acquire(guard.lock);
  }
  if (!copied) {
    if (task.cancel.load() && !task.outcome.status()) {
      core_.finish(slot_, {TaskFailure::Cancelled});
      return;
    }
    task.execution.fault(task.outcome.status() ? task.outcome.status() : 3);
    core_.finish(slot_, invocationFailure(task.execution));
    return;
  }
  task.valueInitialized = true;
  core_.finish(slot_, task.cancel.load() ? Failure{TaskFailure::Cancelled} : Failure{});
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
