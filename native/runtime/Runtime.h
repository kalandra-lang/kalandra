#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <new>
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
  // User-provided so Invocation{} and make_unique do not zero deferred storage.
  Invocation() noexcept {}
  ~Invocation() { settle(); }
  Invocation(const Invocation &) = delete;
  Invocation &operator=(const Invocation &) = delete;
  void bindCancellation(const std::atomic<bool> *token) noexcept {
    cancellation_ = token;
    if (pollMode_ < 2) pollMode_ = token ? 1 : 0;
  }
  // Private scheduler capability; never grants interruption of foreign frames.
  void allowNativeInterrupts(bool available) noexcept {
    flags_ = available ? flags_ | NativeInterrupts : flags_ & ~NativeInterrupts;
  }
  // Private test probe; its atomic counter must outlive this invocation.
  void watchPolls(std::atomic<std::uint64_t> *counter) noexcept { ledger().pollCounter_ = counter; pollMode_ = 2; }
  std::int64_t poll() noexcept {
    // Fault, settlement and observation all enter mode 2. Healthy helpers can
    // reuse that invariant while retaining their acquire cancellation check.
    if (pollMode_ == 0) return 0;
    if (pollMode_ == 1 || pollMode_ == 3)
      return cancellation_ && cancellation_->load(std::memory_order_acquire) ? -1 : 0;
    return pollObserved();
  }
  // Runtime-boundary nesting is diagnostic accounting, not a recursion budget.
  // Actual stack bounds protect native frames; tail calls reuse their frame.
  bool enterCall(std::int64_t line, std::int64_t column) noexcept;
  void leaveCall() noexcept;
  std::uint64_t callDepth() const noexcept { return flags_ & ControlReady ? callDepth_ : 0; }
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
  std::uint64_t environmentBytes() const noexcept { return flags_ & Materialized ? storage_.value.environmentBytes_ : 0; }
  // Conservative per-invocation reservations for all backing allocation and
  // retention in bound task contexts. Include backing and one owner; ordinary
  // retains each reserve independently, while graph visits reuse charged owners.
  // Charges remain reserved until settlement, including after explicit release.
  void bindRecursiveBudget(void *state, EnvironmentReserve reserve,
                           EnvironmentRelease release) noexcept;
  std::uint64_t recursiveBytes() const noexcept { return flags_ & Materialized ? storage_.value.recursiveBytes_ : 0; }
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
  bool usable() const noexcept {
    if (flags_ & Settled) return false;
    if (flags_ & Materialized)
      return !code_ && !storage_.value.taskError_ && !storage_.value.applicationFamily_;
    return !status();
  }
  std::optional<AllocationLease> lease(const void *allocated) const noexcept;
  void fault(std::int64_t code, std::int64_t line = 0, std::int64_t column = 0) noexcept;
  void settle() noexcept {
    if (flags_ & Settled) return;
    if (!(flags_ & Materialized)) {
      pollMode_ = 2;
      flags_ |= Settled;
      return;
    }
    // Scalar work need not cross the runtime ABI to settle an empty ledger.
    // Keep owned buffers and both resource-budget release paths together.
    const unsigned resources = unsigned(ledger().head_ != nullptr) | unsigned(ledger().spareSmall_ != nullptr) |
                               unsigned(ledger().environmentBytes_ != 0) | unsigned(ledger().recursiveBytes_ != 0);
    if (!resources) {
      pollMode_ = 2;
      flags_ |= Settled;
      return;
    }
    settleResources();
  }
  void expectedTaskError(std::int64_t kind, std::int64_t code, std::int64_t line,
                         std::int64_t column) noexcept;
  void expectedApplicationError(const char *family, const char *variant) noexcept;
  bool hasTaskError() const noexcept { return (flags_ & Materialized) && storage_.value.taskError_ != 0; }
  bool hasApplicationError() const noexcept { return (flags_ & Materialized) && storage_.value.applicationFamily_ != nullptr; }
  bool hasExpectedError() const noexcept { return hasTaskError() || hasApplicationError(); }
  [[noreturn]] void reportFailure() noexcept; // CLI boundary only, never generated code.
  void failAllocationAt(std::uint64_t attempt) noexcept {
    ledger().failAt_ = attempt;
    flags_ |= AllocationConfigured;
  }
  std::int64_t status() const noexcept { return flags_ & ControlReady ? code_ : 0; }
  std::int64_t executionStatus() noexcept {
    if (flags_ & Settled)
      fault(3);
    return status();
  }
  std::int64_t line() const noexcept { return flags_ & ControlReady ? line_ : 0; }
  std::int64_t column() const noexcept { return flags_ & ControlReady ? column_ : 0; }
  // An atomic diagnostic snapshot; it does not publish ledger/backing data.
  std::uint64_t live() const noexcept { return live_.load(std::memory_order_relaxed); }
  std::uint64_t allocations() const noexcept { return flags_ & Materialized ? storage_.value.allocations_ : 0; }
  std::uint64_t releases() const noexcept { return flags_ & Materialized ? storage_.value.releases_ : 0; }

private:
  friend struct InvocationABI;
  // Only the token, control tag and atomic diagnostic exist at cold entry.
  const std::atomic<bool> *cancellation_ = nullptr;
  enum Flag : std::uint32_t {
    Settled = 1, AllocationConfigured = 2, NativeInterrupts = 4,
    Materialized = 8, ControlReady = 16
  };
  std::uint32_t flags_ = 0;
  // 0 clear, 1 token, 2 observed/faulted/settled, 3 native protected.
  std::uint8_t pollMode_ = 0;
  std::atomic<std::uint64_t> live_{0};
  // Source execution through a managed boundary initializes these once.
  std::uint64_t callDepth_;
  std::int64_t code_, line_, column_;
  [[maybe_unused]] std::uint64_t stackLeaseDepth_, stackLow_, stackHigh_;
  void initializeControl() noexcept {
    if (flags_ & ControlReady) return;
    callDepth_ = 0;
    code_ = line_ = column_ = 0;
    stackLeaseDepth_ = stackLow_ = stackHigh_ = 0;
    flags_ |= ControlReady;
  }
  // Inline storage, but no ledger object until a resource/observer is needed.
  // References still cross contexts through Backing's atomic ownership.
  struct Ledger {
    Owner *head_ = nullptr;
    std::atomic<std::uint64_t> *pollCounter_ = nullptr;
    std::uint64_t environmentBytes_ = 0, recursiveBytes_ = 0;
    void *recursiveBudget_ = nullptr;
    EnvironmentReserve recursiveReserve_ = nullptr;
    EnvironmentRelease recursiveRelease_ = nullptr;
    void *environmentBudget_ = nullptr;
    EnvironmentReserve environmentReserve_ = nullptr;
    EnvironmentRelease environmentRelease_ = nullptr;
    std::uint64_t allocations_ = 0, releases_ = 0, attempts_ = 0, failAt_ = 0;
    std::int64_t taskError_ = 0, expectedCode_ = 0, expectedLine_ = 0, expectedColumn_ = 0;
    const char *applicationFamily_ = nullptr, *applicationVariant_ = nullptr;
    // Released exclusive small buffer; logical charges still precede reuse.
    Backing *spareSmall_ = nullptr;
  };
  static_assert(std::is_trivially_destructible_v<Ledger>);
  union LedgerStorage {
    Ledger value;
    LedgerStorage() noexcept {}
    ~LedgerStorage() {}
  } storage_;
  static const Ledger emptyLedger_;
  void materializeLedger() noexcept;
  Ledger &ledger() noexcept {
    if (!(flags_ & Materialized)) {
      materializeLedger();
      flags_ |= Materialized;
    }
    return storage_.value;
  }
  const Ledger &ledger() const noexcept {
    return flags_ & Materialized ? storage_.value : emptyLedger_;
  }
  const void *makeRecursiveValueImpl(std::span<const std::byte>, const void *,
                                     std::span<const AllocationLease>, std::span<const void *const>,
                                     bool) noexcept;
  bool reserve() noexcept;
  std::int64_t pollObserved() noexcept;
  void settleResources() noexcept;
  void attach(Owner *owner) noexcept;
  void remove(Owner *owner) noexcept;
};
// The supported 64-bit CPU ABIs share this invocation layout. Keep compiler
// field access and runtime storage identical even when built on different hosts.
struct InvocationABI {
  static std::uint8_t armNativeCancellation(Invocation &invocation) noexcept {
    invocation.initializeControl();
    const auto previous = invocation.pollMode_;
    if (previous == 1 && (invocation.flags_ & Invocation::NativeInterrupts)) invocation.pollMode_ = 3;
    return previous;
  }
  static bool nativeCancellationArmed(const Invocation &invocation) noexcept {
    return invocation.pollMode_ == 3;
  }
  static const std::atomic<bool> *cancellationToken(const Invocation &invocation) noexcept {
    return invocation.cancellation_;
  }
  static void restoreNativeCancellation(Invocation &invocation, std::uint8_t mode) noexcept {
    if (invocation.pollMode_ == 3)
      invocation.pollMode_ = (mode == 1 || mode == 3) && !invocation.cancellation_ ? 0 : mode;
  }
  static void cacheStackBounds(Invocation &) noexcept;
  static constexpr std::size_t ledgerStorage = offsetof(Invocation, storage_);
  static bool resourceLedgerMaterialized(const Invocation &invocation) noexcept {
    return invocation.flags_ & Invocation::Materialized;
  }
  static constexpr std::size_t control = offsetof(Invocation, flags_);
  static constexpr std::uint32_t controlReady = Invocation::ControlReady;
  static constexpr std::size_t pollMode = offsetof(Invocation, pollMode_);
  static constexpr std::size_t cancellation = offsetof(Invocation, cancellation_);
  static constexpr std::size_t leaseDepth = offsetof(Invocation, stackLeaseDepth_);
  static constexpr std::size_t stackLow = offsetof(Invocation, stackLow_);
  static constexpr std::size_t stackHigh = offsetof(Invocation, stackHigh_);
  static constexpr std::size_t depth = offsetof(Invocation, callDepth_);
  static constexpr std::size_t faultCode = offsetof(Invocation, code_);
  static constexpr std::size_t faultLine = offsetof(Invocation, line_);
  static constexpr std::size_t faultColumn = offsetof(Invocation, column_);
};
static_assert(std::is_standard_layout_v<Invocation>);
// Generated entries allocate this storage and access these fields directly.
// Fail the compiler/runtime build if a toolchain or future edit changes the ABI;
// cross-compilers must never use a source-host layout for a different target.
static_assert(sizeof(Invocation) == 248 && alignof(Invocation) == 8);
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && alignof(std::atomic<std::uint64_t>) == 8);
static_assert(InvocationABI::cancellation == 0 && InvocationABI::control == 8 &&
              InvocationABI::pollMode == 12 && InvocationABI::depth == 24 &&
              InvocationABI::faultCode == 32 && InvocationABI::faultLine == 40 &&
              InvocationABI::faultColumn == 48 && InvocationABI::leaseDepth == 56 &&
              InvocationABI::stackLow == 64 && InvocationABI::stackHigh == 72 &&
              InvocationABI::ledgerStorage == 80);
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

// Experimental process-input prototype. Initialize before entering user code or workers.
extern "C" void kalandra_process_arguments_v1(std::int32_t argc, char **argv) noexcept;
extern "C" std::int32_t kalandra_args_int32_v1(kalandra::runtime::Invocation *, std::int64_t index) noexcept;
extern "C" std::int32_t kalandra_random_int32_v1(kalandra::runtime::Invocation *, std::int32_t minimum, std::int32_t maximumExclusive) noexcept;
