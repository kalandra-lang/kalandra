#pragma once
#include "DeviceGraphWire.h"
#include <algorithm>
#include <cstring>
#include <expected>
#include <map>
#include <tuple>

namespace kalandra::runtime {
struct DeviceGraphNativeCell {
  DeviceGraphCell wire;
  std::size_t offset, bytes;
};

struct DeviceGraphNativeLayout {
  std::span<const DeviceGraphNativeCell> cells;
  std::size_t stride;
  bool singleton;
};

// The caller retains the root and every resolved view throughout packing.
// Views have a canonical native element stride. An empty array uses no node.
struct DeviceGraphNativeView {
  std::size_t layout, elements;
  std::span<const std::byte> data;
};

template <class Resolve, class Cancelled>
std::expected<std::vector<std::uint64_t>, DeviceGraphValidation>
packDeviceGraph(DeviceGraphNativeView root, std::span<const DeviceGraphNativeLayout> layouts,
                std::size_t wordLimit, std::size_t work, Resolve resolve, Cancelled cancelled) {
  using A = DeviceGraphWireABI;
  using V = DeviceGraphValidation;
  using Id = std::expected<std::uint64_t, V>;
  using Key = std::tuple<std::uintptr_t, std::size_t, std::size_t>;
  std::map<Key, std::uint64_t> ids;
  std::vector<DeviceGraphNativeView> nodes;
  std::vector<std::uint64_t> payload;
  wordLimit = std::min<std::size_t>(wordLimit, DeviceArrayWireABI::MaxWords);
  if (cancelled())
    return std::unexpected(V::Cancelled);
  if (wordLimit < A::HeaderWords)
    return std::unexpected(V::Budget);
  std::size_t words = A::HeaderWords;
  const auto intern = [&](DeviceGraphNativeView view) -> Id {
    if (!view.layout || view.layout > layouts.size())
      return std::unexpected(V::Invalid);
    const auto &layout = layouts[view.layout - 1];
    if (!view.elements || !layout.stride || layout.cells.empty() ||
        (layout.singleton && view.elements != 1) ||
        view.elements != view.data.size() / layout.stride ||
        view.data.size() % layout.stride)
      return std::unexpected(V::Invalid);
    const Key key{reinterpret_cast<std::uintptr_t>(view.data.data()), view.layout, view.elements};
    if (const auto found = ids.find(key); found != ids.end())
      return found->second;
    const auto available = wordLimit - words;
    if (available < A::DirectoryWords ||
        view.elements > (available - A::DirectoryWords) / layout.cells.size())
      return std::unexpected(V::Budget);
    const auto id = nodes.size() + 1;
    ids.emplace(key, id);
    nodes.push_back(view);
    words += A::DirectoryWords + view.elements * layout.cells.size();
    return id;
  };
  auto rootId = intern(root);
  if (!rootId)
    return std::unexpected(rootId.error());
  for (std::size_t at = 0; at < nodes.size(); ++at) {
    const auto node = nodes[at]; // Resolving a reference may grow the directory.
    const auto &layout = layouts[node.layout - 1];
    for (std::size_t element = 0; element < node.elements; ++element) {
      const auto bytes = node.data.subspan(element * layout.stride, layout.stride);
      const auto start = payload.size();
      for (std::size_t word = 0; word < layout.cells.size(); ++word) {
        if (cancelled())
          return std::unexpected(V::Cancelled);
        if (!work)
          return std::unexpected(V::Budget);
        --work;
        const auto &cell = layout.cells[word];
        bool active = true;
        for (const auto &guard : cell.wire.guards) {
          if (cancelled())
            return std::unexpected(V::Cancelled);
          if (!work)
            return std::unexpected(V::Budget);
          --work;
          if (guard.word >= word)
            return std::unexpected(V::Invalid);
          if (payload[start + guard.word] != guard.tag) {
            active = false;
            break;
          }
        }
        std::uint64_t value = 0;
        if (active && cell.bytes) {
          if (cell.offset > bytes.size() || cell.bytes > bytes.size() - cell.offset)
            return std::unexpected(V::Invalid);
          const auto field = bytes.subspan(cell.offset, cell.bytes);
          if (cell.wire.target) {
            auto view = resolve(cell.wire.target, field);
            if (!view)
              return std::unexpected(view.error());
            if (view->layout != cell.wire.target)
              return std::unexpected(V::Invalid);
            if (view->elements) {
              auto id = intern(*view);
              if (!id)
                return std::unexpected(id.error());
              value = *id;
            } else if (!cell.wire.nullable || !view->data.empty()) {
              return std::unexpected(V::Invalid);
            }
          } else {
            if (cell.bytes > sizeof(value))
              return std::unexpected(V::Invalid);
            std::memcpy(&value, field.data(), field.size());
            if (value < cell.wire.minimum || value > cell.wire.maximum)
              return std::unexpected(V::Invalid);
          }
        } else if (active && (cell.wire.target || cell.wire.minimum)) {
          return std::unexpected(V::Invalid);
        }
        payload.push_back(value);
      }
    }
  }
  std::vector<std::uint64_t> wire;
  wire.reserve(words);
  wire.insert(wire.end(), {A::Magic, nodes.size(), *rootId});
  for (const auto &node : nodes) {
    if (cancelled())
      return std::unexpected(V::Cancelled);
    wire.insert(wire.end(), {node.layout, node.elements});
  }
  for (std::size_t at = 0; at < payload.size();) {
    if (cancelled())
      return std::unexpected(V::Cancelled);
    const auto end = at + std::min(DeviceArrayWireABI::PageWords, payload.size() - at);
    wire.insert(wire.end(), payload.begin() + at, payload.begin() + end);
    at = end;
  }
  return wire;
}
} // namespace kalandra::runtime
