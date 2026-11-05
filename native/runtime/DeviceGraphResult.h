#pragma once
#include "DeviceGraphMaterialize.h"
#include "Tensor.h"

namespace kalandra::runtime {
template <class Policy> class DeviceGraphResultDecode {
  std::optional<RetainedCpuTensor> retained_;

public:
  using Result = typename Policy::Record;
  using Wire = CpuTensor<NumericType::UInt64, 1>;
  bool initialize(Invocation &context, const Wire &wire) noexcept {
    using W = DeviceArrayWireABI;
    if (context.poll())
      return false;
    auto retained = retainCpuTensor(context, wire.view());
    if (retained_ || !retained || wire.sizes[0] < std::int64_t(W::HeaderWords) ||
        std::uint64_t(wire.sizes[0]) > W::MaxWords) {
      context.fault(3);
      return false;
    }
    const auto *words = reinterpret_cast<const std::uint64_t *>(retained->data().data());
    if (words[W::Version] != W::ResultMagic ||
        words[W::Layout] != W::layout(NumericType::UInt64, false) ||
        words[W::Origin] || words[W::Length] != std::uint64_t(wire.sizes[0]) - W::HeaderWords) {
      context.fault(3);
      return false;
    }
    retained_.emplace(std::move(*retained));
    return true;
  }
  bool advance(Invocation &context, Result &value) {
    using W = DeviceArrayWireABI;
    using V = DeviceGraphValidation;
    if (context.poll())
      return false;
    if (!retained_) {
      context.fault(3);
      return false;
    }
    const auto bytes = retained_->data();
    const auto words = std::span(reinterpret_cast<const std::uint64_t *>(bytes.data()), bytes.size()/8);
    const auto status = materializeDeviceGraph(context, words.subspan(W::HeaderWords),
        Policy::wireLayouts, Policy::nativeLayouts, Policy::keys, Policy::root,
        std::as_writable_bytes(std::span(&value, 1)), 8 * 1024 * 1024, 8 * 1024 * 1024);
    if (status == V::Valid)
      return true;
    if (!context.poll())
      context.fault(status == V::Budget ? 2 : 3);
    return false;
  }
};
} // namespace kalandra::runtime
