#pragma once
#include "DeviceGraphPack.h"
#include "DeviceGraphArena.h"
#include "DeviceGraphOrigins.h"
#include "DeviceArrayWire.h"
#include "Tasks.h"
#include <functional>
#include <new>
#include <tuple>
#include <type_traits>

namespace kalandra::runtime {
struct DeviceGraphCaptureLayout {
  std::size_t stride, alignment;
  const void *boxKey; // Nominal recursive payload key; null for other view kinds.
  const void *environmentKey = nullptr; // Native closure code identity, never serialized.
  bool inlineValue = false; // A carrier embedded in an already-retained parent view.
};

// Holds every backing used by the packer. This runs while the source invocation
// has exclusive access; the resulting wire contains only copied payload words.
class DeviceGraphCapture final {
public:
  DeviceGraphCapture(const Invocation &source, std::span<const DeviceGraphCaptureLayout> layouts)
      : source_(source), layouts_(layouts) {}

  std::expected<DeviceGraphNativeView, DeviceGraphValidation>
  operator()(std::size_t id, std::span<const std::byte> field) {
    using V = DeviceGraphValidation;
    if (!id || id > layouts_.size())
      return std::unexpected(V::Invalid);
    const auto &layout = layouts_[id - 1];
    if (!layout.stride || !layout.alignment || (layout.alignment & (layout.alignment - 1)) ||
        layout.stride % layout.alignment)
      return std::unexpected(V::Invalid);
    if (layout.inlineValue) {
      if (layout.boxKey || layout.environmentKey || field.size() != layout.stride ||
          reinterpret_cast<std::uintptr_t>(field.data()) % layout.alignment)
        return std::unexpected(V::Invalid);
      return DeviceGraphNativeView{id, 1, field};
    }
    if (layout.boxKey || layout.environmentKey) {
      if (layout.boxKey && layout.environmentKey)
        return std::unexpected(V::Invalid);
      const void *pointer = nullptr;
      if (field.size() != sizeof(pointer))
        return std::unexpected(V::Invalid);
      std::memcpy(&pointer, field.data(), sizeof(pointer));
      const auto *lease = retain(pointer);
      if (!lease)
        return std::unexpected(V::Invalid);
      auto bytes = layout.environmentKey
                       ? lease->environmentBytes(layout.environmentKey, layout.stride,
                                                 layout.alignment)
                       : lease->recursiveBytes(layout.boxKey, layout.stride, layout.alignment);
      if (!bytes)
        return std::unexpected(V::Invalid);
      return DeviceGraphNativeView{id, 1, *bytes, pointer, pointer, 0};
    }
    struct Descriptor {
      const void *allocated, *aligned;
      std::int64_t offset, count, stride;
    };
    static_assert(sizeof(Descriptor) == 40);
    if (field.size() != sizeof(Descriptor))
      return std::unexpected(V::Invalid);
    Descriptor array{};
    std::memcpy(&array, field.data(), sizeof(array));
    if (array.count < 0 || array.offset < 0 || (array.count && array.stride != 1))
      return std::unexpected(V::Invalid);
    if (!array.count)
      return DeviceGraphNativeView{id, 0, {}};
    const auto *lease = retain(array.allocated);
    if (!lease)
      return std::unexpected(V::Invalid);
    const auto bytes = lease->bytes();
    const auto base = reinterpret_cast<std::uintptr_t>(bytes.data());
    const auto aligned = reinterpret_cast<std::uintptr_t>(array.aligned);
    if (aligned < base || aligned % layout.alignment || aligned - base > bytes.size())
      return std::unexpected(V::Invalid);
    auto start = aligned - base;
    if (std::uint64_t(array.offset) > (bytes.size() - start) / layout.stride)
      return std::unexpected(V::Invalid);
    start += std::size_t(array.offset) * layout.stride;
    if (std::uint64_t(array.count) > (bytes.size() - start) / layout.stride)
      return std::unexpected(V::Invalid);
    return DeviceGraphNativeView{id, static_cast<std::size_t>(array.count),
                                 bytes.subspan(start, std::size_t(array.count) * layout.stride),
                                 array.allocated, array.aligned, array.offset};
  }

private:
  const AllocationLease *retain(const void *pointer) {
    if (auto found = leases_.find(pointer); found != leases_.end())
      return &found->second;
    auto lease = source_.lease(pointer);
    if (!lease)
      return nullptr;
    return &leases_.emplace(pointer, std::move(*lease)).first->second;
  }

  const Invocation &source_;
  std::span<const DeviceGraphCaptureLayout> layouts_;
  std::map<const void *, AllocationLease> leases_;
};
// All generated native capture descriptors are trivially destructible.
// The invocation scope owns the dependencies copied by the member layouts.
template <std::uint64_t Identity, class... Captures>
const ValueLayout &deviceGraphCaptureTupleLayout() noexcept {
  using Tuple = std::tuple<typename Captures::Record...>;
  static_assert(std::is_trivially_destructible_v<Tuple>);
  static const ValueLayout layout{
      Identity, sizeof(Tuple), alignof(Tuple),
      [](Invocation &destination, void *output, const Invocation &source,
         const void *input) noexcept {
        const auto &values = *static_cast<const Tuple *>(input);
        auto *copy = new (output) Tuple{};
        const auto retain = [&]<std::size_t... I>(std::index_sequence<I...>) {
          return (Captures::layout().copy(destination, &std::get<I>(*copy), source,
                                          &std::get<I>(values)) && ...);
        };
        return retain(std::index_sequence_for<Captures...>{});
      },
      [](void *) noexcept {}};
  return layout;
}

template <class Policy> struct DeviceGraphArgument {
  static constexpr bool graph = true;
  using Descriptor = typename Policy::Record;
  struct Capture {
    DeviceArrayCapture<NumericType::UInt64> transfer;
    std::vector<DeviceGraphOrigin> origins;
    auto origin() const noexcept { return transfer.origin(); }
    auto wire() const noexcept { return transfer.wire(); }
    bool advance() noexcept { return transfer.advance(); }
    void retireWire() noexcept { transfer.retireWire(); }
  };
  static const ValueLayout &layout() noexcept { return Policy::layout(); }

  static bool prepare(Invocation &context, Capture &capture, const Descriptor &value,
                      std::uint64_t origin, bool preserveOrigins) noexcept {
    using V = DeviceGraphValidation;
    using W = DeviceArrayWireABI;
    const auto fail = [&](V error) {
      if (error != V::Cancelled && !context.poll())
        context.fault(error == V::Budget ? 2 : 3);
      return false;
    };
    try {
      // Bound temporary packing work and the resulting transfer separately.
      constexpr std::size_t limit = 1 << 20, work = 8 * limit;
      const auto cancelled = [&] { return context.poll() != 0; };
      DeviceGraphCapture resolver(context, Policy::captureLayouts);
      std::vector<DeviceGraphNativeView> views;
      constexpr bool resolved = requires { Policy::captureViews(value, resolver); };
      constexpr bool joined = resolved || requires { Policy::captureViews(value); };
      auto *originViews = preserveOrigins ? &views : nullptr;
      auto wire = [&]() -> std::expected<std::vector<std::uint64_t>, V> {
        if constexpr (resolved) {
          const auto roots = Policy::captureViews(value, resolver);
          if (!roots)
            return std::unexpected(roots.error());
          return packDeviceGraphCaptures(*roots, Policy::nativeLayouts, Policy::root, limit,
                                         work, std::ref(resolver), cancelled, originViews);
        } else if constexpr (joined) {
          const auto roots = Policy::captureViews(value);
          return packDeviceGraphCaptures(roots, Policy::nativeLayouts, Policy::root, limit,
                                         work, std::ref(resolver), cancelled, originViews);
        } else {
          return packDeviceGraph({Policy::root, 1, std::as_bytes(std::span(&value, 1))},
                                 Policy::nativeLayouts, limit, work, std::ref(resolver),
                                 cancelled, originViews);
        }
      }();
      if (!wire)
        return fail(wire.error());
      std::vector<std::uint64_t> handles;
      auto image = makeDeviceGraphArena(*wire, Policy::wireLayouts, Policy::arenaLayouts,
                                        Policy::root, limit, work, cancelled,
                                        preserveOrigins ? &handles : nullptr, joined);
      if (!image)
        return fail(image.error());
      auto origins = makeDeviceGraphOrigins(views, handles, Policy::arenaLayouts);
      if (!origins)
        return fail(origins.error());
      const auto roots = joined ? std::span<const std::uint64_t>(image->captureRoots)
                                : std::span<const std::uint64_t>(&image->root, 1);
      if (joined && roots.size() > limit - image->words.size())
        return fail(V::Budget);
      const auto count = image->words.size() + roots.size();
      auto *data = static_cast<std::uint64_t *>(context.allocate(count * sizeof(std::uint64_t)));
      if (!data)
        return false;
      std::ranges::copy(roots, data);
      for (std::size_t at = 0; at < image->words.size();) {
        if (cancelled()) {
          context.release(data);
          return false;
        }
        const auto words = std::min(W::PageWords, image->words.size() - at);
        std::memcpy(data + roots.size() + at, image->words.data() + at, words * sizeof(std::uint64_t));
        at += words;
      }
      const bool ready = capture.transfer.initialize(context,
          {data, data, 0, {static_cast<std::int64_t>(count)}, {1}}, origin);
      context.release(data);
      if (ready)
        capture.origins = std::move(*origins);
      return ready;
    } catch (const std::bad_alloc &) {
      return fail(V::Budget);
    }
  }
};
} // namespace kalandra::runtime
