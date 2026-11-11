#pragma once
#include "Tensor.h"
#include <atomic>
#include <mutex>
namespace kalandra::runtime {
// Private common representation for compiler-owned opaque host capabilities.
struct OwnedDescriptor {
  CpuTensor<NumericType::UInt8, 1> storage;
};
enum class OwnedKind { File, ByteBuffer };
struct ResourceScope;
struct OwnedState {
  std::mutex mutex;
  ResourceScope *scope = nullptr;
  AllocationLease scopeOwner;
  OwnedState *previous = nullptr, *next = nullptr;
  std::atomic<bool> retired{false};
  std::atomic<std::uint64_t> additionalBytes{0};
  OwnedKind kind;
  std::uint64_t bytes;
  void (*destroy)(void *) noexcept;
  void (*retire)(void *) noexcept;
  OwnedState(OwnedKind kind, std::uint64_t bytes, void (*destroy)(void *) noexcept,
             void (*retire)(void *) noexcept) noexcept
      : kind(kind), bytes(bytes), destroy(destroy), retire(retire) {}
  ~OwnedState();
  void detach() noexcept;
  static void cleanup(void *p) noexcept {
    auto *s = static_cast<OwnedState *>(p);
    s->destroy(p);
  }
  static std::uint64_t cost(const void *p) noexcept {
    return static_cast<const OwnedState *>(p)->additionalBytes.load(std::memory_order_acquire);
  }
};
struct ResourceScope {
  OwnedState *first = nullptr;
  ResourceScope *parent = nullptr;
  AllocationLease parentOwner;
  bool active = true;
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
