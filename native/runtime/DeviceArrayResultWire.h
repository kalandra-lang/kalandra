#pragma once
#include "DeviceArrayABI.h"
#include "Tensor.h"
#include <cstring>
#include <optional>
namespace kalandra::runtime {
// Private terminal-wire decoder. A captured origin retains its original native
// descriptor; fresh origin zero receives fresh backing. Source computations and
// heap traversal have already finished on the device. Decode only transports
// typed bytes, in cancellable pages, and never publishes a partial native array.
// Like capture frames, this object and its embedded scope must not move and
// must be destroyed before their Invocation.
template <NumericType Element, bool Boolean = false> class DeviceArrayResultDecode final {
public:
  static_assert(!Boolean || Element == NumericType::UInt8);
  using Descriptor = CpuTensor<Element, 1>;
  using Wire = CpuTensor<NumericType::UInt64, 1>;
  DeviceArrayResultDecode() = default;
  DeviceArrayResultDecode(const DeviceArrayResultDecode &) = delete;
  DeviceArrayResultDecode &operator=(const DeviceArrayResultDecode &) = delete;
  ~DeviceArrayResultDecode() = default;
  template <class Resolve> bool initialize(Invocation &context, const Wire &wire, Resolve resolve) {
    auto retained = retainWire(context, wire, DeviceArrayWireABI::HeaderWords);
    if (!retained)
      return false;
    const auto *words = reinterpret_cast<const std::uint64_t *>(retained->data().data());
    const auto length = words[DeviceArrayWireABI::Length];
    const auto origin = words[DeviceArrayWireABI::Origin];
    if (words[DeviceArrayWireABI::Version] != DeviceArrayWireABI::ResultMagic ||
        words[DeviceArrayWireABI::Layout] != DeviceArrayWireABI::layout(Element, Boolean) ||
        length > DeviceArrayWireABI::MaxWords - DeviceArrayWireABI::HeaderWords ||
        std::uint64_t(wire.sizes[0]) != DeviceArrayWireABI::HeaderWords + (origin ? 0 : length)) {
      context.fault(3);
      return false;
    }
    return beginRegion(context, std::move(*retained),
                       origin ? 0 : DeviceArrayWireABI::HeaderWords, length, origin, resolve);
  }
  // Decode one typed region from a larger, separately validated word envelope.
  // The region remains private and holds its backing through every copy page.
  template <class Resolve>
  bool initializeRegion(Invocation &context, const Wire &wire, std::uint64_t offset,
                        std::uint64_t length, std::uint64_t origin, Resolve resolve) {
    auto retained = retainWire(context, wire, 0);
    return retained &&
           beginRegion(context, std::move(*retained), offset, length, origin, resolve);
  }
  bool advance() noexcept {
    if (!context_ || context_->poll())
      return false;
    if (complete_)
      return true;
    if (!origin_ && !result_.allocated) {
      scope_.emplace(*context_);
      if (!scope_->valid())
        return false;
      Invocation::OwnershipSelection selection(*scope_);
      auto *storage = static_cast<NumericValue<Element> *>(
          context_->allocate(length_ * sizeof(NumericValue<Element>)));
      if (!storage)
        return false;
      result_ = Descriptor{storage, storage, 0, {std::int64_t(length_)}, {1}};
      return false; // Allocation and payload copy are separate bounded steps.
    }
    const auto end =
        cursor_ + std::min<std::uint64_t>(DeviceArrayWireABI::PageWords, length_ - cursor_);
    const auto *words = reinterpret_cast<const std::uint64_t *>(wire_->data().data());
    for (; cursor_ < end; ++cursor_) {
      if (context_->poll())
        return false;
      const auto word = origin_ ? std::uint64_t(std::to_integer<unsigned>(alias_->data()[cursor_]))
                                : words[payloadOffset_ + cursor_];
      if constexpr (Boolean) {
        if (word > 1) {
          context_->fault(3);
          return false;
        }
      } else if constexpr (sizeof(NumericValue<Element>) < sizeof(std::uint64_t)) {
        if (word >> (8 * sizeof(NumericValue<Element>))) {
          context_->fault(3);
          return false;
        }
      }
      if (!origin_)
        std::memcpy(result_.aligned + cursor_, &word, sizeof(NumericValue<Element>));
    }
    complete_ = cursor_ == length_;
    return complete_;
  }
  bool complete() const noexcept { return context_ && complete_ && !context_->poll(); }
  std::optional<Descriptor> result() const noexcept {
    if (!complete())
      return {};
    return result_;
  }
  std::uint64_t decodedElements() const noexcept { return cursor_; }
  std::uint64_t origin() const noexcept { return origin_; }

private:
  std::optional<RetainedCpuTensor> retainWire(Invocation &context, const Wire &wire,
                                           std::size_t minimum) const noexcept {
    if (context_ || context.poll()) {
      if (context_)
        context.fault(3);
      return {};
    }
    if (wire.sizes[0] < std::int64_t(minimum) ||
        std::uint64_t(wire.sizes[0]) > DeviceArrayWireABI::MaxWords || wire.strides[0] != 1) {
      context.fault(3);
      return {};
    }
    auto retained = retainCpuTensor(context, wire.view());
    if (!retained)
      context.fault(3);
    return retained;
  }
  template <class Resolve>
  bool beginRegion(Invocation &context, RetainedCpuTensor retained, std::uint64_t offset,
                   std::uint64_t length, std::uint64_t origin, Resolve &resolve) {
    const auto words = retained.bytes() / sizeof(std::uint64_t);
    if (length > DeviceArrayWireABI::MaxWords || length > SIZE_MAX / sizeof(NumericValue<Element>) ||
        origin > UINT32_MAX || (origin ? offset != 0 : offset > words || length > words - offset)) {
      context.fault(3);
      return false;
    }
    if (origin) {
      auto original = resolve(origin);
      if (!original || original->sizes[0] != std::int64_t(length) ||
          (length && original->strides[0] != 1)) {
        context.fault(3);
        return false;
      }
      auto lease = retainCpuTensor(context, original->view());
      if (!lease) {
        context.fault(3);
        return false;
      }
      alias_.emplace(std::move(*lease));
      result_ = *original;
    }
    context_ = &context;
    wire_.emplace(std::move(retained));
    payloadOffset_ = offset;
    origin_ = origin;
    length_ = length;
    complete_ = !length || (origin && !Boolean);
    return true;
  }
  Invocation *context_ = nullptr;
  std::optional<RetainedCpuTensor> wire_, alias_;
  std::optional<Invocation::OwnershipScope> scope_;
  Descriptor result_{};
  std::uint64_t length_ = 0, origin_ = 0, cursor_ = 0, payloadOffset_ = 0;
  bool complete_ = false;
};
} // namespace kalandra::runtime
