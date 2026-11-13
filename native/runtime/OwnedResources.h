#pragma once
#include "Tensor.h"
#include <atomic>
#include <mutex>
namespace kalandra::runtime {
// Private common representation for compiler-owned opaque host capabilities.
struct OwnedDescriptor {
  CpuTensor<NumericType::UInt8, 1> storage;
};
enum class OwnedKind { File, ByteBuffer, Generator };
class BorrowedGroupOwner;
struct BorrowedCleanup;
struct ResourceScope;
struct OwnedState {
  std::mutex mutex;
  ResourceScope *scope = nullptr;
  AllocationLease scopeOwner;
  // Only detached retirement work owns this pin. Live scope links are weak:
  // losing the last value/job owner must not retain an otherwise dead resource.
  AllocationLease scopePin;
  OwnedState *retirementNext = nullptr;
  OwnedState *previous = nullptr, *next = nullptr;
  std::atomic<bool> retired{false};
  // Non-null only for resumable producers with asynchronous owned cleanup.
  BorrowedGroupOwner *deferredOwner = nullptr;
  const BorrowedCleanup *deferredCleanup = nullptr;
  OwnedKind kind;
  std::uint64_t bytes;
  void (*destroy)(void *) noexcept;
  void (*retire)(void *) noexcept;
  OwnedState(OwnedKind kind, std::uint64_t bytes, void (*destroy)(void *) noexcept,
             void (*retire)(void *) noexcept) noexcept
      : kind(kind), bytes(bytes), destroy(destroy), retire(retire) {}
  ~OwnedState();
  // Caller holds resourceMutex(), so a linked state cannot finish detaching.
  std::optional<AllocationLease> pinForRetirement() noexcept;
  void detach() noexcept;
  static void cleanup(void *p) noexcept {
    auto *s = static_cast<OwnedState *>(p);
    s->destroy(p);
  }
};
struct ResourceControl;
struct ResourceScope {
  Invocation *context = nullptr;
  ResourceControl *control = nullptr;
  std::atomic<bool> hasDeferred{false};
  BorrowedGroupOwner *cleanupRoot = nullptr;
  OwnedState *first = nullptr;
  ResourceScope *parent = nullptr;
  AllocationLease parentOwner;
  bool active = true;
  bool retiring = false;
  bool escapePrepared = false;
  static void cleanup(void *p) noexcept { static_cast<ResourceScope *>(p)->~ResourceScope(); }
};
struct ResourceControl {
  Invocation::OwnershipScope storage;
  ResourceScope *scope = nullptr;
  explicit ResourceControl(Invocation &ctx) noexcept : storage(ctx) {}
  ~ResourceControl();
  static void cleanup(void *p) noexcept { static_cast<ResourceControl *>(p)->~ResourceControl(); }
};
std::mutex &resourceMutex();
ResourceControl *currentScope(Invocation &) noexcept;
void adopt(Invocation &, OwnedState &, ResourceScope *) noexcept;
OwnedDescriptor ownedDescriptor(OwnedState *) noexcept;
OwnedState *inspectOwned(Invocation &, const OwnedDescriptor &) noexcept;
} // namespace kalandra::runtime
extern "C" {
void *kalandra_file_scope_enter_v1(kalandra::runtime::Invocation *,
                                   const kalandra::runtime::OwnedDescriptor *const *,
                                   std::uint64_t) noexcept;
void kalandra_file_scope_leave_v1(kalandra::runtime::Invocation *, void *,
                                  const kalandra::runtime::OwnedDescriptor *) noexcept;
bool kalandra_file_receive_v1(kalandra::runtime::Invocation *,
                              const kalandra::runtime::OwnedDescriptor *) noexcept;
}
