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
  bool advance(Invocation &context, Result &value, std::span<const DeviceGraphOrigin> origins) {
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
    const auto payload = words.subspan(W::HeaderWords);
    constexpr std::size_t budget = 8 * 1024 * 1024;
    const auto cancelled = [&] { return context.poll() != 0; };
    V status;
    try {
      if (!payload.empty() && payload[0] == DeviceGraphOriginEnvelope::Magic) {
        const auto envelope = decodeDeviceGraphOrigins(payload, Policy::wireLayouts,
            Policy::arenaLayouts, Policy::root, origins, budget, cancelled);
        status = envelope ? verifyDeviceGraphOriginContents(*envelope, Policy::nativeLayouts,
                                                             budget, cancelled) : envelope.error();
        if (status == V::Valid)
          status = materializeDeviceGraph(context, envelope->graph, Policy::wireLayouts,
              Policy::nativeLayouts, Policy::keys, Policy::root,
              std::as_writable_bytes(std::span(&value, 1)), budget, budget, envelope->nodes);
      } else
        status = materializeDeviceGraph(context, payload, Policy::wireLayouts, Policy::nativeLayouts,
            Policy::keys, Policy::root, std::as_writable_bytes(std::span(&value, 1)), budget, budget);
    } catch (const std::bad_alloc &) {
      status = V::Budget;
    }
    if (status == V::Valid)
      return true;
    if (!context.poll())
      context.fault(status == V::Budget ? 2 : 3);
    return false;
  }
};
} // namespace kalandra::runtime
