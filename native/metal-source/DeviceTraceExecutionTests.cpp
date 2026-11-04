// Executes exactly the emitted device collector IR, lowered to CPU for an
// independent graph oracle. Physical GPU qualification is a separate gate.
#include <cstdint>
namespace kalandra {
struct DeviceHeapEmitter {
  enum { PC = 0, Capacity = 1, Phase = 4, Cells = 24 };
};
} // namespace kalandra
#include <algorithm>
#include <array>
#include <iostream>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>
using namespace kalandra;
template <class T> struct Memref {
  T *allocated, *aligned;
  std::int64_t offset, size, stride;
};
extern "C" void _mlir_ciface_step(Memref<std::uint64_t> *, Memref<std::uint32_t> *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Heap {
  std::vector<std::uint64_t> data{0};
  std::array<std::uint32_t, 52> frame{};
  std::vector<std::uint64_t> handles;
  Heap() {
    frame[0] = 1;
    frame.back() = UINT32_MAX;
  }
  void cell(unsigned i, std::uint64_t v) {
    frame[1 + 2 * i] = v;
    frame[2 + 2 * i] = v >> 32;
  }
  std::uint64_t cell(unsigned i) const {
    return frame[1 + 2 * i] | (std::uint64_t(frame[2 + 2 * i]) << 32);
  }
  std::uint64_t block(unsigned layout, std::initializer_list<std::uint64_t> payload) {
    auto h = data.size();
    handles.push_back(h);
    data.insert(data.end(), {4 + payload.size(), layout, payload.size(), 1});
    data.insert(data.end(), payload);
    return h;
  }
  void step() {
    Memref<std::uint64_t> heap{data.data(), data.data(), 0, std::int64_t(data.size()), 1};
    Memref<std::uint32_t> state{frame.data(), frame.data(), 0, 52, 1};
    _mlir_ciface_step(&heap, &state);
  }
  void collect(std::uint64_t root, bool malformed = false) {
    cell(DeviceHeapEmitter::PC, 1);
    cell(DeviceHeapEmitter::Capacity, data.size());
    cell(DeviceHeapEmitter::Cells, root);
    cell(DeviceHeapEmitter::Phase, 2);
    cell(5, 1); // Collector admission starts at the first physical block.
    std::set<unsigned> phases;
    for (unsigned i = 0; i < 100000; ++i) {
      phases.insert(cell(DeviceHeapEmitter::Phase));
      step();
      if (frame.back() != UINT32_MAX) {
        if (!malformed || frame.back() != 99) {
          std::cerr << "fault=" << frame.back() << " phase=" << cell(DeviceHeapEmitter::Phase)
                    << " cursor=" << cell(5) << " root=" << root << " trace=" << cell(21)
                    << " element=" << cell(22) << " field=" << cell(23) << '\n';
          throw std::runtime_error("unexpected collection metadata fault");
        }
        return;
      }
      if (cell(DeviceHeapEmitter::Phase) == 1) {
        require(!malformed && phases.contains(2) && phases.contains(3) && phases.contains(4) &&
                    phases.contains(5),
                "missing bounded collection phase");
        return;
      }
    }
    throw std::runtime_error("collector failed to terminate within graph-derived test bound");
  }
};
int main() {
  try {
    std::mt19937 random(0x279281);
#ifdef KALANDRA_TRACE_PHYSICAL
    constexpr unsigned trials = 20, maxNodes = 24;
#else
    constexpr unsigned trials = 300, maxNodes = 120;
#endif
    for (unsigned trial = 0; trial < trials; ++trial) {
      Heap heap;
      const unsigned count = 1 + random() % maxNodes;
      for (unsigned i = 0; i < count; ++i)
        heap.block(12, {0, 0, 0});
      std::vector<std::vector<unsigned>> edges(count);
      for (unsigned i = 0; i < count; ++i) {
        const auto h = heap.handles[i];
        const unsigned tag = random() % 3;
        heap.data[h + 4] = tag;
        for (unsigned field = 1; field <= 2; ++field) {
          auto target = random() % count;
          const bool active = tag == 2 || (tag == 1 && field == 1);
          // Inactive enum payload deliberately contains a poison handle.
          heap.data[h + 4 + field] = active ? heap.handles[target] : UINT64_MAX;
          if (active)
            edges[i].push_back(target);
        }
      }
      std::set<unsigned> reachable;
      std::queue<unsigned> pending;
      if (trial % 3)
        pending.push(0);
      while (!pending.empty()) {
        auto i = pending.front();
        pending.pop();
        if (!reachable.insert(i).second)
          continue;
        for (auto child : edges[i])
          pending.push(child);
      }
      heap.collect(trial % 3 ? heap.handles[0] : 0);
      for (unsigned i = 0; i < count; ++i) {
        const bool live = reachable.contains(i);
        // Coalescing leaves interior obsolete headers untouched: inspect the
        // physical span covering the original block, never stale header bits.
        std::uint64_t h = 1;
        while (h + heap.data[h] <= heap.handles[i])
          h += heap.data[h];
        require((heap.data[h + 1] != 0) == live,
                "reachability differs from independent graph oracle");
      }
    }
    { // Repeated elements and aliases into a cycle.
      Heap h;
      auto leaf = h.block(1, {42});
      auto root = h.block(12, {2, leaf, 0, 1, leaf, UINT64_MAX});
      h.data[root + 6] = root;
      h.collect(root);
      require(h.data[leaf + 1] == 1 && h.data[root + 1] == 12, "array stride/cycle trace failed");
      h.collect(0);
      require(h.data[2] == 0 && h.data[1] == h.data.size() - 1, "dead cycle was not reclaimed");
    }
    {
      Heap h;
      auto root = h.block(12, {1, UINT64_MAX, 0});
      h.collect(root, true);
    }
    {
      Heap h;
      auto root = h.block(12, {1, 0});
      h.collect(root, true);
    }
    {
      Heap h;
      auto root = h.block(99, {0});
      h.collect(root, true);
    }
    {
      Heap h;
      auto leaf = h.block(1, {7});
      auto chain = h.block(13, {leaf, leaf});
      h.collect(chain);
      require(h.data[leaf + 1] == 1, "unconditional reference traversal failed");
    }
    {
      Heap h;
      auto empty = h.block(13, {});
      h.collect(empty);
      require(h.data[empty + 1] == 13, "empty owned array was lost");
    }
    std::cout << trials
              << " independent graph oracles, cycles, aliases, enum guards, repeated elements "
                 "and malformed metadata passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
