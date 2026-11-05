#pragma once
#include "DeviceArrayABI.h"
#include <span>
#include <vector>

namespace kalandra::runtime {
// A directory precedes the packed payloads. References are one-based directory
// indices. Each layout is a compiler-generated schema local to the entry.
struct DeviceGraphWireABI {
  static constexpr std::uint64_t OriginMagic = UINT64_C(0x4b414c4f52470001);
  static constexpr std::uint64_t Magic = UINT64_C(0x4b414c4752500001);
  static constexpr std::size_t HeaderWords = 3, DirectoryWords = 2;
};

struct DeviceGraphGuard {
  std::size_t word;
  std::uint64_t tag;
};

struct DeviceGraphCell {
  // A nonzero target layout denotes a reference. A scalar uses the same
  // ceiling for narrow integers, Float32 bits, Bool or the enum tag.
  std::uint64_t maximum;
  std::size_t target = 0;
  bool nullable = false;
  std::uint64_t minimum = 0;
  std::span<const DeviceGraphGuard> guards = {};
};

struct DeviceGraphLayout {
  std::span<const DeviceGraphCell> cells;
  bool singleton;
};

enum class DeviceGraphValidation { Valid, Invalid, Budget, Cancelled };

// Validation precedes materialization. Directory offsets are derived instead
// of trusted from the wire. Traversal visits each reachable allocation once,
// and every active edge is checked even when it targets a previously seen node.
template <class Cancelled>
DeviceGraphValidation validateDeviceGraph(std::span<const std::uint64_t> wire,
                                          std::span<const DeviceGraphLayout> layouts,
                                          std::size_t rootLayout, std::size_t work,
                                          Cancelled cancelled) {
  using V = DeviceGraphValidation;
  using A = DeviceGraphWireABI;
  if (cancelled())
    return V::Cancelled;
  if (wire.size() < A::HeaderWords || wire.size() > DeviceArrayWireABI::MaxWords ||
      wire[0] != A::Magic || !rootLayout || rootLayout > layouts.size())
    return V::Invalid;
  const auto count = wire[1], root = wire[2];
  if (!count || !root || root > count ||
      count > (wire.size() - A::HeaderWords) / A::DirectoryWords)
    return V::Invalid;
  if (count > work)
    return V::Budget;
  struct Node {
    std::size_t layout, begin, end;
    bool seen = false;
  };
  std::vector<Node> nodes;
  nodes.reserve(static_cast<std::size_t>(count));
  std::size_t payload = A::HeaderWords + A::DirectoryWords * static_cast<std::size_t>(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (cancelled())
      return V::Cancelled;
    --work;
    const auto layout = wire[A::HeaderWords + A::DirectoryWords * i];
    const auto elements = wire[A::HeaderWords + A::DirectoryWords * i + 1];
    if (!layout || layout > layouts.size() || !elements)
      return V::Invalid;
    const auto &schema = layouts[layout - 1];
    const auto stride = schema.cells.size();
    if (!stride || (schema.singleton && elements != 1) ||
        elements > (wire.size() - payload) / stride)
      return V::Invalid;
    const auto end = payload + static_cast<std::size_t>(elements) * stride;
    nodes.push_back({static_cast<std::size_t>(layout), payload, end});
    payload = end;
  }
  if (payload != wire.size() || nodes[root - 1].layout != rootLayout)
    return V::Invalid;
  std::vector<std::size_t> pending{static_cast<std::size_t>(root - 1)};
  nodes[root - 1].seen = true;
  for (std::size_t at = 0; at < pending.size(); ++at) {
    const auto &node = nodes[pending[at]];
    const auto cells = layouts[node.layout - 1].cells;
    for (auto element = node.begin; element < node.end; element += cells.size()) {
      for (std::size_t word = 0; word < cells.size(); ++word) {
        if (cancelled())
          return V::Cancelled;
        if (!work)
          return V::Budget;
        --work;
        const auto &cell = cells[word];
        bool active = true;
        for (const auto &guard : cell.guards) {
          if (cancelled())
            return V::Cancelled;
          if (!work)
            return V::Budget;
          --work;
          if (guard.word >= word)
            return V::Invalid;
          if (wire[element + guard.word] != guard.tag) {
            active = false;
            break;
          }
        }
        const auto value = wire[element + word];
        if (!active) {
          if (value)
            return V::Invalid;
        } else if (!cell.target) {
          if (value > cell.maximum || value < cell.minimum)
            return V::Invalid;
        } else if (!value) {
          if (!cell.nullable)
            return V::Invalid;
        } else {
          if (value > count || nodes[value - 1].layout != cell.target)
            return V::Invalid;
          if (!nodes[value - 1].seen) {
            nodes[value - 1].seen = true;
            pending.push_back(static_cast<std::size_t>(value - 1));
          }
        }
      }
    }
  }
  return pending.size() == nodes.size() ? V::Valid : V::Invalid;
}
} // namespace kalandra::runtime
