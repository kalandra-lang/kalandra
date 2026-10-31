#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "../include/kalandra/NumericType.h"
#include "Runtime.h"
#include "NativeCancellation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <charconv>
#include <mutex>
#include <random>
#include <string_view>

#if defined(__APPLE__)
// Trivial TLS is zero-initialized for each physical pthread, including reused
// thread storage. Never cache stack bounds in a migratable Invocation.
extern "C" {
// Use the C TLS ABI: C++ thread_local on Darwin hides storage behind a
// C++-mangled accessor, while generated LLVM references the C storage symbol.
__thread std::uintptr_t kalandra_stack_bounds_v1[2]{};
void kalandra_stack_bounds_init_v1() noexcept {
  const auto thread = pthread_self();
  const auto high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
  const auto size = pthread_get_stacksize_np(thread);
  constexpr std::uintptr_t reserve = 128 * 1024;
  kalandra_stack_bounds_v1[0] = size > reserve && high >= size ? high - size + reserve : high;
  kalandra_stack_bounds_v1[1] = high;
}
}
#endif

#if !defined(__APPLE__)
namespace {
thread_local std::uintptr_t portableStackBounds[2]{};
void initializePortableStackBounds() noexcept {
  std::uintptr_t low = 0, high = 0;
#if defined(__linux__)
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
    void *base = nullptr;
    std::size_t size = 0;
    if (pthread_attr_getstack(&attributes, &base, &size) == 0) {
      low = reinterpret_cast<std::uintptr_t>(base);
      high = low + size;
    }
    pthread_attr_destroy(&attributes);
  }
#elif defined(_WIN32)
  ULONG_PTR bottom = 0, top = 0;
  GetCurrentThreadStackLimits(&bottom, &top);
  MEMORY_BASIC_INFORMATION region{};
  // AllocationBase includes the reserved stack, not just its current commit.
  // Windows probes grow committed pages as native frames descend.
  if (VirtualQuery(__builtin_frame_address(0), &region, sizeof(region))) {
    low = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
    high = top;
  }
#endif
  constexpr std::uintptr_t reserve = 128 * 1024;
  portableStackBounds[0] = high > low && high - low > reserve ? low + reserve : high;
  portableStackBounds[1] = high;
}
} // namespace
#endif

// Generated modules retain a link-time dependency on this private layout.
extern "C" const unsigned char kalandra_invocation_layout_v2 = 0;

namespace kalandra::runtime {
void InvocationABI::cacheStackBounds(Invocation &invocation) noexcept {
  invocation.initializeControl();
#if defined(__APPLE__)
  if (!kalandra_stack_bounds_v1[1])
    kalandra_stack_bounds_init_v1();
  const auto *bounds = kalandra_stack_bounds_v1;
#else
  if (!portableStackBounds[1])
    initializePortableStackBounds();
  const auto *bounds = portableStackBounds;
#endif
  invocation.stackLow_ = bounds[0];
  invocation.stackHigh_ = bounds[1];
}
namespace {
// Private native control transfer, never a source-level application exception.
// Generated frames contain no C++ owners: the invocation ledger owns their
// backing, and the runtime boundary balances its admission on both paths.
struct SourceAbort {};
}
extern "C" [[noreturn]] void kalandra_source_abort_v2() {
  throw SourceAbort{};
}
extern "C" bool kalandra_source_run_v3(Invocation *invocation, bool (*body)(void *),
                                        void *arguments) noexcept {
  if (!invocation->enterCall(0, 0))
    return false;
  bool complete = false;
  try {
    complete = body(arguments) && invocation->poll() == 0;
  } catch (const SourceAbort &) {
    // A cancellation remains a stopped execution, without becoming a fault.
  }
  invocation->leaveCall();
  return complete;
}
} // namespace kalandra::runtime
#include "NativeCancellation.inc"
namespace kalandra::runtime {
struct Backing;
struct Owner {
  Owner *next = nullptr;
  Owner *previous = nullptr;
  Backing *backing;
  bool embedded;
  bool recursiveCharged = false;
};
struct alignas(std::max_align_t) Backing {
  std::atomic<std::uint64_t> references{1};
  Owner initial{nullptr, nullptr, this, true};
  std::uint64_t bytes = 0, storageBytes = 0;
  bool record = false;
  const void *environmentKey = nullptr;
  const void *recursiveKey = nullptr;
  Backing **dependencies = nullptr;
  std::size_t dependencyCount = 0;
  Backing *releaseNext = nullptr; // Used exclusively after the final reference retires.
};
namespace {
// Reservations are transferred only when the corresponding owner is published.
struct BackingReservation {
  void *state;
  Invocation::EnvironmentRelease release;
  std::uint64_t bytes = 0;
  ~BackingReservation() {
    if (bytes)
      release(state, bytes);
  }
  bool acquire(Invocation &context, std::uint64_t storageBytes, std::uint64_t current,
               Invocation::EnvironmentReserve reserve) noexcept {
    if (!reserve)
      return true;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (storageBytes > maximum - sizeof(Owner) ||
        storageBytes + sizeof(Owner) > maximum - current ||
        !reserve(state, storageBytes + sizeof(Owner))) {
      context.fault(2);
      return false;
    }
    bytes = storageBytes + sizeof(Owner);
    return !context.poll();
  }
};
bool retainBacking(Backing *backing) noexcept {
  auto references = backing->references.load(std::memory_order_relaxed);
  do {
    if (references == std::numeric_limits<std::uint64_t>::max())
      return false;
  } while (!backing->references.compare_exchange_weak(references, references + 1,
                                                      std::memory_order_relaxed));
  return true;
}
void releaseBacking(Backing *backing) noexcept {
  if (!backing || backing->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  // Only the thread observing the final reference may write releaseNext. Each
  // edge owns a reference, including duplicates, so a shared child is enqueued
  // exactly once. The intrusive worklist needs no allocation or native recursion.
  auto *pending = backing;
  while (pending) {
    auto *current = pending;
    pending = current->releaseNext;
    for (std::size_t edge = 0; edge < current->dependencyCount; ++edge) {
      auto *child = current->dependencies[edge];
      if (child->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        child->releaseNext = pending;
        pending = child;
      }
    }
    current->~Backing();
    std::free(current);
  }
}
} // namespace
AllocationLease::~AllocationLease() { releaseBacking(backing_); }
AllocationLease::AllocationLease(AllocationLease &&other) noexcept
    : backing_(std::exchange(other.backing_, nullptr)) {}
AllocationLease &AllocationLease::operator=(AllocationLease &&other) noexcept {
  if (this != &other) {
    releaseBacking(backing_);
    backing_ = std::exchange(other.backing_, nullptr);
  }
  return *this;
}
std::span<const std::byte> AllocationLease::bytes() const noexcept {
  return backing_ ? std::span<const std::byte>(reinterpret_cast<const std::byte *>(backing_ + 1),
                                               backing_->bytes)
                  : std::span<const std::byte>{};
}
std::uint64_t AllocationLease::referenceCount() const noexcept {
  return backing_ ? backing_->references.load(std::memory_order_relaxed) : 0;
}
std::optional<AllocationLease> AllocationLease::dependency(std::size_t index) const noexcept {
  if (!backing_ || index >= backing_->dependencyCount)
    return {};
  auto *child = backing_->dependencies[index];
  if (!retainBacking(child))
    return {};
  return AllocationLease(child);
}
std::optional<std::span<const std::byte>>
AllocationLease::recursiveBytes(const void *key, std::uint64_t size,
                                std::uint64_t alignment) const noexcept {
  if (!backing_ || !key || backing_->recursiveKey != key || backing_->bytes != size || !alignment ||
      alignment > alignof(std::max_align_t) || (alignment & (alignment - 1)) ||
      reinterpret_cast<std::uintptr_t>(backing_ + 1) % alignment)
    return {};
  return bytes();
}
std::optional<AllocationLease> Invocation::lease(const void *allocated) const noexcept {
  auto &ledgerState = ledger();
  if (!usable() || !allocated)
    return {};
  auto *owner = ledgerState.head_;
  while (owner && owner->backing + 1 != allocated)
    owner = owner->next;
  if (!owner)
    return {};
  auto *backing = owner->backing;
  if (!retainBacking(backing))
    return {};
  return AllocationLease(backing);
}
constinit const Invocation::Ledger Invocation::emptyLedger_{};
void Invocation::materializeLedger() noexcept {
  initializeControl();
  ::new (static_cast<void *>(&storage_.value)) Ledger{};
}
std::int64_t Invocation::pollObserved() noexcept {
  if ((flags_ & Materialized) && storage_.value.pollCounter_)
    storage_.value.pollCounter_->fetch_add(1, std::memory_order_relaxed);
  const auto code = executionStatus();
  return code ? code : cancellation_ && cancellation_->load(std::memory_order_acquire) ? -1 : 0;
}
void Invocation::fault(std::int64_t code, std::int64_t line, std::int64_t column) noexcept {
  initializeControl();
  pollMode_ = 2;
  if (!code_) {
    code_ = code ? code : 3;
    line_ = line;
    column_ = column;
  }
}
namespace {
std::uint64_t processAllocationFailure() noexcept {
  static const auto processFailure = []() noexcept -> std::uint64_t {
    if (const auto *text = std::getenv("KALANDRA_TEST_FAIL_ALLOC_AT")) {
      char *end = nullptr;
      const auto failure = std::strtoull(text, &end, 10);
      if (end != text && *end == '\0') return failure;
    }
    return 0;
  }();
  return processFailure;
}
}
extern "C" std::uint64_t kalandra_ordinary_local_enabled_v1() noexcept {
  return processAllocationFailure() == 0;
}
bool Invocation::reserve() noexcept {
  // Private callers have already admitted/materialized this ledger.
  auto &ledgerState = storage_.value;
  if (flags_ & Settled)
    fault(3);
  if (code_)
    return false;
  if (!(flags_ & AllocationConfigured)) {
    flags_ |= AllocationConfigured;
    // Immutable process test configuration, resolved once on first resource
    // use. Explicit per-invocation settings (including zero) bypass this default.
    ledgerState.failAt_ = processAllocationFailure();
  }
  if (++ledgerState.attempts_ == ledgerState.failAt_) {
    fault(2);
    return false;
  }
  return true;
}
void Invocation::attach(Owner *owner) noexcept {
  // Private callers have already admitted/materialized this ledger.
  auto &ledgerState = storage_.value;
  owner->next = ledgerState.head_;
  owner->previous = nullptr;
  if (ledgerState.head_)
    ledgerState.head_->previous = owner;
  ledgerState.head_ = owner;
  // Invocation mutation has one exclusive writer. Keep diagnostic reads atomic
  // without paying for an inter-writer read/modify/write transaction.
  live_.store(live_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  ++ledgerState.allocations_;
}
void Invocation::remove(Owner *owner) noexcept {
  // Private callers have already admitted/materialized this ledger.
  auto &ledgerState = storage_.value;
  if (owner->previous)
    owner->previous->next = owner->next;
  else
    ledgerState.head_ = owner->next;
  if (owner->next)
    owner->next->previous = owner->previous;
  auto *backing = owner->backing;
  const bool recycle = !ledgerState.spareSmall_ && owner->embedded && !backing->record &&
                       backing->bytes <= 256 && !backing->dependencyCount &&
                       backing->references.load(std::memory_order_acquire) == 1;
  if (!owner->embedded) {
    owner->~Owner();
    std::free(owner);
  }
  if (recycle)
    ledgerState.spareSmall_ = backing;
  else
    releaseBacking(backing);
  live_.store(live_.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
  ++ledgerState.releases_;
}
void *Invocation::allocate(std::uint64_t bytes) noexcept {
  auto &ledgerState = ledger();
  if (poll() || !usable() || !reserve())
    return nullptr;
  if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(Backing)) {
    fault(2);
    return nullptr;
  }
  const auto storageBytes = static_cast<std::size_t>(bytes) + sizeof(Backing);
  BackingReservation reservation{ledgerState.recursiveBudget_, ledgerState.recursiveRelease_};
  if (!reservation.acquire(*this, storageBytes, ledgerState.recursiveBytes_, ledgerState.recursiveReserve_))
    return nullptr;
  Backing *backing;
  if (ledgerState.spareSmall_ && ledgerState.spareSmall_->storageBytes == storageBytes) {
    // The cache owns the sole reference to this numeric backing. Its size and
    // immutable metadata still match; attach resets the detached owner links.
    backing = std::exchange(ledgerState.spareSmall_, nullptr);
  } else {
    auto *storage = std::malloc(storageBytes);
    if (!storage) {
      fault(2);
      return nullptr;
    }
    backing = new (storage) Backing;
    backing->bytes = bytes;
    backing->storageBytes = storageBytes;
  }
  ledgerState.recursiveBytes_ += reservation.bytes;
  backing->initial.recursiveCharged = reservation.bytes != 0;
  reservation.bytes = 0;
  attach(&backing->initial);
  return backing + 1;
}
void *Invocation::allocateRecord(std::uint64_t bytes) noexcept {
  auto &ledgerState = ledger();
  if (!bytes)
    return nullptr;
  auto *buffer = allocate(bytes);
  if (buffer)
    ledgerState.head_->backing->record = true;
  return buffer;
}
const void *Invocation::makeRecursiveValue(std::span<const std::byte> payload, const void *key,
                                           std::span<const AllocationLease> dependencies) noexcept {
  return makeRecursiveValueImpl(payload, key, dependencies, {}, false);
}
const void *
Invocation::makeRecursiveValueFromPointers(std::span<const std::byte> payload, const void *key,
                                           std::span<const void *const> dependencies) noexcept {
  return makeRecursiveValueImpl(payload, key, {}, dependencies, true);
}
const void *Invocation::makeRecursiveValueImpl(std::span<const std::byte> payload, const void *key,
                                               std::span<const AllocationLease> leases,
                                               std::span<const void *const> pointers,
                                               bool rawPointers) noexcept {
  auto &ledgerState = ledger();
  const auto dependencyCount = rawPointers ? pointers.size() : leases.size();
  if (poll() || !usable())
    return nullptr;
  if (!key || payload.empty() || !payload.data() || dependencyCount > maxRecursiveDependencies) {
    fault(3);
    return nullptr;
  }
  for (const auto &dependency : leases)
    if (!dependency) {
      fault(3);
      return nullptr;
    }
  constexpr auto alignment = alignof(Backing *);
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  if (payload.size() > maximum - sizeof(Backing) - (alignment - 1)) {
    fault(2);
    return nullptr;
  }
  const auto padded = (payload.size() + alignment - 1) & ~(alignment - 1);
  if (dependencyCount > (maximum - sizeof(Backing) - padded) / sizeof(Backing *)) {
    fault(2);
    return nullptr;
  }
  if (!reserve())
    return nullptr;
  const auto storageBytes = sizeof(Backing) + padded + dependencyCount * sizeof(Backing *);
  BackingReservation reservation{ledgerState.recursiveBudget_, ledgerState.recursiveRelease_};
  if (!reservation.acquire(*this, storageBytes, ledgerState.recursiveBytes_, ledgerState.recursiveReserve_))
    return nullptr;
  auto *storage = std::malloc(storageBytes);
  if (!storage) {
    fault(2);
    return nullptr;
  }
  auto *backing = new (storage) Backing;
  // The temporary lease rolls back partially retained edges on cancellation or
  // failure. No invocation owner sees the node until the copy is complete.
  AllocationLease pending(backing);
  backing->bytes = payload.size();
  backing->storageBytes = storageBytes;
  backing->record = true;
  backing->recursiveKey = key;
  auto *output = reinterpret_cast<std::byte *>(backing + 1);
  backing->dependencies = reinterpret_cast<Backing **>(output + padded);
  for (std::size_t index = 0; index < dependencyCount; ++index) {
    if (poll())
      return nullptr;
    Backing *child = nullptr;
    if (rawPointers) {
      if (!pointers[index])
        continue;
      auto *owner = ledgerState.head_;
      while (owner && owner->backing + 1 != pointers[index])
        owner = owner->next;
      if (owner)
        child = owner->backing;
    } else
      child = leases[index].backing_;
    if (!child || !retainBacking(child)) {
      fault(3);
      return nullptr;
    }
    backing->dependencies[backing->dependencyCount++] = child;
  }
  for (std::size_t offset = 0; offset < payload.size();) {
    if (poll())
      return nullptr;
    const auto count = std::min<std::size_t>(4096, payload.size() - offset);
    std::memcpy(output + offset, payload.data() + offset, count);
    offset += count;
  }
  if (poll())
    return nullptr;
  ledgerState.recursiveBytes_ += reservation.bytes;
  backing->initial.recursiveCharged = reservation.bytes != 0;
  reservation.bytes = 0; // Transfer the reservation only with complete ownership.
  attach(&backing->initial);
  pending.backing_ = nullptr; // Transfer its initial reference to the invocation.
  return output;
}
const void *Invocation::bindRecursiveEnvironment(const void *environment,
                                                 const void *code) noexcept {
  if (poll() || !usable())
    return nullptr;
  auto lease = this->lease(environment);
  if (!code || !lease || !lease->backing_->recursiveKey || lease->backing_->environmentKey) {
    fault(3);
    return nullptr;
  }
  lease->backing_->environmentKey = code;
  return environment;
}
const void *Invocation::readRecursiveEnvironment(const void *environment, std::uint64_t bytes,
                                                 const void *code) noexcept {
  if (poll() || !usable())
    return nullptr;
  auto lease = this->lease(environment);
  if (!code || !lease || lease->bytes().size() != bytes || !lease->backing_->recursiveKey ||
      lease->backing_->environmentKey != code) {
    fault(3);
    return nullptr;
  }
  return environment;
}
const void *Invocation::retainRecursiveEnvironment(const Invocation &source,
                                                   const void *environment, std::uint64_t bytes,
                                                   std::uint64_t alignment,
                                                   const void *code) noexcept {
  if (poll() || !usable())
    return nullptr;
  auto retained = source.lease(environment);
  if (!code || !retained || !retained->backing_->recursiveKey ||
      retained->backing_->environmentKey != code) {
    fault(3);
    return nullptr;
  }
  return retainRecursiveGraph(*retained, retained->backing_->recursiveKey, bytes, alignment)
             ? environment
             : nullptr;
}
const void *Invocation::recursiveFunctionDependency(const void *code,
                                                    const void *environment) noexcept {
  if (poll() || !usable())
    return nullptr;
  if (!code) {
    fault(3);
    return nullptr;
  }
  if (!environment)
    return nullptr; // Compiler-owned capture-free entry.
  auto lease = this->lease(environment);
  if (!lease || !lease->backing_->recursiveKey || lease->backing_->environmentKey != code) {
    fault(3);
    return nullptr;
  }
  return environment;
}
void Invocation::bindEnvironmentBudget(void *state, EnvironmentReserve reserve,
                                       EnvironmentRelease release) noexcept {
  auto &ledgerState = ledger();
  if (ledgerState.environmentBytes_ || ledgerState.environmentReserve_ || !state || !reserve || !release) {
    fault(3);
    return;
  }
  ledgerState.environmentBudget_ = state;
  ledgerState.environmentReserve_ = reserve;
  ledgerState.environmentRelease_ = release;
}
void Invocation::bindRecursiveBudget(void *state, EnvironmentReserve reserve,
                                     EnvironmentRelease release) noexcept {
  auto &ledgerState = ledger();
  if (ledgerState.recursiveBytes_ || ledgerState.recursiveReserve_ || !state || !reserve || !release) {
    fault(3);
    return;
  }
  ledgerState.recursiveBudget_ = state;
  ledgerState.recursiveReserve_ = reserve;
  ledgerState.recursiveRelease_ = release;
}
void *Invocation::allocateEnvironment(std::uint64_t bytes, const void *key) noexcept {
  auto &ledgerState = ledger();
  if (poll())
    return nullptr;
  if (!key || !bytes) {
    fault(3);
    return nullptr;
  }
  auto *result = allocateRecord(bytes);
  if (result)
    ledgerState.head_->backing->environmentKey = key;
  return result;
}
void *Invocation::allocateTransportStorage(std::uint64_t bytes, const void *key) noexcept {
  auto &ledgerState = ledger();
  if (poll())
    return nullptr;
  if (!key || !bytes) {
    fault(3);
    return nullptr;
  }
  if (bytes > 1024 * 1024 - ledgerState.environmentBytes_ ||
      (ledgerState.environmentReserve_ && !ledgerState.environmentReserve_(ledgerState.environmentBudget_, bytes))) {
    fault(2);
    return nullptr;
  }
  ledgerState.environmentBytes_ += bytes;
  return allocateEnvironment(bytes, key);
}
void *Invocation::cloneEnvironment(const Invocation &source, const void *environment,
                                   std::uint64_t bytes, std::uint64_t alignment,
                                   const void *key) noexcept {
  if (poll())
    return nullptr;
  if (!key || !bytes || !alignment || alignment > alignof(std::max_align_t) ||
      (alignment & (alignment - 1)) || reinterpret_cast<std::uintptr_t>(environment) % alignment) {
    fault(3);
    return nullptr;
  }
  auto lease = source.lease(environment);
  if (!lease || lease->bytes().size() != bytes || lease->backing_->environmentKey != key) {
    fault(3);
    return nullptr;
  }
  auto *output = static_cast<std::byte *>(allocateTransportStorage(bytes, key));
  if (!output)
    return nullptr;
  for (std::uint64_t offset = 0; offset < bytes; offset += 4096) {
    if (poll())
      return nullptr;
    const auto count = std::min<std::uint64_t>(4096, bytes - offset);
    std::memcpy(output + offset, lease->bytes().data() + offset, static_cast<std::size_t>(count));
  }
  return output;
}
void Invocation::releaseRecords() noexcept {
  auto &ledgerState = ledger();
  auto *owner = ledgerState.head_;
  while (owner) {
    auto *next = owner->next;
    if (owner->backing->record)
      remove(owner);
    owner = next;
  }
}
bool Invocation::retainFrom(const Invocation &source, void *buffer) noexcept {
  auto &ledgerState = ledger();
  if (poll() || !usable() || !reserve())
    return false;
  if (!source.usable()) {
    fault(3);
    return false;
  }
  auto *existing = source.ledger().head_;
  while (existing && existing->backing + 1 != buffer)
    existing = existing->next;
  if (!existing) {
    fault(3);
    return false;
  }
  auto *backing = existing->backing;
  // Every ordinary retain creates an owner. Conservatively reserve its backing
  // again, even if this invocation already has another owner for the same bytes.
  BackingReservation reservation{ledgerState.recursiveBudget_, ledgerState.recursiveRelease_};
  if (!reservation.acquire(*this, backing->storageBytes, ledgerState.recursiveBytes_, ledgerState.recursiveReserve_))
    return false;
  auto *storage = std::malloc(sizeof(Owner));
  if (!storage) {
    fault(2);
    return false;
  }
  // Source context access is exclusive during packet construction. Its owner
  // keeps this backing alive until the new reference has been established.
  if (!retainBacking(backing)) {
    std::free(storage);
    fault(3);
    return false;
  }
  auto *owner = new (storage) Owner{nullptr, nullptr, backing, false, reservation.bytes != 0};
  ledgerState.recursiveBytes_ += reservation.bytes;
  reservation.bytes = 0;
  attach(owner);
  return true;
}
bool Invocation::retainRecursiveGraph(const AllocationLease &root, const void *key,
                                      std::uint64_t bytes, std::uint64_t alignment) noexcept {
  auto &ledgerState = ledger();
  if (poll() || !usable())
    return false;
  if (!root.recursiveBytes(key, bytes, alignment)) {
    fault(3);
    return false;
  }
  struct Pending {
    Owner *head = nullptr;
    ~Pending() {
      while (head) {
        auto *owner = head;
        head = owner->next;
        releaseBacking(owner->backing);
        owner->~Owner();
        std::free(owner);
      }
    }
  } staged;
  std::vector<Owner *> existingCharges;
  std::uint64_t charge = 0;
  // Workspace allocation is charged as one admission attempt. Container growth
  // remains bounded and allocation exceptions roll back staged references.
  if (!reserve())
    return false;
  try {
    std::unordered_map<Backing *, Owner *> owned;
    std::unordered_set<Backing *> charged, visited;
    std::vector<Backing *> pending{root.backing_};
    std::size_t owners = 0, edges = 0;
    for (auto *owner = ledgerState.head_; owner; owner = owner->next) {
      if (poll())
        return false;
      if (++owners > maxRecursiveDependencies) {
        fault(2);
        return false;
      }
      owned.try_emplace(owner->backing, owner);
      if (owner->recursiveCharged)
        charged.insert(owner->backing);
    }
    while (!pending.empty()) {
      if (poll())
        return false;
      auto *backing = pending.back();
      pending.pop_back();
      if (!visited.insert(backing).second)
        continue;
      if (visited.size() > maxRecursiveDependencies ||
          backing->dependencyCount > maxRecursiveDependencies - edges) {
        fault(2);
        return false;
      }
      edges += backing->dependencyCount;
      if (!charged.contains(backing)) {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (backing->storageBytes > maximum - sizeof(Owner) ||
            backing->storageBytes + sizeof(Owner) > maximum - charge) {
          fault(2);
          return false;
        }
        charge += backing->storageBytes + sizeof(Owner);
        if (auto found = owned.find(backing); found != owned.end())
          existingCharges.push_back(found->second);
      }
      if (!owned.contains(backing)) {
        if (++owners > maxRecursiveDependencies) {
          fault(2);
          return false;
        }
        if (!reserve())
          return false;
        auto *storage = std::malloc(sizeof(Owner));
        if (!storage) {
          fault(2);
          return false;
        }
        if (!retainBacking(backing)) {
          std::free(storage);
          fault(3);
          return false;
        }
        staged.head = new (storage) Owner{staged.head, nullptr, backing, false, true};
      }
      for (std::size_t edge = 0; edge < backing->dependencyCount; ++edge) {
        if (poll())
          return false;
        pending.push_back(backing->dependencies[edge]);
      }
    }
  } catch (const std::bad_alloc &) {
    fault(2);
    return false;
  }
  if (poll())
    return false;
  if (charge > std::numeric_limits<std::uint64_t>::max() - ledgerState.recursiveBytes_ ||
      (charge && ledgerState.recursiveReserve_ && !ledgerState.recursiveReserve_(ledgerState.recursiveBudget_, charge))) {
    fault(2);
    return false;
  }
  if (poll()) {
    if (charge && ledgerState.recursiveRelease_)
      ledgerState.recursiveRelease_(ledgerState.recursiveBudget_, charge);
    return false;
  }
  ledgerState.recursiveBytes_ += charge;
  for (auto *owner : existingCharges)
    owner->recursiveCharged = true;
  // No allocation or fallible operation remains: publish the complete ledger.
  while (staged.head) {
    auto *owner = staged.head;
    staged.head = owner->next;
    attach(owner);
  }
  return true;
}
void Invocation::release(void *buffer) noexcept {
  auto &ledgerState = ledger();
  auto *owner = ledgerState.head_;
  while (owner && owner->backing + 1 != buffer)
    owner = owner->next;
  if (!owner) {
    fault(3);
    return;
  }
  remove(owner);
}
void Invocation::settleResources() noexcept {
  // Private callers have already admitted/materialized this ledger.
  auto &ledgerState = storage_.value;
  if (flags_ & Settled)
    return;
  pollMode_ = 2;
  while (ledgerState.head_)
    remove(ledgerState.head_);
  if (ledgerState.spareSmall_) {
    ledgerState.spareSmall_->~Backing();
    std::free(std::exchange(ledgerState.spareSmall_, nullptr));
  }
  flags_ |= Settled;
  if (ledgerState.environmentBytes_ && ledgerState.environmentRelease_)
    ledgerState.environmentRelease_(ledgerState.environmentBudget_, ledgerState.environmentBytes_);
  ledgerState.environmentBytes_ = 0;
  if (ledgerState.recursiveBytes_ && ledgerState.recursiveRelease_)
    ledgerState.recursiveRelease_(ledgerState.recursiveBudget_, ledgerState.recursiveBytes_);
  ledgerState.recursiveBytes_ = 0;
}
bool Invocation::enterCall(std::int64_t line, std::int64_t column) noexcept {
  initializeControl();
  if (poll())
    return false;
  InvocationABI::cacheStackBounds(*this);
  const auto position = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
  const bool exhausted = position <= stackLow_ || position > stackHigh_ ||
                         callDepth_ == std::numeric_limits<std::uint64_t>::max();
  if (exhausted) {
    fault(6, line, column);
    return false;
  }
  ++callDepth_;
  return true;
}
void Invocation::leaveCall() noexcept {
  initializeControl();
  if (callDepth_)
    --callDepth_;
  else
    fault(3);
}
void Invocation::expectedTaskError(std::int64_t kind, std::int64_t code, std::int64_t line,
                                   std::int64_t column) noexcept {
  auto &ledgerState = ledger();
  if (!ledgerState.taskError_ && !code_ && !ledgerState.applicationFamily_) {
    ledgerState.taskError_ = kind;
    ledgerState.expectedCode_ = code;
    ledgerState.expectedLine_ = line;
    ledgerState.expectedColumn_ = column;
  }
}
void Invocation::expectedApplicationError(const char *family, const char *variant) noexcept {
  auto &ledgerState = ledger();
  if (!code_ && !ledgerState.taskError_ && !ledgerState.applicationFamily_ && family && variant) {
    ledgerState.applicationFamily_ = family;
    ledgerState.applicationVariant_ = variant;
  }
}
[[noreturn]] void Invocation::reportFailure() noexcept {
  auto &ledgerState = ledger();
  if (!code_ && !ledgerState.taskError_ && !ledgerState.applicationFamily_)
    fault(3);
  const char *name = code_ == 1   ? "ShapeFault"
                     : code_ == 2 ? "AllocationFault"
                     : code_ == 4 ? "ArithmeticFault"
                     : code_ == 5 ? "BoundsFault"
                     : code_ == 6 ? "StackExhaustionFault"
                                  : "RuntimeFault";
  if (ledgerState.taskError_)
    name = ledgerState.taskError_ == 1   ? "TaskError.Fault"
           : ledgerState.taskError_ == 2 ? "TaskError.Cancelled"
           : ledgerState.taskError_ == 3 ? "TaskError.DeadlineExceeded"
                             : "TaskError.AdmissionRejected";
  if (ledgerState.applicationFamily_)
    std::fprintf(stderr, "%s.%s\n", ledgerState.applicationFamily_, ledgerState.applicationVariant_);
  else
    std::fprintf(stderr, "%s at source %lld:%lld\n", name,
                 static_cast<long long>(ledgerState.taskError_ ? ledgerState.expectedLine_ : line_),
                 static_cast<long long>(ledgerState.taskError_ ? ledgerState.expectedColumn_ : column_));
  settle();
  if (std::getenv("KALANDRA_TEST_REPORT_CLEANUP"))
    std::fprintf(stderr, "cleanup: live=%llu allocations=%llu releases=%llu\n",
                 static_cast<unsigned long long>(live_.load()),
                 static_cast<unsigned long long>(ledgerState.allocations_),
                 static_cast<unsigned long long>(ledgerState.releases_));
  std::exit(ledgerState.applicationFamily_ ? 89 : ledgerState.taskError_ ? 88 : code_ == 1 ? 87 : 86);
}
} // namespace kalandra::runtime
extern "C" void *kalandra_alloc_v1(kalandra::runtime::Invocation *context,
                                   std::uint64_t bytes) noexcept {
  return context->allocate(bytes);
}
extern "C" void kalandra_free_v1(kalandra::runtime::Invocation *context, void *buffer) noexcept {
  context->release(buffer);
}
extern "C" void kalandra_fault_v1(kalandra::runtime::Invocation *context, std::int64_t code,
                                  std::int64_t line, std::int64_t column) noexcept {
  context->fault(code, line, column);
}
extern "C" std::int64_t kalandra_status_v1(kalandra::runtime::Invocation *context) noexcept {
  return context->executionStatus();
}

extern "C" void *kalandra_environment_alloc_v1(kalandra::runtime::Invocation *context,
                                               std::uint64_t bytes, const void *key) noexcept {
  return context->allocateEnvironment(bytes, key);
}
extern "C" void *kalandra_environment_clone_v1(kalandra::runtime::Invocation *destination,
                                               const kalandra::runtime::Invocation *source,
                                               const void *environment, std::uint64_t bytes,
                                               std::uint64_t alignment, const void *key) noexcept {
  if (!source) {
    if (!destination->poll())
      destination->fault(3);
    return nullptr;
  }
  return destination->cloneEnvironment(*source, environment, bytes, alignment, key);
}
// Captured numeric views can use aligned/offset backing from MLIR rather than
// the canonical packed representation required by inline record fields.
extern "C" bool kalandra_environment_tensor_retain_v1(kalandra::runtime::Invocation *destination,
                                                      const kalandra::runtime::Invocation *source,
                                                      const void *descriptor, std::uint64_t rank,
                                                      std::uint64_t element) noexcept {
  using namespace kalandra;
  if (destination->poll())
    return false;
  const auto invalid = [&] {
    destination->fault(3);
    return false;
  };
  if (!source || !source->usable() || !descriptor || rank > 128 ||
      element > static_cast<unsigned>(NumericType::Float64))
    return invalid();
  const auto type = static_cast<NumericType>(element);
  if (!numericCollectionStorageAvailable(type, NumericTarget::Cpu))
    return invalid();
  const auto *data = static_cast<const std::byte *>(descriptor);
  void *allocated, *aligned;
  std::int64_t offset, sizes[128], strides[128];
  std::memcpy(&allocated, data, sizeof(void *));
  std::memcpy(&aligned, data + sizeof(void *), sizeof(void *));
  std::memcpy(&offset, data + 2 * sizeof(void *), 8);
  std::memcpy(sizes, data + 2 * sizeof(void *) + 8, rank * 8);
  std::memcpy(strides, data + 2 * sizeof(void *) + 8 + rank * 8, rank * 8);
  if (offset < 0)
    return invalid();
  bool empty = false;
  for (std::size_t i = 0; i < rank; ++i) {
    if (sizes[i] < 0 || strides[i] < 0)
      return invalid();
    empty |= sizes[i] == 0;
  }
  if (empty && !allocated)
    return !aligned && offset == 0 ? true : invalid();
  auto lease = source->lease(allocated);
  if (!lease)
    return invalid();
  const auto base = reinterpret_cast<std::uintptr_t>(allocated),
             start = reinterpret_cast<std::uintptr_t>(aligned);
  const auto width = describeNumeric(type)->bytes();
  if (start < base || start % width || start - base > lease->bytes().size())
    return invalid();
  const auto capacity = (lease->bytes().size() - (start - base)) / width;
  auto maximum = static_cast<std::uint64_t>(offset);
  if (empty) {
    if (maximum > capacity)
      return invalid();
  } else {
    if (maximum >= capacity)
      return invalid();
    for (std::size_t i = 0; i < rank; ++i) {
      const auto steps = static_cast<std::uint64_t>(sizes[i] - 1),
                 stride = static_cast<std::uint64_t>(strides[i]);
      if (stride && steps > (capacity - 1 - maximum) / stride)
        return invalid();
      maximum += steps * stride;
    }
  }
  return destination->retainFrom(*source, allocated);
}
extern "C" bool kalandra_retain_v1(kalandra::runtime::Invocation *destination,
                                   const kalandra::runtime::Invocation *source,
                                   void *buffer) noexcept {
  return source && destination->retainFrom(*source, buffer);
}
extern "C" void kalandra_expected_task_error_v1(kalandra::runtime::Invocation *context,
                                                std::int64_t kind, std::int64_t code,
                                                std::int64_t line, std::int64_t column) noexcept {
  context->expectedTaskError(kind, code, line, column);
}

extern "C" bool kalandra_call_enter_v1(kalandra::runtime::Invocation *context, std::int64_t line,
                                       std::int64_t column) noexcept {
  return context->enterCall(line, column);
}
extern "C" void kalandra_call_leave_v1(kalandra::runtime::Invocation *context) noexcept {
  context->leaveCall();
}
extern "C" std::int64_t kalandra_poll_slow_v1(kalandra::runtime::Invocation *context) noexcept {
  return context->poll();
}
extern "C" std::int64_t kalandra_poll_v1(kalandra::runtime::Invocation *context) noexcept {
  return context->poll();
}

extern "C" void *kalandra_record_alloc_v1(kalandra::runtime::Invocation *ctx,
                                          std::uint64_t bytes) noexcept {
  return ctx->allocateRecord(bytes);
}
extern "C" void kalandra_record_cleanup_v1(kalandra::runtime::Invocation *ctx) noexcept {
  ctx->releaseRecords();
}

namespace {
// Private compiler descriptor: two pointers, offset, sizes, strides. Read through
// memcpy so forged metadata cannot introduce unaligned C++ accesses.
bool validRecordStorage(const kalandra::runtime::Invocation &source, const void *descriptor,
                        std::uint64_t rank, std::uint64_t element, void *&allocated) noexcept {
  using namespace kalandra;
  if (!source.usable() || !descriptor || rank > 128 ||
      element > static_cast<unsigned>(NumericType::Float64))
    return false;
  auto numeric = static_cast<NumericType>(element);
  if (!numericCollectionStorageAvailable(numeric, NumericTarget::Cpu))
    return false;
  const auto *data = static_cast<const std::byte *>(descriptor);
  void *aligned;
  std::int64_t offset;
  std::memcpy(&allocated, data, sizeof(void *));
  std::memcpy(&aligned, data + sizeof(void *), sizeof(void *));
  std::memcpy(&offset, data + 2 * sizeof(void *), 8);
  std::int64_t sizes[128], strides[128];
  std::memcpy(sizes, data + 2 * sizeof(void *) + 8, rank * 8);
  std::memcpy(strides, data + 2 * sizeof(void *) + 8 + rank * 8, rank * 8);
  bool empty = false;
  for (std::size_t i = 0; i < rank; ++i) {
    if (sizes[i] < 0)
      return false;
    empty |= sizes[i] == 0;
  }
  if (offset || aligned != allocated)
    return false;
  if (empty) {
    if (allocated)
      return false;
    for (std::size_t i = 0; i < rank; ++i)
      if (strides[i])
        return false;
    return true;
  }
  std::uint64_t count = 1;
  const auto bytes = describeNumeric(numeric)->bytes();
  for (std::size_t i = rank; i > 0; --i) {
    if (strides[i - 1] != static_cast<std::int64_t>(count) ||
        count > (INT64_MAX / bytes) / static_cast<std::uint64_t>(sizes[i - 1]))
      return false;
    count *= sizes[i - 1];
  }
  if (reinterpret_cast<std::uintptr_t>(allocated) % bytes)
    return false;
  auto lease = source.lease(allocated);
  return lease && lease->bytes().size() >= count * bytes;
}
} // namespace
extern "C" void kalandra_record_validate_v1(kalandra::runtime::Invocation *ctx,
                                            const void *descriptor, std::uint64_t rank,
                                            std::uint64_t element) noexcept {
  void *allocated = nullptr;
  if (!validRecordStorage(*ctx, descriptor, rank, element, allocated))
    ctx->fault(3);
}
extern "C" bool kalandra_record_retain_v1(kalandra::runtime::Invocation *destination,
                                          const kalandra::runtime::Invocation *source,
                                          const void *descriptor, std::uint64_t rank,
                                          std::uint64_t element) noexcept {
  void *allocated = nullptr;
  if (!validRecordStorage(*source, descriptor, rank, element, allocated)) {
    destination->fault(3);
    return false;
  }
  if (!destination->usable()) {
    destination->fault(3);
    return false;
  }
  return !allocated || destination->retainFrom(*source, allocated);
}

namespace {
bool validRecordArray(const kalandra::runtime::Invocation &source, const void *descriptor,
                      std::uint64_t cellBytes, std::uint64_t alignment, void *&allocated,
                      std::uint64_t &count) noexcept {
  if (!source.usable() || !descriptor || !cellBytes || !alignment ||
      (alignment & (alignment - 1)) || alignment > alignof(std::max_align_t) ||
      cellBytes % alignment)
    return false;
  const auto *data = static_cast<const std::byte *>(descriptor);
  void *aligned;
  std::int64_t offset, length, stride;
  std::memcpy(&allocated, data, sizeof(void *));
  std::memcpy(&aligned, data + sizeof(void *), sizeof(void *));
  std::memcpy(&offset, data + 2 * sizeof(void *), 8);
  std::memcpy(&length, data + 2 * sizeof(void *) + 8, 8);
  std::memcpy(&stride, data + 2 * sizeof(void *) + 16, 8);
  if (length < 0 || offset || aligned != allocated)
    return false;
  count = static_cast<std::uint64_t>(length);
  if (!count)
    return !allocated && !stride;
  if (stride != 1 || count > INT64_MAX / cellBytes ||
      reinterpret_cast<std::uintptr_t>(allocated) % alignment)
    return false;
  auto lease = source.lease(allocated);
  return lease && lease->bytes().size() >= count * cellBytes;
}
} // namespace
extern "C" bool kalandra_function_array_clone_v1(kalandra::runtime::Invocation *destination,
                                                 void *output,
                                                 const kalandra::runtime::Invocation *source,
                                                 const void *input, std::uint64_t cellBytes,
                                                 std::uint64_t alignment,
                                                 const void *key) noexcept {
  if (destination->poll())
    return false;
  void *allocated = nullptr;
  std::uint64_t count = 0;
  if (!source || !output || !key ||
      !validRecordArray(*source, input, cellBytes, alignment, allocated, count)) {
    destination->fault(3);
    return false;
  }
  struct Descriptor {
    void *allocated, *aligned;
    std::int64_t offset, count, stride;
  };
  static_assert(sizeof(Descriptor) == 40);
  Descriptor result{};
  if (count) {
    auto lease = source->lease(allocated);
    if (!lease) {
      destination->fault(3);
      return false;
    }
    const auto bytes = count * cellBytes;
    auto *buffer = static_cast<std::byte *>(destination->allocateTransportStorage(bytes, key));
    if (!buffer)
      return false;
    for (std::uint64_t offset = 0; offset < bytes; offset += 4096) {
      if (destination->poll())
        return false;
      const auto chunk = std::min<std::uint64_t>(4096, bytes - offset);
      std::memcpy(buffer + offset, lease->bytes().data() + offset, chunk);
    }
    result = {buffer, buffer, 0, static_cast<std::int64_t>(count), 1};
  }
  std::memcpy(output, &result, sizeof result);
  return true;
}
extern "C" void kalandra_record_array_validate_v1(kalandra::runtime::Invocation *context,
                                                  const void *descriptor, std::uint64_t cellBytes,
                                                  std::uint64_t alignment) noexcept {
  void *allocated = nullptr;
  std::uint64_t count = 0;
  if (!validRecordArray(*context, descriptor, cellBytes, alignment, allocated, count))
    context->fault(3);
}
extern "C" bool kalandra_record_array_retain_v1(kalandra::runtime::Invocation *destination,
                                                const kalandra::runtime::Invocation *source,
                                                const void *descriptor, std::uint64_t cellBytes,
                                                std::uint64_t alignment) noexcept {
  void *allocated = nullptr;
  std::uint64_t count = 0;
  if (!destination->usable() ||
      !validRecordArray(*source, descriptor, cellBytes, alignment, allocated, count)) {
    destination->fault(3);
    return false;
  }
  return !allocated || destination->retainFrom(*source, allocated);
}

namespace {
bool validBoolArray(kalandra::runtime::Invocation &destination,
                    const kalandra::runtime::Invocation &source, const void *descriptor,
                    void *&allocated) noexcept {
  std::uint64_t count = 0;
  if (!destination.usable() || !validRecordArray(source, descriptor, 1, 1, allocated, count)) {
    destination.fault(3);
    return false;
  }
  const auto *data = static_cast<const unsigned char *>(allocated);
  for (std::uint64_t i = 0; i < count; ++i) {
    if (!(i & 255) && kalandra_poll_v1(&destination))
      return false;
    if (data[i] > 1) {
      destination.fault(3);
      return false;
    }
  }
  return true;
}
} // namespace
extern "C" void kalandra_bool_array_validate_v1(kalandra::runtime::Invocation *context,
                                                const void *descriptor) noexcept {
  void *allocated = nullptr;
  validBoolArray(*context, *context, descriptor, allocated);
}
extern "C" bool kalandra_bool_array_retain_v1(kalandra::runtime::Invocation *destination,
                                              const kalandra::runtime::Invocation *source,
                                              const void *descriptor) noexcept {
  void *allocated = nullptr;
  if (!validBoolArray(*destination, *source, descriptor, allocated))
    return false;
  return !allocated || destination->retainFrom(*source, allocated);
}

extern "C" void kalandra_expected_application_error_v1(kalandra::runtime::Invocation *context,
                                                       const char *family,
                                                       const char *variant) noexcept {
  context->expectedApplicationError(family, variant);
}

namespace {
bool recursiveRequest(kalandra::runtime::Invocation *context, const void *payload,
                      std::uint64_t bytes, std::uint64_t alignment, const void *key) noexcept {
  if (context->poll() || !context->usable())
    return false;
  if (!payload || !key || !bytes || bytes > std::numeric_limits<std::size_t>::max() || !alignment ||
      alignment > alignof(std::max_align_t) || (alignment & (alignment - 1)) ||
      reinterpret_cast<std::uintptr_t>(payload) % alignment) {
    context->fault(3);
    return false;
  }
  return true;
}
} // namespace
extern "C" const void *kalandra_recursive_make_v1(kalandra::runtime::Invocation *context,
                                                  const void *payload, std::uint64_t bytes,
                                                  std::uint64_t alignment, const void *key,
                                                  const void *const *dependencies,
                                                  std::uint64_t count) noexcept {
  if (!recursiveRequest(context, payload, bytes, alignment, key))
    return nullptr;
  if (count > kalandra::runtime::Invocation::maxRecursiveDependencies || (count && !dependencies)) {
    context->fault(3);
    return nullptr;
  }
  return context->makeRecursiveValueFromPointers(
      {static_cast<const std::byte *>(payload), static_cast<std::size_t>(bytes)}, key,
      {dependencies, static_cast<std::size_t>(count)});
}
extern "C" const void *kalandra_recursive_read_v1(kalandra::runtime::Invocation *context,
                                                  const void *node, std::uint64_t bytes,
                                                  std::uint64_t alignment,
                                                  const void *key) noexcept {
  if (!recursiveRequest(context, node, bytes, alignment, key))
    return nullptr;
  auto lease = context->lease(node);
  if (!lease || !lease->recursiveBytes(key, bytes, alignment)) {
    context->fault(3);
    return nullptr;
  }
  return node;
}

extern "C" const void *kalandra_recursive_transfer_v1(kalandra::runtime::Invocation *destination,
                                                      const kalandra::runtime::Invocation *source,
                                                      const void *root, std::uint64_t bytes,
                                                      std::uint64_t alignment,
                                                      const void *key) noexcept {
  if (destination->poll() || !destination->usable())
    return nullptr;
  if (!source || !source->usable()) {
    destination->fault(3);
    return nullptr;
  }
  auto retained = source->lease(root);
  if (!retained) {
    destination->fault(3);
    return nullptr;
  }
  return destination->retainRecursiveGraph(*retained, key, bytes, alignment) ? root : nullptr;
}

extern "C" const void *
kalandra_recursive_tensor_dependency_v1(kalandra::runtime::Invocation *context,
                                        const void *descriptor, std::uint64_t rank,
                                        std::uint64_t element, std::uint64_t active) noexcept {
  if (context->poll() != 0 || !context->usable())
    return nullptr;
  if (active > 1) {
    context->fault(3);
    return nullptr;
  }
  if (!active)
    return nullptr;
  void *allocated = nullptr;
  if (!validRecordStorage(*context, descriptor, rank, element, allocated)) {
    context->fault(3);
    return nullptr;
  }
  return allocated;
}
extern "C" const void *kalandra_recursive_bool_dependency_v1(kalandra::runtime::Invocation *context,
                                                             const void *descriptor,
                                                             std::uint64_t active) noexcept {
  if (context->poll() != 0 || !context->usable())
    return nullptr;
  if (active > 1) {
    context->fault(3);
    return nullptr;
  }
  if (!active)
    return nullptr;
  void *allocated = nullptr;
  return validBoolArray(*context, *context, descriptor, allocated) ? allocated : nullptr;
}

extern "C" const void *
kalandra_recursive_reference_dependency_v1(kalandra::runtime::Invocation *context, const void *node,
                                           std::uint64_t bytes, std::uint64_t alignment,
                                           const void *key, std::uint64_t active) noexcept {
  if (context->poll() || !context->usable())
    return nullptr;
  if (active > 1) {
    context->fault(3);
    return nullptr;
  }
  return active ? kalandra_recursive_read_v1(context, node, bytes, alignment, key) : nullptr;
}
extern "C" const void *kalandra_recursive_tag_check_v1(kalandra::runtime::Invocation *context,
                                                       std::uint64_t tag, std::uint64_t cases,
                                                       std::uint64_t active) noexcept {
  if (context->poll() || !context->usable())
    return nullptr;
  if (active > 1 || (active && (!tag || tag > cases)))
    context->fault(3);
  return nullptr;
}
extern "C" const void *kalandra_recursive_record_array_dependency_v1(
    kalandra::runtime::Invocation *context, const void *descriptor, std::uint64_t cellBytes,
    std::uint64_t alignment, KalandraRecursiveRecordCell callback, std::uint64_t active) noexcept {
  using namespace kalandra::runtime;
  if (context->poll() || !context->usable())
    return nullptr;
  if (active > 1) {
    context->fault(3);
    return nullptr;
  }
  if (!active)
    return nullptr;
  void *allocated = nullptr;
  std::uint64_t count = 0;
  if (!callback ||
      !validRecordArray(*context, descriptor, cellBytes, alignment, allocated, count)) {
    context->fault(3);
    return nullptr;
  }
  if (!count)
    return nullptr;
  if (count >= Invocation::maxRecursiveDependencies) {
    context->fault(2);
    return nullptr;
  }
  auto **dependencies =
      static_cast<const void **>(context->allocateRecord((count + 1) * sizeof(void *)));
  if (!dependencies)
    return nullptr;
  struct Temporary {
    Invocation *context;
    void *storage;
    ~Temporary() { context->release(storage); }
  } temporary{context, dependencies};
  dependencies[0] = allocated;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (context->poll())
      return nullptr;
    auto *cell = static_cast<const std::byte *>(allocated) + i * cellBytes;
    dependencies[i + 1] = callback(cell, context);
    if (context->poll() || !context->usable())
      return nullptr;
    if (!dependencies[i + 1]) {
      context->fault(3);
      return nullptr;
    }
  }
  // The immutable array backing is retained alongside checked per-element owners.
  static const char key = 0;
  std::array<std::uint64_t, 2> payload{count, cellBytes};
  return context->makeRecursiveValueFromPointers(
      std::as_bytes(std::span{payload}), &key, {dependencies, static_cast<std::size_t>(count + 1)});
}

extern "C" const void *
kalandra_recursive_environment_bind_v1(kalandra::runtime::Invocation *context,
                                       const void *environment, const void *code) noexcept {
  return context->bindRecursiveEnvironment(environment, code);
}
extern "C" const void *
kalandra_recursive_environment_read_v1(kalandra::runtime::Invocation *context,
                                       const void *environment, std::uint64_t bytes,
                                       const void *code) noexcept {
  return context->readRecursiveEnvironment(environment, bytes, code);
}
extern "C" const void *
kalandra_recursive_function_dependency_v1(kalandra::runtime::Invocation *context, const void *code,
                                          const void *environment, std::uint64_t active) noexcept {
  if (context->poll() || !context->usable())
    return nullptr;
  if (active > 1) {
    context->fault(3);
    return nullptr;
  }
  return active ? context->recursiveFunctionDependency(code, environment) : nullptr;
}

extern "C" const void *kalandra_recursive_function_array_dependency_v1(
    kalandra::runtime::Invocation *context, const void *descriptor, std::uint64_t active) noexcept {
  using kalandra::runtime::Invocation;
  if (context->poll() || !context->usable())
    return nullptr;
  if (active > 1) {
    context->fault(3);
    return nullptr;
  }
  if (!active)
    return nullptr;
  struct Cell {
    const void *code, *environment;
  };
  void *allocated = nullptr;
  std::uint64_t size = 0;
  if (!validRecordArray(*context, descriptor, sizeof(Cell), alignof(Cell), allocated, size)) {
    context->fault(3);
    return nullptr;
  }
  if (!size)
    return nullptr;
  // The descriptor and immutable cells belong to this invocation throughout both passes.
  auto backing = context->lease(allocated);
  std::size_t count = 1;
  for (std::uint64_t i = 0; i < size; ++i) {
    if (context->poll())
      return nullptr;
    Cell cell;
    std::memcpy(&cell, backing->bytes().data() + i * sizeof(Cell), sizeof(Cell));
    context->recursiveFunctionDependency(cell.code, cell.environment);
    if (!context->usable())
      return nullptr;
    if (cell.environment && ++count > Invocation::maxRecursiveDependencies) {
      context->fault(2);
      return nullptr;
    }
  }
  auto **dependencies = static_cast<const void **>(context->allocateRecord(count * sizeof(void *)));
  if (!dependencies)
    return nullptr;
  struct Temporary {
    Invocation *context;
    void *storage;
    ~Temporary() { context->release(storage); }
  } temporary{context, dependencies};
  dependencies[0] = allocated;
  std::size_t index = 1;
  for (std::uint64_t i = 0; i < size; ++i) {
    if (context->poll())
      return nullptr;
    Cell cell;
    std::memcpy(&cell, backing->bytes().data() + i * sizeof(Cell), sizeof(Cell));
    if (cell.environment)
      dependencies[index++] = cell.environment;
  }
  static const char key = 0;
  return context->makeRecursiveValueFromPointers(std::as_bytes(std::span{&size, 1}), &key,
                                                 {dependencies, count});
}

extern "C" bool kalandra_invocation_ready_v1(kalandra::runtime::Invocation *context) noexcept {
  return !context->poll() && context->usable();
}

extern "C" const void *
kalandra_recursive_environment_transfer_v1(kalandra::runtime::Invocation *destination,
                                           const kalandra::runtime::Invocation *source,
                                           const void *environment, std::uint64_t bytes,
                                           std::uint64_t alignment, const void *code) noexcept {
  if (destination->poll() || !destination->usable())
    return nullptr;
  if (!source) {
    destination->fault(3);
    return nullptr;
  }
  return destination->retainRecursiveEnvironment(*source, environment, bytes, alignment, code);
}

// Process arguments are immutable once the executable enters user code. They
// remain valid until process exit and can be read concurrently by CPU workers.
namespace {
int processArgumentCount = 0;
char **processArguments = nullptr;
void inputFailure(kalandra::runtime::Invocation *context, std::uint64_t code,
                  const char *message) noexcept {
  context->fault(code);
  std::fprintf(stderr, "%s\n", message);
}
}
extern "C" void kalandra_process_arguments_v1(std::int32_t argc, char **argv) noexcept {
  processArgumentCount = argc;
  processArguments = argv;
}
extern "C" std::int32_t kalandra_args_int32_v1(kalandra::runtime::Invocation *context,
                                              std::int64_t index) noexcept {
  if (context->poll()) return 0;
  if (index < 0 || index >= std::int64_t(processArgumentCount) - 1 || !processArguments) {
    inputFailure(context, 5, "args.int32: missing argument or invalid argument index");
    return 0;
  }
  const std::string_view text(processArguments[index + 1]);
  std::int32_t result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    inputFailure(context, 4, "args.int32: expected a complete, in-range decimal int32 argument");
    return 0;
  }
  return result;
}
extern "C" std::int32_t kalandra_random_int32_v1(kalandra::runtime::Invocation *context,
                                                std::int32_t minimum,
                                                std::int32_t maximumExclusive) noexcept {
  if (context->poll()) return 0;
  if (minimum >= maximumExclusive) {
    inputFailure(context, 5, "random.int32: minimum must be less than maximumExclusive");
    return 0;
  }
  try {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    static std::mt19937 engine(std::random_device{}());
    return std::uniform_int_distribution<std::int32_t>(minimum, maximumExclusive - 1)(engine);
  } catch (...) {
    inputFailure(context, 3, "random.int32: randomness source unavailable");
    return 0;
  }
}
