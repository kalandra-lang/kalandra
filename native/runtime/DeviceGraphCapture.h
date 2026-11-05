#pragma once
#include "DeviceGraphPack.h"
#include "DeviceGraphArena.h"
#include "DeviceArrayWire.h"
#include "Tasks.h"
#include <functional>
#include <new>

namespace kalandra::runtime {
struct DeviceGraphCaptureLayout {
  std::size_t stride, alignment;
  const void *boxKey; // Null for array descriptors.
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
    if (layout.boxKey) {
      const void *pointer = nullptr;
      if (field.size() != sizeof(pointer))
        return std::unexpected(V::Invalid);
      std::memcpy(&pointer, field.data(), sizeof(pointer));
      const auto *lease = retain(pointer);
      if (!lease)
        return std::unexpected(V::Invalid);
      auto bytes = lease->recursiveBytes(layout.boxKey, layout.stride, layout.alignment);
      if (!bytes)
        return std::unexpected(V::Invalid);
      return DeviceGraphNativeView{id, 1, *bytes};
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
                                 bytes.subspan(start, std::size_t(array.count) * layout.stride)};
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
template <class Policy> struct DeviceGraphArgument {
  static constexpr bool graph = true;
  using Descriptor = typename Policy::Record;
  using Capture = DeviceArrayCapture<NumericType::UInt64>;
  static const ValueLayout &layout() noexcept { return Policy::layout(); }

  static bool prepare(Invocation &context, Capture &capture, const Descriptor &value,
                      std::uint64_t origin) noexcept {
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
      auto wire = packDeviceGraph({Policy::root, 1, std::as_bytes(std::span(&value, 1))},
                                  Policy::nativeLayouts, limit, work, std::ref(resolver), cancelled);
      if (!wire)
        return fail(wire.error());
      auto image = makeDeviceGraphArena(*wire, Policy::wireLayouts, Policy::arenaLayouts,
                                        Policy::root, limit, work, cancelled);
      if (!image)
        return fail(image.error());
      const auto count = image->words.size() + 1;
      auto *data = static_cast<std::uint64_t *>(context.allocate(count * sizeof(std::uint64_t)));
      if (!data)
        return false;
      data[0] = image->root;
      for (std::size_t at = 0; at < image->words.size();) {
        if (cancelled()) {
          context.release(data);
          return false;
        }
        const auto words = std::min(W::PageWords, image->words.size() - at);
        std::memcpy(data + 1 + at, image->words.data() + at, words * sizeof(std::uint64_t));
        at += words;
      }
      const bool ready = capture.initialize(context,
          {data, data, 0, {static_cast<std::int64_t>(count)}, {1}}, origin);
      context.release(data);
      return ready;
    } catch (const std::bad_alloc &) {
      return fail(V::Budget);
    }
  }
};
} // namespace kalandra::runtime
