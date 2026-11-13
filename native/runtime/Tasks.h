#pragma once
#include "Runtime.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace kalandra::runtime {
struct TaskHandle {
  std::uint32_t slot = UINT32_MAX;
  std::uint64_t generation = 0;
  explicit operator bool() const noexcept { return slot != UINT32_MAX; }
  bool operator==(const TaskHandle &) const = default;
};
struct GroupHandle {
  std::uint32_t slot = UINT32_MAX;
  std::uint64_t generation = 0;
  explicit operator bool() const noexcept { return slot != UINT32_MAX; }
  bool operator==(const GroupHandle &) const = default;
};
enum class TaskFailure { None, Fault, Cancelled, DeadlineExceeded, AdmissionRejected };
struct Failure {
  TaskFailure kind = TaskFailure::None;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Packet {
  std::uint64_t layout;
  const void *data;
  std::size_t bytes;
  Invocation *source = nullptr;
};
// Trusted compiler/runtime descriptors, not a public dynamic-value API.
// copy must retain dependencies before publishing the fully initialized value.
// A failed copy/construct must clean its partial C++ fields; Invocation owns
// and settles any managed backing dependencies acquired before failure.
struct ValueLayout {
  std::uint64_t identity;
  std::size_t bytes, alignment;
  bool (*copy)(Invocation &destination, void *output, const Invocation &source,
               const void *input) noexcept;
  void (*destroy)(void *) noexcept;
};
class Task;
class ExternalCompletion;
class ClosedTaskRunner;
struct ResourceControl;
struct OwnedDescriptor;
struct TaskEntry {
  std::uint32_t revision;
  std::uint64_t packetLayout;
  std::size_t packetBytes, packetAlignment, frameBytes, frameAlignment;
  bool (*construct)(Invocation &, void *frame, const Packet &);
  void (*destroy)(void *frame) noexcept;
  void (*resume)(Task &);
  const ValueLayout *result;
};
class BorrowedGroupOwner;
struct BorrowedCleanup {
  void *state = nullptr;
  void (*children)(BorrowedGroupOwner &, void *) noexcept = nullptr;
  void (*finish)(void *) noexcept = nullptr;
  Failure (*failure)(void *) noexcept = nullptr;
};
enum class CleanupStart { Complete, Pending, Rejected };
// Stable identity for groups belonging to a persistent producer frame. Only
// the borrowed frame's exclusive execution lease may bind or advance it.
// The enclosing backing must remain pinned until all its groups are disposed.
class BorrowedGroupOwner final {
public:
  explicit BorrowedGroupOwner(Invocation &storage) noexcept : storage_(&storage) {}
  BorrowedGroupOwner(const BorrowedGroupOwner &) = delete;
  BorrowedGroupOwner &operator=(const BorrowedGroupOwner &) = delete;
  // Trusted retirement callbacks only; they never execute producer source code.
  bool enqueueCleanupChild(BorrowedGroupOwner &, AllocationLease, BorrowedCleanup) noexcept;
  bool enqueueCleanupSibling(BorrowedGroupOwner &, AllocationLease, BorrowedCleanup) noexcept;
  // Called only by the trusted children callback, after this producer's own
  // groups have settled. Its body scopes stay alive until all children finish.
  bool enqueueOwnedScopeChildren(Invocation &) noexcept;
  void sealCleanup() noexcept;
  bool cleanupFinished() const noexcept { return cleanupDone_.load(std::memory_order_acquire); }

private:
  Invocation *storage_;
  // Published once under the exclusive producer lease. It belongs to the
  // control header, whose lifetime extends beyond settled producer locals.
  std::weak_ptr<void> core_;
  std::atomic<bool> bound_{false};
  enum class CleanupPhase { Idle, Groups, WaitingGroups, Children, Finish, Done };
  struct Cleanup {
    BorrowedGroupOwner *next = nullptr, *parent = nullptr, *root = nullptr;
    AllocationLease pin, anchor;
    BorrowedCleanup callbacks;
    TaskHandle waiter;
    std::uint64_t children = 0, pending = 0;
    Failure failure;
    CleanupPhase phase = CleanupPhase::Idle;
    bool queued = false, busy = false, sealed = false;
  } cleanup_;
  void *cleanupCore_ = nullptr; // present only while retirement is queued
  std::atomic<bool> cleanupDone_{false};
  std::atomic<bool> closing_{false};
  std::uint32_t groupCount_ = 0;
  friend class Task;
  friend class TaskCore;
};
// Private bridge for independently owned continuation storage. The descriptor,
// context and frame remain valid under `pin` until leave returns. enter acquires
// exclusive advancement and binds transient cancellation; failed enter must
// release any partial acquisition itself. leave releases a successful entry only
// after result publication or call unwinding. Neither callback may drive a core.
// With a group owner, completing the root call detaches its unfinished groups
// until a later advance/cleanup call. The source scope/drop cleanup bridge must
// settle those groups before releasing any retained data loans.
struct BorrowedTaskEntry {
  const TaskEntry *entry;
  Invocation *execution;
  void *frame, *state;
  bool (*enter)(void *state, Invocation &caller) noexcept;
  void (*leave)(void *state) noexcept;
  BorrowedGroupOwner *groups = nullptr;
};
struct TaskLimits {
  std::uint32_t workers = 1;
  std::uint32_t records = 128, groups = 32, ready = 32, inFlight = 64, depth = 16;
  std::size_t bytes = 1024 * 1024;
  // Ordinary resumable calls consume memory, never task/group admission slots.
  std::size_t callBytes = 64 * 1024 * 1024;
};
struct TaskStats {
  std::size_t retainedBytes = 0, peakBytes = 0, environmentBytes = 0, recursiveBytes = 0;
  std::size_t callBytes = 0, peakCallBytes = 0;
  std::uint64_t callFrames = 0, peakCallFrames = 0;
  std::uint64_t retainedOwners = 0;
  std::uint32_t unfinished = 0, records = 0, groups = 0;
  std::uint32_t ready = 0, pending = 0, maintenanceTasks = 0, externalPending = 0, timers = 0;
  std::uint64_t resumes = 0, publications = 0, migrations = 0;
  std::uint32_t peakRunning = 0;
};
#ifdef KALANDRA_TASK_PROFILE
// Private, opt-in diagnostics. Never enabled in performance timing builds.
struct TaskProfile {
  std::uint64_t locks = 0, contendedLocks = 0, lockWaitNs = 0;
  std::uint64_t notifications = 0, waits = 0, waitReturns = 0, waitNs = 0;
  std::uint64_t workerSignals = 0, workerBroadcasts = 0, controllerSignals = 0;
  std::uint64_t workerWaitReturns = 0, controllerWaitReturns = 0;
  std::uint64_t allocationAttempts = 0, frees = 0, allocatorNs = 0;
  std::uint64_t maintenancePasses = 0, recordVisits = 0, groupVisits = 0;
  std::uint64_t taskPasses = 0, idleTaskPasses = 0;
  std::uint64_t readyVisits = 0, steps = 0;
  std::uint64_t runnableEvents = 0, readyPushes = 0, pendingPromotions = 0, peakPending = 0;
};
#endif
// Host controls are synchronized; drain has one controller. Destruction is not
// concurrent with host calls. Entry callbacks may not recursively drive/destroy
// their core. Invocation and frame access belong to the active execution lease.
class TaskCore final {
public:
  static std::unique_ptr<TaskCore> create(TaskLimits limits = {}) noexcept;
  ~TaskCore();
  TaskCore(const TaskCore &) = delete;
  TaskCore &operator=(const TaskCore &) = delete;
  GroupHandle openRoot() noexcept;
  TaskHandle submit(GroupHandle, const TaskEntry &, Packet) noexcept;
  bool close(GroupHandle) noexcept;
  bool cancel(GroupHandle, bool deadline = false) noexcept;
  bool settled(GroupHandle) const noexcept;
  Failure failure(GroupHandle) const noexcept;
  bool settled(TaskHandle) const noexcept;
  Failure failure(TaskHandle) const noexcept;
  bool observe(TaskHandle, const ValueLayout &, Invocation &, void *output) noexcept;
  bool release(GroupHandle) noexcept;
  bool step() noexcept;
  bool drain(std::size_t steps = 100000) noexcept;
  // Finish productive scheduling batches; a controller slice is not a source
  // execution limit. Return false only if no progress source remains.
  bool drainToCompletion() noexcept;
  TaskStats stats() const noexcept;
#ifdef KALANDRA_TASK_PROFILE
  // Cumulative snapshot; sampling itself is not counted.
  TaskProfile profile() const noexcept;
#endif
  // Deterministic admission allocation failure, separate from invocation buffers.
  void failAllocationAt(std::uint64_t attempt) noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  explicit TaskCore(std::shared_ptr<Impl>) noexcept;
  static std::unique_ptr<TaskCore> create(TaskLimits, bool closedCaller) noexcept;
  friend class ExternalCompletion;
  friend class Task;
  friend class ClosedTaskRunner;
  friend class BorrowedGroupOwner;
};
// Ordinary synchronous entry. The scheduler and its cancellation capabilities
// never escape this owner. Concurrent run calls are serialized; stats is read-only.
// Source parallelism still uses the normal scheduler. A runtime leaf may omit
// asynchronous checkpoints only after Task::canExecuteSynchronously() proves
// that doing so cannot suppress independent progress in this private core.
class ClosedTaskRunner final {
public:
  static std::unique_ptr<ClosedTaskRunner> create(TaskLimits limits = {}) noexcept;
  ~ClosedTaskRunner();
  ClosedTaskRunner(const ClosedTaskRunner &) = delete;
  ClosedTaskRunner &operator=(const ClosedTaskRunner &) = delete;
  Failure run(const TaskEntry &, Packet, const ValueLayout &, Invocation &, void *output) noexcept;
  TaskStats stats() const noexcept;
  void failAllocationAt(std::uint64_t attempt) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit ClosedTaskRunner(std::unique_ptr<Impl>) noexcept;
};
// A one-shot completion capability. Copies are safe on foreign callback threads.
// Signal after external access ends, including on errors. The owning frame stays
// pinned until signal; cancellation cannot reclaim it early. Stale/duplicate
// tokens are rejected. Signalling queues progress, never executes source code.
class ExternalCompletion final {
public:
  ExternalCompletion() = default;
  bool signal() const noexcept;
  // Read-only cancellation probe for a still-leased external operation.
  // An expired or stale capability is stopped as well.
  bool cancelled() const noexcept;
  explicit operator bool() const noexcept { return bool(handle_); }

private:
  std::weak_ptr<TaskCore::Impl> core_;
  TaskHandle handle_;
  std::uint64_t epoch_ = 0;
  ExternalCompletion(const std::shared_ptr<TaskCore::Impl> &, TaskHandle, std::uint64_t) noexcept;
  friend class Task;
};
class Task final {
public:
  Invocation &context() noexcept;
  void *frame() noexcept;
  GroupHandle openGroup(GroupHandle parent = {}) noexcept;
  TaskHandle spawn(GroupHandle, const TaskEntry &, Packet) noexcept;
  // Retry the same call after suspension. A successful retry copies its result
  // into the caller's active ownership scope. No native recursion is involved.
  bool call(const TaskEntry &, Packet, void *output, GroupHandle lexical = {},
            bool tail = false) noexcept;
  // As with call, retry after suspension. Does not construct, destroy or settle
  // the borrowed execution storage, and never replaces it via a tail call.
  bool callBorrowed(const BorrowedTaskEntry &, AllocationLease pin, void *output,
                    GroupHandle lexical = {}) noexcept;
  // Private cleanup continuation. Cancels and joins the active borrowed
  // producer's groups without executing its suspended body. Retry after wait.
  bool settleBorrowedGroups() noexcept;
  // Begin a trusted cleanup batch without a scheduled task per producer.
  // For a batch with siblings, pass seal=false, enqueue them, then sealCleanup.
  CleanupStart beginOwnedCleanup(BorrowedGroupOwner &, AllocationLease, BorrowedCleanup,
                                 bool seal = true) noexcept;
  Failure ownedCleanupFailure() const noexcept;
  // Private lexical-scope barrier. The caller keeps the control alive, retries
  // after Pending, and releases it only after Complete. No source body runs.
  CleanupStart prepareOwnedScope(ResourceControl &) noexcept;
  bool leaveOwnedScope(ResourceControl *, const OwnedDescriptor *escaping = nullptr) noexcept;
  bool await(TaskHandle) noexcept;
  bool join(GroupHandle) noexcept;
  bool releaseGroup(GroupHandle) noexcept;
  bool read(TaskHandle, const ValueLayout &, void *output) noexcept;
  Failure failure(GroupHandle) const noexcept;
  Failure failure(TaskHandle) const noexcept;
  bool close(GroupHandle) noexcept;
  bool cancel(GroupHandle, bool deadline = false) noexcept;
  bool failGroup(GroupHandle, Failure) noexcept;
  // Reserve the primary group outcome for an application error held in the
  // owning compiler frame, then cancel/drain descendants. No payload crosses
  // this private scheduler boundary. False preserves an earlier task failure.
  bool stopGroup(GroupHandle) noexcept;
  bool cancelled() const noexcept;
  // Trusted runtime leaf proof, not a source-language cancellation opt-out.
  // Valid only while this execution lease remains active and the leaf publishes
  // no new independent work or externally usable capability before completion.
  bool canExecuteSynchronously() const noexcept;
  ExternalCompletion suspendExternal() noexcept;
  void suspendUntil(std::chrono::steady_clock::time_point deadline) noexcept;
  void yield() noexcept;
  void complete(const void *value) noexcept;
  // Trusted terminal publication from an independently retained outcome ledger,
  // after persistent producer locals have been cleaned. Source remains exclusively
  // readable for this execution lease; normal completion uses context().
  void completeFrom(const Invocation &source, const void *value) noexcept;
  // Propagate a settled child/group error without converting its variant to Fault.
  // An existing invocation fault or cancellation still takes precedence.
  void fail(Failure) noexcept;
  void fault(std::int64_t code, std::int64_t line = 0, std::int64_t column = 0) noexcept;

private:
  Task(TaskCore::Impl &core, std::uint32_t slot) : core_(core), slot_(slot) {}
  TaskCore::Impl &core_;
  std::uint32_t slot_;
  friend struct TaskCore::Impl;
};
} // namespace kalandra::runtime
