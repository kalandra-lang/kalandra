#pragma once
#include "DeviceGraphArena.h"
#include "DeviceGraphPack.h"
#include <set>

namespace kalandra::runtime {
struct DeviceGraphOrigin {
  std::uint64_t handle;
  unsigned arenaLayout;
  std::size_t elements, stride;
  const void *owner, *aligned;
  std::int64_t offset;
  std::span<const std::byte> bytes;
  std::vector<std::byte> tensorDescriptor{}, tensorWords{};
};

// Root records live inline and have no native backing identity of their own.
// All other origin entries are tied to the retained source capture.
inline std::expected<std::vector<DeviceGraphOrigin>, DeviceGraphValidation>
makeDeviceGraphOrigins(std::span<const DeviceGraphNativeView> views,
                       std::span<const std::uint64_t> handles,
                       std::span<const unsigned> arenaLayouts) {
  using V = DeviceGraphValidation;
  if (views.size() != handles.size())
    return std::unexpected(V::Invalid);
  std::vector<DeviceGraphOrigin> origins;
  for (std::size_t i = 0; i < views.size(); ++i) {
    const auto &view = views[i];
    if (!view.owner && view.tensorDescriptor.empty())
      continue;
    if (!view.layout || view.layout > arenaLayouts.size() || !view.elements ||
        view.data.empty() || view.data.size() % view.elements ||
        !handles[i] || (i && handles[i] <= handles[i - 1]))
      return std::unexpected(V::Invalid);
    origins.push_back({handles[i], arenaLayouts[view.layout - 1], view.elements,
                       view.data.size() / view.elements, view.owner, view.aligned,
                       view.offset, view.tensorDescriptor.empty() ? view.data
                                                                  : std::span<const std::byte>{},
                       {view.tensorDescriptor.begin(), view.tensorDescriptor.end()},
                       view.tensorDescriptor.empty() ? std::vector<std::byte>{}
                           : std::vector<std::byte>{view.data.begin(), view.data.end()}});
  }
  return origins;
}

inline const DeviceGraphOrigin *
findDeviceGraphOrigin(std::span<const DeviceGraphOrigin> origins,
                      std::uint64_t handle, unsigned arenaLayout, std::size_t elements) {
  const auto found = std::ranges::lower_bound(origins, handle, {}, &DeviceGraphOrigin::handle);
  if (found == origins.end() || found->handle != handle ||
      found->arenaLayout != arenaLayout || found->elements != elements)
    return nullptr;
  return &*found;
}
struct DeviceGraphOriginEnvelope {
  static constexpr std::uint64_t Magic = DeviceGraphWireABI::OriginMagic;
  std::span<const std::uint64_t> graph;
  std::vector<const DeviceGraphOrigin *> nodes;
};

// The sidecar is indexed by graph node ID. Zero identifies freshly allocated
// device backing. Only exact registered capture views may be reused.
template <class Cancelled>
std::expected<DeviceGraphOriginEnvelope, DeviceGraphValidation>
decodeDeviceGraphOrigins(std::span<const std::uint64_t> wire,
                         std::span<const DeviceGraphLayout> layouts,
                         std::span<const unsigned> arenaLayouts, std::size_t rootLayout,
                         std::span<const DeviceGraphOrigin> origins, std::size_t work,
                         Cancelled cancelled) {
  using V = DeviceGraphValidation;
  using G = DeviceGraphWireABI;
  if (cancelled())
    return std::unexpected(V::Cancelled);
  if (wire.size() < 2 || wire[0] != DeviceGraphOriginEnvelope::Magic ||
      wire[1] > wire.size() - 2 || layouts.size() != arenaLayouts.size())
    return std::unexpected(V::Invalid);
  const auto count = static_cast<std::size_t>(wire[1]);
  if (count > work)
    return std::unexpected(V::Budget);
  auto graph = wire.subspan(2 + count);
  const auto valid = validateDeviceGraph(graph, layouts, rootLayout, work - count, cancelled);
  if (valid != V::Valid)
    return std::unexpected(valid);
  if (count != graph[1] || wire[1 + graph[2]])
    return std::unexpected(V::Invalid);
  DeviceGraphOriginEnvelope envelope{graph, {}};
  envelope.nodes.reserve(count);
  std::set<std::uint64_t> seen;
  for (std::size_t i = 0; i < count; ++i) {
    if (cancelled())
      return std::unexpected(V::Cancelled);
    const auto handle = wire[2 + i];
    if (!handle) {
      envelope.nodes.push_back(nullptr);
      continue;
    }
    const auto directory = G::HeaderWords + G::DirectoryWords * i;
    const auto layout = graph[directory] - 1;
    const auto elements = static_cast<std::size_t>(graph[directory + 1]);
    const auto *origin = findDeviceGraphOrigin(origins, handle, arenaLayouts[layout], elements);
    if (!origin || !seen.insert(handle).second)
      return std::unexpected(V::Invalid);
    envelope.nodes.push_back(origin);
  }
  return envelope;
}
// Check that a returned captured node still describes the same native bytes
// and edges. Never use a graph payload to overwrite native captured backing.
template <class Cancelled>
DeviceGraphValidation verifyDeviceGraphOriginContents(
    const DeviceGraphOriginEnvelope &envelope,
    std::span<const DeviceGraphNativeLayout> native, std::size_t work, Cancelled cancelled) {
  using V = DeviceGraphValidation;
  using G = DeviceGraphWireABI;
  struct Descriptor {
    const void *owner, *aligned;
    std::int64_t offset, count, stride;
  };
  const auto graph = envelope.graph;
  const auto count = envelope.nodes.size();
  std::size_t payload = G::HeaderWords + G::DirectoryWords * count;
  for (std::size_t i = 0; i < count; ++i) {
    if (cancelled())
      return V::Cancelled;
    const auto directory = G::HeaderWords + G::DirectoryWords * i;
    const auto layout = graph[directory] - 1;
    if (layout >= native.size())
      return V::Invalid;
    const auto elements = static_cast<std::size_t>(graph[directory + 1]);
    const auto &schema = native[layout];
    const auto *origin = envelope.nodes[i];
    if (origin) {
      if (origin->stride != schema.stride ||
          (schema.tensorRank >= 0) != !origin->tensorDescriptor.empty())
        return V::Invalid;
      const auto backing = schema.tensorRank >= 0 ? std::span<const std::byte>(origin->tensorWords)
                                                   : origin->bytes;
      if (!schema.stride || backing.size() / schema.stride != elements ||
          backing.size() % schema.stride)
        return V::Invalid;
      for (std::size_t element = 0; element < elements; ++element) {
        const auto words = graph.subspan(payload + element * schema.cells.size(), schema.cells.size());
        const auto data = backing.subspan(element * schema.stride, schema.stride);
        for (std::size_t word = 0; word < schema.cells.size(); ++word) {
          if (cancelled())
            return V::Cancelled;
          if (!work)
            return V::Budget;
          --work;
          const auto &cell = schema.cells[word];
          bool active = true;
          for (const auto &guard : cell.wire.guards) {
            if (!work)
              return V::Budget;
            --work;
            if (words[guard.word] != guard.tag) {
              active = false;
              break;
            }
          }
          if (!active)
            continue;
          if (cell.offset > data.size() || cell.bytes > data.size() - cell.offset)
            return V::Invalid;
          const auto *field = data.data() + cell.offset;
          const auto value = words[word];
          if (!cell.wire.target) {
            std::uint64_t bits = 0;
            if (cell.bytes > sizeof(bits))
              return V::Invalid;
            if (cell.bytes)
              std::memcpy(&bits, field, cell.bytes);
            if (bits != value)
              return V::Invalid;
          } else {
            if (cell.wire.target > native.size())
              return V::Invalid;
            const auto *child = value ? envelope.nodes[value - 1] : nullptr;
            if (value && !child)
              return V::Invalid;
            const auto &target = native[cell.wire.target - 1];
            if (target.tensorRank >= 0) {
              if (!child || child->tensorDescriptor.size() != cell.bytes ||
                  !std::ranges::equal(child->tensorDescriptor, std::span(field, cell.bytes)))
                return V::Invalid;
            } else if (target.singleton) {
              const void *pointer = nullptr;
              if (!child || cell.bytes != sizeof(pointer))
                return V::Invalid;
              std::memcpy(&pointer, field, sizeof(pointer));
              if (pointer != child->owner)
                return V::Invalid;
            } else {
              Descriptor descriptor{};
              if (cell.bytes != sizeof(descriptor))
                return V::Invalid;
              std::memcpy(&descriptor, field, sizeof(descriptor));
              if (child ? (descriptor.owner != child->owner || descriptor.aligned != child->aligned ||
                           descriptor.offset != child->offset || descriptor.stride != 1 ||
                           descriptor.count != static_cast<std::int64_t>(child->elements))
                        : descriptor.count != 0)
                return V::Invalid;
            }
          }
        }
      }
    }
    payload += elements * schema.cells.size();
  }
  return V::Valid;
}
} // namespace kalandra::runtime
