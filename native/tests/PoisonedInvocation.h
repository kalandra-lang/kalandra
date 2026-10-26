#pragma once
#include "../runtime/Runtime.h"
#include <cstddef>
#include <cstring>
#include <new>

// Force source-entry tests through storage containing stale nonzero bytes.
// The runtime must inspect its live tag before accessing deferred state.
class PoisonedInvocation final {
  alignas(kalandra::runtime::Invocation)
      std::byte storage_[sizeof(kalandra::runtime::Invocation)];
  kalandra::runtime::Invocation *value_;
public:
  PoisonedInvocation() {
    std::memset(storage_, 0xa5, sizeof(storage_));
    value_ = ::new (storage_) kalandra::runtime::Invocation;
  }
  ~PoisonedInvocation() { value_->~Invocation(); }
  PoisonedInvocation(const PoisonedInvocation &) = delete;
  PoisonedInvocation &operator=(const PoisonedInvocation &) = delete;
  kalandra::runtime::Invocation &get() noexcept { return *value_; }
};
