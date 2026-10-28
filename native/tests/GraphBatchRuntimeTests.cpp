#include "../metal-proof/FloatOracle.h"
#include "../metal/Metal.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Graph batching check %d: %s\n", __LINE__, #x);                         \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
void empty(m::Runtime &runtime, std::optional<std::uint64_t> baseline = {}) {
  auto s = runtime.stats();
  CHECK(s.bytes == baseline.value_or(0) && !s.inFlight && !s.stagingBuffers && !s.argumentBytes &&
        !s.argumentPacks);
  if (!baseline)
    CHECK(!s.metalBuffers && !s.hostBackings && !s.deviceBackings);
}
void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.retainedOwners && !s.retainedBytes && !s.externalPending);
}
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *v = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *v;
  v->~Buffer();
  return result;
}
m::Buffer upload(m::Runtime &r, std::span<const std::uint32_t> bits,
                 std::array<std::int64_t, 2> shape) {
  auto host = r.hostTensor(std::as_bytes(bits), shape, m::Format::Float32);
  CHECK(host);
  auto d = r.device();
  CHECK(d);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto task = r.upload(*core, group, *host, *d);
  CHECK(task && core->drain());
  auto result = observe(*core, task);
  retire(*core, group);
  return result;
}
std::uint32_t sum(std::vector<std::uint32_t> v) {
  if (v.empty())
    return 0;
  while (v.size() > 1) {
    auto n = v.size();
    for (std::size_t i = 0; i < n / 2; ++i)
      v[i] = float_proof::oracle({v[2 * i], v[2 * i + 1]}, 0);
    if (n % 2)
      v[n / 2] = v.back();
    v.resize((n + 1) / 2);
  }
  return v[0];
}
template <class F> void until(TaskCore &core, F predicate) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < end);
    core.step();
    std::this_thread::yield();
  }
}
std::vector<std::uint32_t> multiply(const std::vector<std::uint32_t> &a,
                                    const std::vector<std::uint32_t> &b, std::int64_t m,
                                    std::int64_t k, std::int64_t n) {
  std::vector<std::uint32_t> result;
  for (std::int64_t row = 0; row < m; ++row)
    for (std::int64_t col = 0; col < n; ++col) {
      std::vector<std::uint32_t> terms(k);
      for (std::int64_t i = 0; i < k; ++i)
        terms[i] = float_proof::oracle({a[row * k + i], b[i * n + col]}, 1);
      result.push_back(sum(std::move(terms)));
    }
  return result;
}
void compare(const m::Buffer &buffer, std::span<const std::uint32_t> expected) {
  auto bytes = buffer.hostBytes();
  CHECK(bytes && bytes->size() == expected.size() * 4);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    std::uint32_t actual;
    std::memcpy(&actual, bytes->data() + 4 * i, 4);
    CHECK(float_proof::equal(actual, expected[i]));
  }
}
struct Plan {
  std::vector<m::MatrixProduct> edges;
  std::vector<m::Kernel> maps;
  Plan(unsigned layers, m::Kernel map = {}) {
    unsigned input = 0;
    for (unsigned i = 0; i < layers; ++i) {
      edges.push_back({input, 1});
      maps.push_back({});
      input = 1 + edges.size();
      if (map) {
        edges.push_back({input, input});
        maps.push_back(map);
        input = 1 + edges.size();
      }
    }
  }
};
int main(int argc, char **argv) {
  CHECK(argc == 5);
  auto made = m::Runtime::create({.profile = true});
  CHECK(made);
  auto &r = **made;
  auto device = r.device();
  CHECK(device);
  const std::string directory = argv[1];
  auto kernel = r.loadMatmulKernel(directory + "/kernel0.metallib", *device);
  auto map = r.loadElementwiseKernel(directory + "/kernel0-map1.metallib", *device, {1, 0});
  auto reduction = r.loadReductionKernel(directory + "/kernel0-reduce.metallib", *device);
  CHECK(kernel && map && reduction);
  std::uint32_t seed = 0x784cf3a1;
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  std::uint64_t comparisons = 0, cases = 0;
  for (unsigned n : {0u, 1u, 3u, 9u, 33u})
    for (unsigned layers : {1u, 2u, 3u, 4u, 5u, 8u, 9u})
      for (bool mapped : {false, true})
        for (unsigned pattern = 0; pattern < 5; ++pattern) {
          std::vector<std::uint32_t> a(2 * n), b(n * n);
          for (auto *v : {&a, &b})
            for (auto &x : *v) {
              auto z = random();
              x = (z & 0x807fffffu) | ((120 + z % 7) << 23);
              if (pattern == 1)
                x = z & 0x807fffffu;
              if (pattern == 2)
                x = z & 0x80000000u;
              if (pattern == 3)
                x = std::array{0x7f800000u, 0xff800000u, 0x7fc12345u, 0xff800001u,
                               0x3f800000u}[z % 5];
              if (pattern == 4)
                x = z;
            }
          auto expected = a;
          for (unsigned i = 0; i < layers; ++i) {
            expected = multiply(expected, b, 2, n, n);
            if (mapped)
              for (auto &x : expected)
                if ((x & 0x80000000u) && (x & 0x7fffffffu) <= 0x7f800000u)
                  x = 0;
          }
          auto ga = upload(r, a, {2, n}), gb = upload(r, b, {n, n});
          const auto baseline = r.stats().bytes;
          TaskLimits limits;
          limits.workers = std::array{1u, 2u, 4u}[cases % 3];
          auto core = TaskCore::create(limits);
          CHECK(core);
          auto group = core->openRoot();
          const Plan plan(layers, mapped ? *map : m::Kernel{});
          const auto before = r.stats().submissions;
          const auto dispatches = r.stats().dispatches;
          auto work =
              r.matmulGraph(*core, group, *kernel, std::array{ga, gb}, plan.edges, plan.maps);
          CHECK(work && core->drain());
          CHECK(r.stats().submissions - before == (n ? (plan.edges.size() + 3) / 4 : 0));
          CHECK(r.stats().dispatches - dispatches == (n ? layers : 0));
          auto value = observe(*core, work);
          auto downloaded = r.download(*core, group, value);
          CHECK(downloaded && core->drain());
          compare(observe(*core, downloaded), expected);
          auto scalar = r.matmulGraphReduceSum(*core, group, *kernel, std::array{ga, gb},
                                               plan.edges, *reduction, {}, {}, plan.maps);
          CHECK(scalar && core->drain());
          Invocation context;
          float actual = 0;
          CHECK(core->observe(scalar, m::Runtime::scalarLayout(), context, &actual));
          CHECK(float_proof::equal(std::bit_cast<std::uint32_t>(actual), sum(expected)));
          retire(*core, group);
          value = {};
          empty(r, baseline);
          comparisons += expected.size() + 1;
          ++cases;
        }
  empty(r);
  // Rectangular chains also cross empty intermediates, where no command is
  // encoded. Such a stage must never strand an earlier uncommitted batch.
  for (auto dims : {std::array<std::int64_t, 5>{2, 3, 5, 7, 4},
                    {2, 3, 5, 0, 4},
                    {2, 0, 5, 7, 4},
                    {0, 3, 5, 7, 4}}) {
    std::array<m::Buffer, 4> inputs;
    std::vector<std::uint32_t> expected;
    for (unsigned i = 0; i < 4; ++i) {
      std::vector<std::uint32_t> bits(dims[i] * dims[i + 1]);
      for (auto &x : bits)
        x = (random() & 0x807fffffu) | 0x3f000000u;
      inputs[i] = upload(r, bits, {dims[i], dims[i + 1]});
      expected = i ? multiply(expected, bits, dims[0], dims[i], dims[i + 1]) : bits;
    }
    auto core = TaskCore::create();
    auto group = core->openRoot();
    const std::array<m::MatrixProduct, 3> edges{{{0, 1}, {4, 2}, {5, 3}}};
    auto t = r.matmulGraph(*core, group, *kernel, inputs, edges);
    CHECK(t && core->drain());
    auto d = r.download(*core, group, observe(*core, t));
    CHECK(d && core->drain());
    compare(observe(*core, d), expected);
    retire(*core, group);
  }
  empty(r);
  auto a = upload(r, std::vector<std::uint32_t>(4, 0x3f800000u), {2, 2});
  const auto baseline = r.stats().bytes;
  const Plan plan(5, *map); // Ten stages, three acknowledgement boundaries.
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps);
    CHECK(work && core->drain());
    if (success) {
      auto download = r.download(*core, group, observe(*core, work));
      CHECK(download && core->drain());
      compare(observe(*core, download),
              std::array<std::uint32_t, 4>{0x42000000u, 0x42000000u, 0x42000000u, 0x42000000u});
    } else
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
    retire(*core, group);
    empty(r, baseline);
  };
  r.failAllocationAt(0);
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto work = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps);
    CHECK(work && core->drain() && core->failure(work).kind == TaskFailure::None);
    retire(*core, group);
  }
  const auto attempts = r.stats().attempts;
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    r.failAllocationAt(i);
    run(false);
  }
  r.failAllocationAt(0);
  run(true);
  for (unsigned boundary = 0; boundary < 3; ++boundary)
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      const auto before = r.stats().submissions;
      r.holdCompletions(true);
      auto task = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps);
      CHECK(task);
      for (unsigned i = 0; i < boundary; ++i) {
        until(*core, [&] {
          return r.stats().submissions == before + i + 1 && core->stats().externalPending == 1;
        });
        r.holdCompletions(false);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (r.stats().completions < before + i + 1) {
          CHECK(std::chrono::steady_clock::now() < end);
          std::this_thread::yield();
        }
        r.holdCompletions(true);
      }
      until(*core, [&] {
        return r.stats().submissions == before + boundary + 1 && core->stats().externalPending == 1;
      });
      const auto held = r.stats();
      CHECK(held.bytes > baseline && held.stagingBuffers > 0 && held.inFlight == 1);
      CHECK(held.stagingBuffers == (boundary == 2 ? 2 : 4));
      if (scenario < 2)
        CHECK(core->cancel(group, scenario == 1));
      else
        r.failNextCompletion();
      CHECK(core->stats().externalPending == 1 && r.stats().bytes == held.bytes);
      r.holdCompletions(false);
      CHECK(core->drain());
      CHECK(core->failure(task).kind ==
            (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
      if (scenario == 1)
        CHECK(core->failure(group).kind == TaskFailure::DeadlineExceeded);
      CHECK(r.stats().submissions == before + boundary + 1);
      retire(*core, group);
      empty(r, baseline);
    }
  // Core destruction must wait for the submitted batch, retaining its owners.
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    r.holdCompletions(true);
    CHECK(r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps));
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r.holdCompletions(false);
    });
    core.reset();
    release.join();
    empty(r, baseline);
  }
  // A shared intermediate is consumed twice, including across batch boundaries.
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    const std::array<m::MatrixProduct, 5> shared{{{0, 0}, {1, 1}, {2, 1}, {3, 1}, {4, 1}}};
    auto t = r.matmulGraph(*core, group, *kernel, std::array{a}, shared);
    CHECK(t && core->drain());
    auto d = r.download(*core, group, observe(*core, t));
    CHECK(d && core->drain());
    std::vector<std::uint32_t> expected(4, 0x3f800000u);
    auto first = multiply(expected, expected, 2, 2, 2);
    expected = first;
    for (unsigned i = 0; i < 4; ++i)
      expected = multiply(expected, first, 2, 2, 2);
    compare(observe(*core, d), expected);
    retire(*core, group);
    empty(r, baseline);
  }
  // The original matrix remains available when another stage consumes it.
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    const std::array<m::MatrixProduct, 3> edges{{{0, 0}, {1, 1}, {1, 2}}};
    const std::array<m::Kernel, 3> maps{{{}, *map, {}}};
    const auto before = r.stats().dispatches;
    auto t = r.matmulGraph(*core, group, *kernel, std::array{a}, edges, maps);
    CHECK(t && core->drain() && r.stats().dispatches == before + 3);
    auto d = r.download(*core, group, observe(*core, t));
    CHECK(d && core->drain());
    compare(observe(*core, d), std::vector<std::uint32_t>(4, 0x41000000u));
    retire(*core, group);
    empty(r, baseline);
  }
  // Scalar captures and explicit unique operand lists survive fused batches.
  {
    auto scale = r.loadElementwiseKernel(std::string(argv[3]) + "/kernel0-map1.metallib",
                                         *device, {1, 1});
    CHECK(scale);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    Plan plan(5, *scale);
    std::array<std::uint32_t, 5> operands;
    for (unsigned i = 0; i < 5; ++i) {
      operands[i] = plan.edges[2 * i + 1].left;
      plan.edges[2 * i + 1].operands = std::span(&operands[i], 1);
    }
    const auto before = r.stats().dispatches;
    auto t = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps,
                            std::array{-0.5f});
    CHECK(t && core->drain() && r.stats().dispatches == before + 5);
    auto d = r.download(*core, group, observe(*core, t));
    CHECK(d && core->drain());
    compare(observe(*core, d), std::vector<std::uint32_t>(4, 0xbf800000u));
    retire(*core, group);
    empty(r, baseline);
  }
  // A map library without optional fused entries retains its staged path.
  {
    auto plain = r.loadElementwiseKernel(std::string(argv[4]) + "/kernel0.metallib", *device, {1, 0});
    CHECK(plain);
    const Plan plan(4, *plain);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    const auto before = r.stats().dispatches;
    auto t = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps);
    CHECK(t && core->drain() && r.stats().dispatches == before + 8);
    auto d = r.download(*core, group, observe(*core, t));
    CHECK(d && core->drain());
    compare(observe(*core, d), std::vector<std::uint32_t>(4, 0x41800000u));
    retire(*core, group);
    empty(r, baseline);
  }
  // A graph-private single-consumer map reuses one output backing, but keeps
  // its matrix/map encoders, input owners and parameter reservations through
  // the actual ACK, including cancellation/deadline/device-failure settlement.
  {
    auto plain = r.loadElementwiseKernel(std::string(argv[4]) + "/kernel0.metallib", *device, {1, 0});
    CHECK(plain);
    const Plan plan(1, *plain);
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      const auto backings = r.stats().deviceBackings;
      const auto submissions = r.stats().submissions;
      r.holdCompletions(true);
      auto t = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps);
      CHECK(t);
      until(*core, [&] { return core->stats().externalPending == 1; });
      CHECK(r.stats().submissions == submissions + 1);
      CHECK(r.stats().deviceBackings == backings + 1);
      if (scenario == 1 || scenario == 2)
        CHECK(core->cancel(group, scenario == 2));
      else if (scenario == 3)
        r.failNextCompletion();
      CHECK(r.stats().deviceBackings == backings + 1);
      r.holdCompletions(false);
      CHECK(core->drain());
      if (!scenario) {
        auto d = r.download(*core, group, observe(*core, t));
        CHECK(d && core->drain());
        compare(observe(*core, d), std::vector<std::uint32_t>(4, 0x40000000u));
      } else {
        CHECK(core->failure(t).kind == (scenario == 3 ? TaskFailure::Fault : TaskFailure::Cancelled));
        if (scenario == 2)
          CHECK(core->failure(group).kind == TaskFailure::DeadlineExceeded);
      }
      retire(*core, group);
      empty(r, baseline);
    }
    for (unsigned layers = 1; layers <= 5; ++layers) {
      const Plan p(layers, *plain);
      auto core = TaskCore::create();
      auto group = core->openRoot();
      const auto submissions = r.stats().submissions;
      auto t = r.matmulGraphReduceSum(*core, group, *kernel, std::array{a, a}, p.edges,
                                      *reduction, {}, {}, p.maps);
      CHECK(t && core->drain());
      CHECK(r.stats().submissions - submissions == (p.edges.size() + 4) / 4);
      Invocation context;
      float value = 0;
      CHECK(core->observe(t, m::Runtime::scalarLayout(), context, &value));
      CHECK(value == std::ldexp(4.0f, layers));
      retire(*core, group);
      empty(r, baseline);
    }
    // Sharing must prevent recycling even when a map changes a negative cell.
    const std::vector<std::uint32_t> bits{0x3f800000u, 0xc0000000u, 0u, 0x3f800000u};
    const std::vector<std::uint32_t> identity{0x3f800000u, 0u, 0u, 0x3f800000u};
    auto x = upload(r, bits, {2, 2}), id = upload(r, identity, {2, 2});
    const std::array<m::MatrixProduct, 3> edges{{{0, 1}, {2, 2}, {2, 3}}};
    const std::array<m::Kernel, 3> maps{{{}, *plain, {}}};
    auto first = multiply(bits, identity, 2, 2, 2), positive = first;
    for (auto &value : positive)
      if ((value & 0x80000000u) && (value & 0x7fffffffu) <= 0x7f800000u)
        value = 0;
    auto expected = multiply(first, positive, 2, 2, 2);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto t = r.matmulGraph(*core, group, *kernel, std::array{x, id}, edges, maps);
    CHECK(t && core->drain());
    auto d = r.download(*core, group, observe(*core, t));
    CHECK(d && core->drain());
    compare(observe(*core, d), expected);
    auto original = r.download(*core, group, x);
    CHECK(original && core->drain());
    compare(observe(*core, original), bits);
    retire(*core, group);
  }
  empty(r, baseline);
  // Cancellation before admission must not submit even the first batch.
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto t = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps);
    CHECK(t && core->cancel(group) && core->drain());
    CHECK(core->failure(t).kind == TaskFailure::Cancelled && r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
  // Independent tasks share immutable inputs/kernels, never batch scratch.
  {
    TaskLimits limits;
    limits.workers = 4;
    auto core = TaskCore::create(limits);
    auto group = core->openRoot();
    std::array<TaskHandle, 16> tasks;
    for (auto &t : tasks) {
      t = r.matmulGraph(*core, group, *kernel, std::array{a, a}, plan.edges, plan.maps);
      CHECK(t);
    }
    CHECK(core->drain());
    for (auto t : tasks) {
      auto d = r.download(*core, group, observe(*core, t));
      CHECK(d && core->drain());
      compare(observe(*core, d),
              std::array<std::uint32_t, 4>{0x42000000u, 0x42000000u, 0x42000000u, 0x42000000u});
    }
    auto d = r.download(*core, group, a);
    CHECK(d && core->drain());
    compare(observe(*core, d),
            std::array<std::uint32_t, 4>{0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u});
    retire(*core, group);
    empty(r, baseline);
  }
  a = {};
  empty(r);
  // A batched argument buffer must retain all 31 source owners and its own
  // reservation through acknowledgement, including failure and cancellation.
  {
    auto indirect =
        r.loadElementwiseKernel(std::string(argv[2]) + "/kernel0.metallib", *device,
                                {31, 0, m::ElementwiseBinding::ArgumentBuffer});
    CHECK(indirect);
    auto input = upload(r, std::vector<std::uint32_t>(4, 0x3f800000u), {2, 2});
    std::array<m::Buffer, 32> inputs;
    inputs.fill(input);
    std::array<std::uint32_t, 31> operands;
    for (unsigned i = 0; i < operands.size(); ++i)
      operands[i] = i + 2;
    const std::array<m::MatrixProduct, 3> edges{{{0, 1}, {2, 32, operands}, {33, 1}}};
    const std::array<m::Kernel, 3> maps{{{}, *indirect, {}}};
    const auto base = r.stats().bytes;
    auto execute = [&](bool success, bool hold) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      auto before = r.stats().submissions;
      r.holdCompletions(hold);
      auto t = r.matmulGraph(*core, group, *kernel, inputs, edges, maps);
      CHECK(t);
      if (hold) {
        until(*core, [&] { return core->stats().externalPending == 1; });
        CHECK(r.stats().submissions == before + 1 && r.stats().stagingBuffers == 4);
        CHECK(core->cancel(group));
        r.holdCompletions(false);
      }
      CHECK(core->drain());
      if (success) {
        CHECK(r.stats().submissions == before + 1);
        auto d = r.download(*core, group, observe(*core, t));
        CHECK(d && core->drain());
        compare(observe(*core, d),
                std::array<std::uint32_t, 4>{0x42800000u, 0x42800000u, 0x42800000u, 0x42800000u});
      } else {
        CHECK(core->failure(t).kind == (hold ? TaskFailure::Cancelled : TaskFailure::Fault));
        if (!hold)
          CHECK(core->failure(t).code == 2);
      }
      retire(*core, group);
      empty(r, base);
    };
    r.failAllocationAt(0);
    // Count graph checkpoints without counting the subsequent download.
    {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      auto t = r.matmulGraph(*core, group, *kernel, inputs, edges, maps);
      CHECK(t && core->drain() && core->failure(t).kind == TaskFailure::None);
      retire(*core, group);
    }
    const auto checkpoints = r.stats().attempts;
    for (std::uint64_t i = 1; i <= checkpoints; ++i) {
      r.failAllocationAt(i);
      execute(false, false);
    }
    r.failAllocationAt(0);
    execute(true, false);
    execute(false, true);
  }
  empty(r);
  // Every budget in this interval could execute the old path. Exercise flushes
  // at changing batch lengths, rather than turning successful graphs into OOM.
  for (std::uint64_t budget = 60; budget <= 180; ++budget) {
    m::Options options;
    options.maxBytes = budget;
    options.maxInFlight = 1;
    auto made = m::Runtime::create(options);
    CHECK(made);
    auto &limited = **made;
    auto device = limited.device();
    CHECK(device);
    auto matrix = limited.loadMatmulKernel(directory + "/kernel0.metallib", *device);
    CHECK(matrix);
    auto input = upload(limited, std::vector<std::uint32_t>(4, 0x3f800000u), {2, 2});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    Plan p(9);
    auto t = limited.matmulGraph(*core, group, *matrix, std::array{input, input}, p.edges);
    CHECK(t && core->drain() && core->failure(t).kind == TaskFailure::None);
    if (budget >= 64) {
      auto d = limited.download(*core, group, observe(*core, t));
      CHECK(d && core->drain());
      compare(observe(*core, d),
              std::array<std::uint32_t, 4>{0x44000000u, 0x44000000u, 0x44000000u, 0x44000000u});
    }
    retire(*core, group);
    input = {};
    empty(limited);
  }
  // Two 2x4096 by 4096x4096 products exactly fill the work allowance; the
  // third must start a new command even though the four-stage limit permits it.
  {
    m::Options options;
    options.maxBytes = 256ull * 1024 * 1024;
    auto made = m::Runtime::create(options);
    CHECK(made);
    auto &large = **made;
    auto device = large.device();
    CHECK(device);
    auto matrix = large.loadMatmulKernel(directory + "/kernel0.metallib", *device);
    CHECK(matrix);
    auto left = upload(large, std::vector<std::uint32_t>(2 * 4096, 0x3f800000u), {2, 4096});
    auto right = upload(large, std::vector<std::uint32_t>(4096 * 4096, 0x3f800000u), {4096, 4096});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    Plan plan(4);
    const auto before = large.stats().submissions;
    auto t = large.matmulGraph(*core, group, *matrix, std::array{left, right}, plan.edges);
    CHECK(t && core->drain() && large.stats().submissions == before + 2);
    auto d = large.download(*core, group, observe(*core, t));
    CHECK(d && core->drain());
    compare(observe(*core, d), std::vector<std::uint32_t>(2 * 4096, 0x57800000u));
    retire(*core, group);
    left = {};
    right = {};
    empty(large);
  }
  std::printf("Graph batching: %llu APFloat cases, %llu output checks, %llu allocation "
              "checkpoints, 9 held failures plus 3 reuse failures, 121 budgets; pre-cancel, 16 concurrent graphs, "
              "input immutability, indirect arguments, empty intermediates, work cap, cleanup "
              "and teardown passed\n",
              (unsigned long long)cases, (unsigned long long)comparisons,
              (unsigned long long)attempts);
}
