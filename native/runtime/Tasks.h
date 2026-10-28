#pragma once
#include "Runtime.h"
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
struct TaskEntry {
  std::uint32_t revision;
  std::uint64_t packetLayout;
  std::size_t packetBytes, packetAlignment, frameBytes, frameAlignment;
  bool (*construct)(Invocation &, void *frame, const Packet &);
  void (*destroy)(void *frame) noexcept;
  void (*resume)(Task &);
  const ValueLayout *result;
};
struct TaskLimits {
  std::uint32_t workers = 1;
  std::uint32_t records = 128, groups = 32, ready = 32, inFlight = 64, depth = 16;
  std::size_t bytes = 1024 * 1024;
};
struct TaskStats {
  std::size_t retainedBytes = 0, peakBytes = 0, environmentBytes = 0, recursiveBytes = 0;
  std::uint64_t retainedOwners = 0;
  std::uint32_t unfinished = 0, records = 0, groups = 0;
  std::uint32_t ready = 0, pending = 0, maintenanceTasks = 0, externalPending = 0;
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
  friend class ExternalCompletion;
  friend class Task;
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
  ExternalCompletion suspendExternal() noexcept;
  void yield() noexcept;
  void complete(const void *value) noexcept;
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
