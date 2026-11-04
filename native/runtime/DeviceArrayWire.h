#pragma once
#include "DeviceArrayABI.h"
#include "Tensor.h"
#include <cstring>
#include <optional>
namespace kalandra::runtime {
// The caller owns the language-level read borrow. This retains backing and
// copies at most one 64 KiB wire page per advance, with cooperative cancellation.
// It is not a replacement for the frontend's task/alias access proof.
// Its embedded ownership scope has a stable address: capture frames cannot move.
// Must be destroyed before its Invocation, like HostArrayScopes.
template <NumericType Element, bool Boolean = false> class DeviceArrayCapture final {
public:
  static_assert(!Boolean || Element == NumericType::UInt8);
  using Descriptor = CpuTensor<Element, 1>;
  DeviceArrayCapture() = default;
  DeviceArrayCapture(const DeviceArrayCapture &) = delete;
  DeviceArrayCapture &operator=(const DeviceArrayCapture &) = delete;
  DeviceArrayCapture(DeviceArrayCapture &&) = delete;
  DeviceArrayCapture &operator=(DeviceArrayCapture &&) = delete;
  ~DeviceArrayCapture() { reset(); }
  bool initialize(Invocation &context, const Descriptor &source, std::uint64_t origin) noexcept {
    if (context_) {
      context.fault(3);
      return false;
    }
    if (context.poll())
      return false;
    if (!origin || origin > UINT32_MAX || source.sizes[0] < 0 ||
        (source.sizes[0] && source.strides[0] != 1)) {
      context.fault(3);
      return false;
    }
    const auto length = std::uint64_t(source.sizes[0]);
    if (length > DeviceArrayWireABI::MaxWords - DeviceArrayWireABI::HeaderWords ||
        length + DeviceArrayWireABI::HeaderWords > SIZE_MAX / sizeof(std::uint64_t)) {
      context.fault(2);
      return false;
    }
    auto retained = retainCpuTensor(context, source.view());
    if (!retained) {
      if (!context.poll())
        context.fault(3);
      return false;
    }
    scope_.emplace(context);
    if (!scope_->valid())
      return false;
    context_ = &context;
    retained_.emplace(std::move(*retained));
    original_ = source;
    origin_ = origin;
    return true;
  }
  // False means either more work, a recorded context fault, or cancellation.
  // A stopped capture never exposes partially packed cells through wire().
  bool advance() noexcept {
    if (!context_ || context_->poll())
      return false;
    const auto length = std::uint64_t(original_.sizes[0]);
    if (!storage_) {
      Invocation::OwnershipSelection selection(*scope_);
      storage_ = static_cast<std::uint64_t *>(
          context_->allocate((length + DeviceArrayWireABI::HeaderWords) * sizeof(std::uint64_t)));
      if (!storage_)
        return false;
      storage_[DeviceArrayWireABI::Version] = DeviceArrayWireABI::Magic;
      storage_[DeviceArrayWireABI::Layout] = DeviceArrayWireABI::layout(Element, Boolean);
      storage_[DeviceArrayWireABI::Length] = length;
      storage_[DeviceArrayWireABI::Origin] = origin_;
      return !length; // Header initialization is its own bounded step.
    }
    const auto data = retained_->data();
    const auto end =
        cursor_ + std::min<std::uint64_t>(DeviceArrayWireABI::PageWords, length - cursor_);
    for (; cursor_ < end; ++cursor_) {
      if (context_->poll())
        return false;
      std::uint64_t word = 0;
      std::memcpy(&word, data.data() + cursor_ * sizeof(NumericValue<Element>),
                  sizeof(NumericValue<Element>));
      if constexpr (Boolean) {
        if (word > 1) {
          context_->fault(3);
          return false;
        }
      }
      storage_[DeviceArrayWireABI::HeaderWords + cursor_] = word;
    }
    return cursor_ == length;
  }
  bool complete() const noexcept {
    return context_ && storage_ && cursor_ == std::uint64_t(original_.sizes[0]) &&
           !context_->poll();
  }
  std::optional<CpuTensor<NumericType::UInt64, 1>> wire() const noexcept {
    if (!complete())
      return {};
    return CpuTensor<NumericType::UInt64, 1>{
        storage_,
        storage_,
        0,
        {original_.sizes[0] + std::int64_t(DeviceArrayWireABI::HeaderWords)},
        {1}};
  }
  const Descriptor &original() const noexcept { return original_; }
  std::uint64_t origin() const noexcept { return origin_; }
  std::uint64_t packedElements() const noexcept { return cursor_; }

private:
  void reset() noexcept {
    scope_.reset();
    storage_ = nullptr;
    context_ = nullptr;
    retained_.reset();
  }
  Invocation *context_ = nullptr;
  std::optional<Invocation::OwnershipScope> scope_;
  std::optional<RetainedCpuTensor> retained_;
  Descriptor original_{};
  std::uint64_t *storage_ = nullptr;
  std::uint64_t origin_ = 0, cursor_ = 0;
};
} // namespace kalandra::runtime
