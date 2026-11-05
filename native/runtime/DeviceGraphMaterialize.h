#pragma once
#include "DeviceGraphPack.h"
#include "Runtime.h"

namespace kalandra::runtime {
// Decode an acyclic graph into native owned backing. The caller selects its
// private result scope and publishes the result only after this call succeeds.
inline DeviceGraphValidation materializeDeviceGraph(
    Invocation &context, std::span<const std::uint64_t> wire,
    std::span<const DeviceGraphLayout> layouts,
    std::span<const DeviceGraphNativeLayout> nativeLayouts,
    std::span<const void *const> keys, std::size_t rootLayout,
    std::span<std::byte> result, std::size_t byteLimit, std::size_t work) {
  using V = DeviceGraphValidation;
  using A = DeviceGraphWireABI;
  const auto cancelled = [&] { return context.poll() != 0; };
  if (layouts.size() != nativeLayouts.size() || layouts.size() != keys.size())
    return V::Invalid;
  struct Node {
    std::size_t layout, begin, words, elements;
    const void *pointer = nullptr;
    unsigned state = 0;
  };
  struct Rollback {
    Invocation &context;
    std::vector<Node> &nodes;
    bool committed = false;
    ~Rollback() {
      if (!committed)
        for (auto &node : nodes)
          if (node.pointer)
            context.release(const_cast<void *>(node.pointer));
    }
  };
  try {
    const auto validated = validateDeviceGraph(wire, layouts, rootLayout, work, cancelled);
    if (validated != V::Valid)
      return validated;
    const auto count = static_cast<std::size_t>(wire[1]);
    const auto root = static_cast<std::size_t>(wire[2] - 1);
    std::vector<Node> nodes;
    nodes.reserve(count);
    std::size_t begin = A::HeaderWords + A::DirectoryWords * count;
    for (std::size_t i = 0; i < count; ++i) {
      if (cancelled())
        return V::Cancelled;
      const auto directory = A::HeaderWords + A::DirectoryWords * i;
      const auto layout = static_cast<std::size_t>(wire[directory] - 1);
      const auto elements = static_cast<std::size_t>(wire[directory + 1]);
      const auto &native = nativeLayouts[layout];
      if (!keys[layout] || !native.stride || native.cells.size() != layouts[layout].cells.size())
        return V::Invalid;
      if (elements > byteLimit / native.stride)
        return V::Budget;
      byteLimit -= elements * native.stride;
      const auto words = elements * native.cells.size();
      nodes.push_back({layout, begin, words, elements});
      begin += words;
    }
    if (!layouts[rootLayout - 1].singleton ||
        result.size() != nativeLayouts[rootLayout - 1].stride)
      return V::Invalid;

    struct Frame {
      std::size_t node, next;
    };
    std::vector<Frame> stack{{root, 0}};
    std::vector<std::size_t> order;
    nodes[root].state = 1;
    while (!stack.empty()) {
      if (cancelled())
        return V::Cancelled;
      if (!work)
        return V::Budget;
      --work;
      auto &frame = stack.back();
      auto &node = nodes[frame.node];
      if (frame.next == node.words) {
        node.state = 2;
        order.push_back(frame.node);
        stack.pop_back();
        continue;
      }
      const auto word = frame.next++;
      const auto cells = layouts[node.layout].cells;
      const auto reference = wire[node.begin + word];
      if (!cells[word % cells.size()].target || !reference)
        continue;
      auto &child = nodes[reference - 1];
      if (child.state == 1)
        return V::Invalid; // Immutable CPU backing requires dependencies before publication.
      if (!child.state) {
        child.state = 1;
        stack.push_back({static_cast<std::size_t>(reference - 1), 0});
      }
    }

    Rollback rollback{context, nodes};
    for (auto id : order) {
      if (cancelled())
        return V::Cancelled;
      auto &node = nodes[id];
      const auto &native = nativeLayouts[node.layout];
      const auto cells = layouts[node.layout].cells;
      std::vector<std::byte> payload(node.elements * native.stride);
      std::vector<const void *> dependencies;
      for (std::size_t word = 0; word < node.words; ++word) {
        if (cancelled())
          return V::Cancelled;
        if (!work)
          return V::Budget;
        --work;
        const auto index = word % cells.size();
        const auto &field = native.cells[index];
        const auto &rule = cells[index];
        if (field.offset > native.stride || field.bytes > native.stride - field.offset)
          return V::Invalid;
        auto *destination = payload.data() + word / cells.size() * native.stride + field.offset;
        const auto value = wire[node.begin + word];
        if (!rule.target) {
          if (field.bytes > sizeof(value) || (!field.bytes && value))
            return V::Invalid;
          if (field.bytes)
            std::memcpy(destination, &value, field.bytes);
        } else if (value) {
          const auto &child = nodes[value - 1];
          if (layouts[child.layout].singleton) {
            if (field.bytes != sizeof(child.pointer))
              return V::Invalid;
            std::memcpy(destination, &child.pointer, sizeof(child.pointer));
          } else {
            struct Descriptor {
              const void *allocated, *aligned;
              std::int64_t offset, count, stride;
            };
            const Descriptor descriptor{child.pointer, child.pointer, 0,
                                        static_cast<std::int64_t>(child.elements), 1};
            if (field.bytes != sizeof(descriptor))
              return V::Invalid;
            std::memcpy(destination, &descriptor, sizeof(descriptor));
          }
          if (dependencies.size() == Invocation::maxRecursiveDependencies)
            return V::Budget;
          dependencies.push_back(child.pointer);
        }
      }
      node.pointer = context.makeRecursiveValueFromPointers(payload, keys[node.layout], dependencies);
      if (!node.pointer)
        return context.status() ? V::Budget : V::Cancelled;
    }
    if (cancelled())
      return V::Cancelled;
    std::memcpy(result.data(), nodes[root].pointer, result.size());
    context.release(const_cast<void *>(nodes[root].pointer));
    rollback.committed = true;
    return V::Valid;
  } catch (const std::bad_alloc &) {
    return V::Budget;
  }
}
} // namespace kalandra::runtime
