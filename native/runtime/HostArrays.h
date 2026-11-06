#pragma once
#include "Tensor.h"
#include "ValueScopes.h"
#include <cstring>
namespace kalandra::runtime {
// Mutable host arrays cross the GPU supervisor as native descriptors, never as
// immutable device buffers. The invocation owns retained backing, not this POD.
template <NumericType Element, bool Boolean = false> const ValueLayout &hostArrayLayout() noexcept {
  static_assert(!Boolean || Element == NumericType::UInt8);
  using Descriptor = CpuTensor<Element, 1>;
  static_assert(sizeof(Descriptor) == 40);
  static const ValueLayout layout{
      Boolean ? UINT64_C(0x4b414c4241520000)
              : UINT64_C(0x4b414c4e41520000) + static_cast<unsigned>(Element),
      sizeof(Descriptor), alignof(Descriptor),
      [](Invocation &destination, void *out, const Invocation &source, const void *input) noexcept {
        if (!out || !input) {
          destination.fault(3);
          return false;
        }
        const bool retained =
            Boolean ? kalandra_bool_array_retain_v1(&destination, &source, input)
                    : kalandra_environment_tensor_retain_v1(&destination, &source, input, 1,
                                                            static_cast<unsigned>(Element));
        if (!retained || destination.poll())
          return false;
        std::memcpy(out, input, sizeof(Descriptor));
        return true;
      },
      [](void *) noexcept {}};
  return layout;
}
template <NumericType Element, bool Boolean = false>
const ValueLayout &hostScalarLayout() noexcept {
  using Scalar = NumericValue<Element>;
  static_assert(!Boolean || Element == NumericType::UInt8);
  static const ValueLayout layout{
      UINT64_C(0x4b414c5343410000) + static_cast<unsigned>(Element) + (Boolean ? 256 : 0),
      sizeof(Scalar), alignof(Scalar),
      [](Invocation &destination, void *out, const Invocation &, const void *input) noexcept {
        if (!out || !input || destination.poll())
          return false;
        if constexpr (Boolean) {
          if (*static_cast<const std::uint8_t *>(input) > 1) {
            destination.fault(3);
            return false;
          }
        }
        std::memcpy(out, input, sizeof(Scalar));
        return true;
      },
      [](void *) noexcept {}};
  return layout;
}
class HostArrayScopes final {
public:
  HostArrayScopes() = default;
  HostArrayScopes(const HostArrayScopes &) = delete;
  HostArrayScopes &operator=(const HostArrayScopes &) = delete;
  ~HostArrayScopes() {
    if (scopes_)
      scopes_->destroy();
  }
  bool initialize(Invocation &context, std::span<const ValueLayout *const> layouts) noexcept {
    if (scopes_) {
      context.fault(3);
      return false;
    }
    scopes_ = ValueScopes::create(context, layouts);
    return scopes_ != nullptr;
  }
  bool begin() noexcept { return scopes_ && scopes_->begin(); }
  void end() noexcept {
    if (scopes_)
      scopes_->end();
  }
  bool clear(std::size_t slot) noexcept { return scopes_ && scopes_->clear(slot); }
  bool discard(std::size_t slot) noexcept { return scopes_ && scopes_->discard(slot); }
  bool commit(std::size_t slot, void *value) noexcept {
    return scopes_ && scopes_->commit(slot, value);
  }
  bool collectBegin(std::size_t slot, const ValueLayout &element, const ValueLayout &array,
                    bool recordStorage) noexcept {
    return scopes_ && scopes_->collectBegin(slot, element, array, recordStorage);
  }
  bool collectAppend(std::size_t slot, const void *value) noexcept {
    return scopes_ && scopes_->collectAppend(slot, value);
  }
  bool collectFinish(std::size_t slot, void *value) noexcept {
    return scopes_ && scopes_->collectFinish(slot, value);
  }
  bool collectDiscard(std::size_t slot) noexcept {
    return scopes_ && scopes_->collectDiscard(slot);
  }
  bool capture(Invocation &context, std::size_t slot, const ValueLayout &layout, void *out,
               const Invocation &source, const void *input) noexcept {
    if (!begin())
      return false;
    if (!layout.copy(context, out, source, input)) {
      end();
      return false;
    }
    return commit(slot, out);
  }

private:
  ValueScopes *scopes_ = nullptr;
};
class HostArrayWork final {
public:
  explicit HostArrayWork(HostArrayScopes &scopes) noexcept
      : scopes_(scopes), ready_(scopes.begin()) {}
  ~HostArrayWork() { scopes_.end(); }
  explicit operator bool() const noexcept { return ready_; }
  HostArrayWork(const HostArrayWork &) = delete;
  HostArrayWork &operator=(const HostArrayWork &) = delete;

private:
  HostArrayScopes &scopes_;
  bool ready_;
};
inline void hostArrayFailure(Task &task) noexcept {
  // An earlier checked fault wins over a simultaneous cancellation request.
  auto &context = task.context();
  if (context.status())
    task.fault(context.status(), context.line(), context.column());
  else if (!task.cancelled())
    task.fault(3);
}
} // namespace kalandra::runtime
