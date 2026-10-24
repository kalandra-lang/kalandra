#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace kalandra::runtime {
struct Owner;
struct Backing;
// A read-only retained allocation independent of any invocation's owner list.
// Move-only: shared values retain their enclosing backing, not this lease itself.
class AllocationLease final {
public:
  AllocationLease() = default;
  ~AllocationLease();
  AllocationLease(AllocationLease &&) noexcept;
  AllocationLease &operator=(AllocationLease &&) noexcept;
  AllocationLease(const AllocationLease &) = delete;
  AllocationLease &operator=(const AllocationLease &) = delete;
  explicit operator bool() const noexcept { return backing_ != nullptr; }
  std::span<const std::byte> bytes() const noexcept;
  // Private diagnostic snapshot, not a synchronization/uniqueness predicate.
  std::uint64_t referenceCount() const noexcept;
  std::optional<AllocationLease> dependency(std::size_t index) const noexcept;
  // Private typed projection of an immutable recursive payload. The retained
  // lease, rather than the originating invocation, owns the returned view.
  std::optional<std::span<const std::byte>> recursiveBytes(const void *key, std::uint64_t bytes,
                                                           std::uint64_t alignment) const noexcept;

private:
  Backing *backing_ = nullptr;
  explicit AllocationLease(Backing *backing) noexcept : backing_(backing) {}
  friend class Invocation;
};
// One execution unit. Access to a context is exclusive, even if its execution
// later migrates between workers. Backing references can cross contexts.
class Invocation final {
public:
  Invocation() noexcept;
  ~Invocation();
  Invocation(const Invocation &) = delete;
  Invocation &operator=(const Invocation &) = delete;
  void bindCancellation(const std::atomic<bool> *token) noexcept {
    cancellation_ = token;
    if (pollMode_ < 2) pollMode_ = token ? 1 : 0;
  }
  // Private test probe; its atomic counter must outlive this invocation.
  void watchPolls(std::atomic<std::uint64_t> *counter) noexcept { pollCounter_ = counter; pollMode_ = 2; }
  std::int64_t poll() noexcept {
    if (pollCounter_)
      pollCounter_->fetch_add(1, std::memory_order_relaxed);
    const auto code = executionStatus();
    return code ? code : cancellation_ && cancellation_->load(std::memory_order_acquire) ? -1 : 0;
  }
  // Runtime-boundary nesting is diagnostic accounting, not a recursion budget.
  // Actual stack bounds protect native frames; tail calls reuse their frame.
  bool enterCall(std::int64_t line, std::int64_t column) noexcept;
  void leaveCall() noexcept;
  std::uint64_t callDepth() const noexcept { return callDepth_; }
  void *allocate(std::uint64_t bytes) noexcept;
  // Field backing remains invocation-owned across inline aggregate copies.
  // Release only at a scalar CLI boundary; ordinary helpers may return records.
  void *allocateRecord(std::uint64_t bytes) noexcept;
  // Private compiler bridge: copy a complete payload and retain all backing
  // dependencies before publication. The compiler must enumerate every owned
  // field; this is not a raw-pointer or worker-transport language surface.
  static constexpr std::size_t maxRecursiveDependencies = 300000;
  const void *makeRecursiveValue(std::span<const std::byte> payload, const void *key,
                                 std::span<const AllocationLease> dependencies = {}) noexcept;
  // Compiler stack tables may contain null slots for inactive enum payloads.
  const void *makeRecursiveValueFromPointers(std::span<const std::byte> payload, const void *key,
                                             std::span<const void *const> dependencies) noexcept;
  // Bind compiler-created recursive environments once, before publishing the function pair.
  const void *bindRecursiveEnvironment(const void *environment, const void *code) noexcept;
  const void *readRecursiveEnvironment(const void *environment, std::uint64_t bytes,
                                       const void *code) noexcept;
  // Private local handoff of an immutable, code-bound capture graph. Source
  // invocation access is exclusive, as for ordinary recursive graph transfer.
  const void *retainRecursiveEnvironment(const Invocation &source, const void *environment,
                                         std::uint64_t bytes, std::uint64_t alignment,
                                         const void *code) noexcept;
  const void *recursiveFunctionDependency(const void *code, const void *environment) noexcept;
  using EnvironmentReserve = bool (*)(void *, std::uint64_t) noexcept;
  using EnvironmentRelease = void (*)(void *, std::uint64_t) noexcept;
  void bindEnvironmentBudget(void *state, EnvironmentReserve reserve,
                             EnvironmentRelease release) noexcept;
  std::uint64_t environmentBytes() const noexcept { return environmentBytes_; }
  // Conservative per-invocation reservations for all backing allocation and
  // retention in bound task contexts. Include backing and one owner; ordinary
  // retains each reserve independently, while graph visits reuse charged owners.
  // Charges remain reserved until settlement, including after explicit release.
  void bindRecursiveBudget(void *state, EnvironmentReserve reserve,
                           EnvironmentRelease release) noexcept;
  std::uint64_t recursiveBytes() const noexcept { return recursiveBytes_; }
  void *allocateEnvironment(std::uint64_t bytes, const void *key) noexcept;
  void *allocateTransportStorage(std::uint64_t bytes, const void *key) noexcept;
  void *cloneEnvironment(const Invocation &source, const void *environment, std::uint64_t bytes,
                         std::uint64_t alignment, const void *key) noexcept;
  void releaseRecords() noexcept;
  // Private local handoff: a retained, fully published immutable root may outlive
  // its producer. Register its complete dependency graph transactionally so
  // checked reads in this invocation can resolve every nested backing address.
  // Caller owns the lease throughout; destination invocation access is exclusive.
  bool retainRecursiveGraph(const AllocationLease &root, const void *key, std::uint64_t bytes,
                            std::uint64_t alignment) noexcept;
  bool retainFrom(const Invocation &source, void *buffer) noexcept;
  void release(void *buffer) noexcept;
  // Requires exclusive source-context access, like retainFrom. Reject unknown,
  // faulted or settled contexts before acquiring a backing reference.
  bool usable() const noexcept { return !settled_ && !code_ && !taskError_ && !applicationFamily_; }
  std::optional<AllocationLease> lease(const void *allocated) const noexcept;
  void fault(std::int64_t code, std::int64_t line = 0, std::int64_t column = 0) noexcept;
  void settle() noexcept;
  void expectedTaskError(std::int64_t kind, std::int64_t code, std::int64_t line,
                         std::int64_t column) noexcept;
  void expectedApplicationError(const char *family, const char *variant) noexcept;
  bool hasTaskError() const noexcept { return taskError_ != 0; }
  bool hasApplicationError() const noexcept { return applicationFamily_ != nullptr; }
  bool hasExpectedError() const noexcept { return hasTaskError() || hasApplicationError(); }
  [[noreturn]] void reportFailure() noexcept; // CLI boundary only, never generated code.
  void failAllocationAt(std::uint64_t attempt) noexcept { failAt_ = attempt; }
  std::int64_t status() const noexcept { return code_; }
  std::int64_t executionStatus() noexcept {
    if (settled_)
      fault(3);
    return code_;
  }
  std::int64_t line() const noexcept { return line_; }
  std::int64_t column() const noexcept { return column_; }
  std::uint64_t live() const noexcept { return live_; }
  std::uint64_t allocations() const noexcept { return allocations_; }
  std::uint64_t releases() const noexcept { return releases_; }

private:
  friend struct InvocationABI;
  Owner *head_ = nullptr;
  std::atomic<std::uint64_t> live_{0};
  const std::atomic<bool> *cancellation_ = nullptr;
  std::atomic<std::uint64_t> *pollCounter_ = nullptr;
  std::uint64_t callDepth_ = 0, environmentBytes_ = 0, recursiveBytes_ = 0;
  void *recursiveBudget_ = nullptr;
  EnvironmentReserve recursiveReserve_ = nullptr;
  EnvironmentRelease recursiveRelease_ = nullptr;
  void *environmentBudget_ = nullptr;
  EnvironmentReserve environmentReserve_ = nullptr;
  EnvironmentRelease environmentRelease_ = nullptr;
  std::uint64_t allocations_ = 0, releases_ = 0, attempts_ = 0, failAt_ = 0;
  std::int64_t code_ = 0, line_ = 0, column_ = 0;
  bool settled_ = false;
  std::int64_t taskError_ = 0, expectedCode_ = 0, expectedLine_ = 0, expectedColumn_ = 0;
  const char *applicationFamily_ = nullptr, *applicationVariant_ = nullptr;
  // 0: clear; 1: acquire-load the bound cancellation token; 2: observed/faulted/settled.
  std::uint8_t pollMode_ = 0;
  // A lease lasts only while a synchronous generated native call chain is live.
  // All exits clear it at its logical entry depth, before worker migration.
  [[maybe_unused]] std::uint64_t stackLeaseDepth_ = 0, stackLow_ = 0, stackHigh_ = 0;
  const void *makeRecursiveValueImpl(std::span<const std::byte>, const void *,
                                     std::span<const AllocationLease>, std::span<const void *const>,
                                     bool) noexcept;
  bool reserve() noexcept;
  void attach(Owner *owner) noexcept;
  void remove(Owner *owner) noexcept;
};
// Native compiler and runtime are built for the same pinned host ABI. Keep the
// field access used by checked fast paths explicit and mechanically verified.
struct InvocationABI {
  static void cacheStackBounds(Invocation &) noexcept;
  static constexpr std::size_t pollMode = offsetof(Invocation, pollMode_);
  static constexpr std::size_t cancellation = offsetof(Invocation, cancellation_);
  static constexpr std::size_t leaseDepth = offsetof(Invocation, stackLeaseDepth_);
  static constexpr std::size_t stackLow = offsetof(Invocation, stackLow_);
  static constexpr std::size_t stackHigh = offsetof(Invocation, stackHigh_);
  static constexpr std::size_t depth = offsetof(Invocation, callDepth_);
};
static_assert(std::is_standard_layout_v<Invocation>);
static_assert(sizeof(void *) == 8 && sizeof(std::atomic<bool>) == 1 &&
              alignof(std::atomic<bool>) == 1 && std::atomic<bool>::is_always_lock_free);
} // namespace kalandra::runtime

// Revision is encoded in the symbols: mismatched compiler/runtime builds fail
// at link time. No process-global or thread-local current invocation exists.
extern "C" {
const void *kalandra_recursive_make_v1(kalandra::runtime::Invocation *, const void *, std::uint64_t,
                                       std::uint64_t, const void *, const void *const *,
                                       std::uint64_t) noexcept;
const void *kalandra_recursive_read_v1(kalandra::runtime::Invocation *, const void *, std::uint64_t,
                                       std::uint64_t, const void *) noexcept;
void kalandra_expected_application_error_v1(kalandra::runtime::Invocation *, const char *,
                                            const char *) noexcept;
void kalandra_bool_array_validate_v1(kalandra::runtime::Invocation *, const void *) noexcept;
bool kalandra_bool_array_retain_v1(kalandra::runtime::Invocation *,
                                   const kalandra::runtime::Invocation *, const void *) noexcept;
void kalandra_record_array_validate_v1(kalandra::runtime::Invocation *, const void *, std::uint64_t,
                                       std::uint64_t) noexcept;
bool kalandra_record_array_retain_v1(kalandra::runtime::Invocation *,
                                     const kalandra::runtime::Invocation *, const void *,
                                     std::uint64_t, std::uint64_t) noexcept;
void *kalandra_record_alloc_v1(kalandra::runtime::Invocation *, std::uint64_t) noexcept;
void *kalandra_environment_alloc_v1(kalandra::runtime::Invocation *, std::uint64_t,
                                    const void *) noexcept;
bool kalandra_environment_tensor_retain_v1(kalandra::runtime::Invocation *,
                                           const kalandra::runtime::Invocation *, const void *,
                                           std::uint64_t, std::uint64_t) noexcept;
bool kalandra_function_array_clone_v1(kalandra::runtime::Invocation *, void *,
                                      const kalandra::runtime::Invocation *, const void *,
                                      std::uint64_t, std::uint64_t, const void *) noexcept;
void *kalandra_environment_clone_v1(kalandra::runtime::Invocation *,
                                    const kalandra::runtime::Invocation *, const void *,
                                    std::uint64_t, std::uint64_t, const void *) noexcept;
void kalandra_record_cleanup_v1(kalandra::runtime::Invocation *) noexcept;
void kalandra_record_validate_v1(kalandra::runtime::Invocation *, const void *, std::uint64_t,
                                 std::uint64_t) noexcept;
bool kalandra_record_retain_v1(kalandra::runtime::Invocation *,
                               const kalandra::runtime::Invocation *, const void *, std::uint64_t,
                               std::uint64_t) noexcept;
bool kalandra_retain_v1(kalandra::runtime::Invocation *, const kalandra::runtime::Invocation *,
                        void *) noexcept;
void kalandra_expected_task_error_v1(kalandra::runtime::Invocation *, std::int64_t, std::int64_t,
                                     std::int64_t, std::int64_t) noexcept;
void *kalandra_alloc_v1(kalandra::runtime::Invocation *, std::uint64_t bytes) noexcept;
void kalandra_free_v1(kalandra::runtime::Invocation *, void *buffer) noexcept;
void kalandra_fault_v1(kalandra::runtime::Invocation *, std::int64_t code, std::int64_t line,
                       std::int64_t column) noexcept;
bool kalandra_call_enter_v1(kalandra::runtime::Invocation *, std::int64_t, std::int64_t) noexcept;
void kalandra_call_leave_v1(kalandra::runtime::Invocation *) noexcept;
std::int64_t kalandra_poll_v1(kalandra::runtime::Invocation *) noexcept;
std::int64_t kalandra_status_v1(kalandra::runtime::Invocation *) noexcept;
}

// Private recursive-box bridges return borrowed validated backing; publication retains it.
extern "C" const void *kalandra_recursive_tensor_dependency_v1(kalandra::runtime::Invocation *,
                                                               const void *descriptor,
                                                               std::uint64_t rank,
                                                               std::uint64_t element,
                                                               std::uint64_t active) noexcept;
extern "C" const void *kalandra_recursive_bool_dependency_v1(kalandra::runtime::Invocation *,
                                                             const void *descriptor,
                                                             std::uint64_t active) noexcept;

// Private compiler callbacks use the checked invocation argument in the final position.
using KalandraRecursiveRecordCell = const void *(*)(const void *, kalandra::runtime::Invocation *);
extern "C" const void *kalandra_recursive_record_array_dependency_v1(
    kalandra::runtime::Invocation *, const void *descriptor, std::uint64_t cellBytes,
    std::uint64_t alignment, KalandraRecursiveRecordCell, std::uint64_t active) noexcept;
extern "C" const void *
kalandra_recursive_reference_dependency_v1(kalandra::runtime::Invocation *, const void *node,
                                           std::uint64_t bytes, std::uint64_t alignment,
                                           const void *key, std::uint64_t active) noexcept;
extern "C" const void *kalandra_recursive_tag_check_v1(kalandra::runtime::Invocation *,
                                                       std::uint64_t tag, std::uint64_t cases,
                                                       std::uint64_t active) noexcept;

extern "C" const void *kalandra_recursive_environment_bind_v1(kalandra::runtime::Invocation *,
                                                              const void *environment,
                                                              const void *code) noexcept;
extern "C" const void *kalandra_recursive_environment_read_v1(kalandra::runtime::Invocation *,
                                                              const void *environment,
                                                              std::uint64_t bytes,
                                                              const void *code) noexcept;
extern "C" const void *kalandra_recursive_function_dependency_v1(kalandra::runtime::Invocation *,
                                                                 const void *code,
                                                                 const void *environment,
                                                                 std::uint64_t active) noexcept;

extern "C" const void *kalandra_recursive_function_array_dependency_v1(
    kalandra::runtime::Invocation *, const void *descriptor, std::uint64_t active) noexcept;

// Private local packet bridge; source access is exclusive while its lease is acquired.
extern "C" const void *kalandra_recursive_transfer_v1(kalandra::runtime::Invocation *destination,
                                                      const kalandra::runtime::Invocation *source,
                                                      const void *root, std::uint64_t bytes,
                                                      std::uint64_t alignment,
                                                      const void *key) noexcept;

// Private checked task-field guard, including invocation-held expected errors.
extern "C" bool kalandra_invocation_ready_v1(kalandra::runtime::Invocation *) noexcept;

extern "C" const void *kalandra_recursive_environment_transfer_v1(
    kalandra::runtime::Invocation *, const kalandra::runtime::Invocation *, const void *environment,
    std::uint64_t bytes, std::uint64_t alignment, const void *code) noexcept;
