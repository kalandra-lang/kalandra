// Periodic inputs permit an independent APFloat reference for every large cell.
#include "../metal-proof/FloatOracle.h"
#include "../metal/MatmulLayout.h"
#include "../metal/Metal.h"
#include "ReluCases.h"
#include <array>
#include <new>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
using Bits = std::uint32_t;
#define CHECK(x) relu_tests::check(bool(x))
static_assert(m::useRegisterMatmul(256, 512, 256));
static_assert(m::useGroupedMatmul(1024, 2048, 1024));
static_assert(m::useGroupedMatmul(513, 4096, 2049));
static_assert(!m::useGroupedMatmul(1024, 1984, 1024));
static_assert(!m::useGroupedMatmul(511, 4096, 2049));
static_assert(!m::useGroupedMatmul(256, 4096, 4096));
static_assert(!m::useGroupedMatmul(65536, 4096, 1024));
static_assert(!m::useGroupedMatmul(1024, 4160, 1024));
static_assert(m::useRegisterMatmul(257, 4032, 257));
static_assert(m::useRegisterMatmul(16, 4096, 4096));
static_assert(!m::useRegisterMatmul(15, 512, 8192));
static_assert(!m::useRegisterMatmul(8192, 512, 15));
static_assert(!m::useRegisterMatmul(255, 512, 257));
static_assert(!m::useRegisterMatmul(256, 511, 256));
static_assert(!m::useRegisterMatmul(256, 513, 256));
static_assert(!m::useRegisterMatmul(256, 4097, 256));
static_assert(!m::useRegisterMatmul(256, 4160, 256));
static Bits flush(Bits value) { return (value & 0x7f800000u) == 0 ? value & 0x80000000u : value; }
static Bits op(Bits a, Bits b, unsigned kind) {
  return flush(float_proof::oracle({flush(a), flush(b)}, kind));
}
static Bits tree(std::vector<Bits> values) {
  while (values.size() > 1) {
    auto n = values.size();
    for (std::size_t i = 0; i < n / 2; ++i)
      values[i] = op(values[2 * i], values[2 * i + 1], 0);
    if (n & 1)
      values[n / 2] = values.back();
    values.resize((n + 1) / 2);
  }
  return values.empty() ? 0 : values[0];
}
static void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  auto s = core.stats();
  CHECK(!s.groups && !s.records && !s.retainedOwners && !s.retainedBytes && !s.externalPending);
}
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation ctx;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), ctx, slot));
  auto *result = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto copy = *result;
  result->~Buffer();
  return copy;
}
static m::Buffer upload(m::Runtime &rt, TaskCore &core, std::span<const Bits> bits,
                        std::array<std::int64_t, 2> shape) {
  auto host = rt.hostTensor(std::as_bytes(bits), shape, m::Format::Float32);
  CHECK(host);
  auto group = core.openRoot();
  auto task = rt.upload(core, group, *host, *rt.device());
  CHECK(task && core.drain());
  auto result = observe(core, task);
  retire(core, group);
  return result;
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  auto made =
      m::Runtime::create({.profile = true, .maxBytes = 512u * 1024u * 1024u, .maxInFlight = 1});
  CHECK(made);
  auto &rt = **made;
  auto device = rt.device();
  CHECK(device);
  const auto library = std::string(argv[1]) + "/kernel0.metallib";
  rt.failAllocationAt(0);
  {
    auto probe = rt.loadMatmulKernel(library, *device);
    CHECK(probe);
  }
  const auto preparation = rt.stats().attempts;
  CHECK(preparation >= 2);
  for (std::uint64_t fail = 1; fail <= preparation; ++fail) {
    rt.failAllocationAt(fail);
    auto probe = rt.loadMatmulKernel(library, *device);
    CHECK(!probe && probe.error() == m::Error::Allocation);
    CHECK(!rt.stats().bytes && !rt.stats().inFlight);
  }
  rt.failAllocationAt(0);
  auto kernel = rt.loadMatmulKernel(std::string(argv[1]) + "/kernel0.metallib", *device);
  CHECK(kernel);
  auto core = TaskCore::create();
  CHECK(core);
  std::size_t cells = 0, cases = 0, products = 0;
  for (auto shape : std::array{
           std::array{256, 512, 256},    std::array{257, 576, 257},    std::array{257, 960, 257},
           std::array{257, 1024, 257},   std::array{257, 1984, 257},   std::array{257, 2048, 257},
           std::array{257, 2112, 257},   std::array{257, 4032, 257},   std::array{257, 4096, 257},
           std::array{257, 4160, 257},   std::array{257, 511, 257},    std::array{257, 513, 257},
           std::array{255, 512, 257},    std::array{16, 512, 4097},    std::array{1024, 1984, 1024},
           std::array{1024, 2048, 1024}, std::array{1025, 2048, 1027}, std::array{513, 4096, 2049},
           std::array{2049, 4096, 513},  std::array{2048, 4096, 512},  std::array{511, 4096, 2049},
           std::array{512, 4096, 2048}}) {
    auto [rows, k, columns] = shape;
    for (unsigned pattern = 0; pattern < 6; ++pattern) {
      if (pattern >= 3 && shape != std::array{256, 512, 256} &&
          shape != std::array{257, 4032, 257} && shape != std::array{257, 4096, 257} &&
          shape != std::array{1025, 2048, 1027})
        continue;
      std::array<Bits, 17> aphase;
      std::array<Bits, 13> bphase;
      for (std::size_t i = 0; i < aphase.size(); ++i)
        aphase[i] = std::bit_cast<Bits>(float(int(i) - 8) / (pattern ? 37.f : 8.f));
      for (std::size_t i = 0; i < bphase.size(); ++i)
        bphase[i] = std::bit_cast<Bits>(float(int(i) - 6) / (pattern ? 29.f : 8.f));
      if (pattern == 2) {
        constexpr Bits edge[] = {0,           0x80000000u, 1,           0x80000001u, 0x7f800000u,
                                 0xff800000u, 0x7fc12345u, 0x7f800001u, 0x3f800001u, 0xbf800000u};
        for (std::size_t i = 0; i < aphase.size(); ++i)
          aphase[i] = edge[i % std::size(edge)];
        for (std::size_t i = 0; i < bphase.size(); ++i)
          bphase[i] = edge[(i + 3) % std::size(edge)];
      }
      if (pattern == 3) {
        aphase.fill(0x80000000u);
        bphase.fill(0x3f800000u);
      }
      if (pattern == 4) {
        constexpr Bits tiny[] = {1, 0x80000001u, 0x007fffffu, 0x807fffffu};
        for (std::size_t i = 0; i < aphase.size(); ++i)
          aphase[i] = tiny[i % std::size(tiny)];
        for (std::size_t i = 0; i < bphase.size(); ++i)
          bphase[i] = std::array{0x3f800000u, 0x3f000000u, 0x3f800001u}[i % 3];
      }
      if (pattern == 5) {
        constexpr Bits cancellation[] = {0x60000000u, 0x3f800000u, 0xe0000000u, 0x3f800000u};
        for (std::size_t i = 0; i < aphase.size(); ++i)
          aphase[i] = cancellation[i % std::size(cancellation)];
        bphase.fill(0x3f800000u);
      }
      std::vector<Bits> av(std::size_t(rows) * k), bv(std::size_t(k) * columns);
      for (std::size_t i = 0; i < av.size(); ++i)
        av[i] = aphase[i % 17];
      for (std::size_t i = 0; i < bv.size(); ++i)
        bv[i] = bphase[i % 13];
      std::array<std::array<Bits, 13>, 17> expected;
      for (unsigned r = 0; r < 17; ++r)
        for (unsigned c = 0; c < 13; ++c) {
          std::vector<Bits> terms(k);
          for (int t = 0; t < k; ++t)
            terms[t] = op(aphase[(r + t) % 17], bphase[(std::uint64_t(t) * columns + c) % 13], 1);
          expected[r][c] = tree(std::move(terms));
          products += k;
        }
      auto a = upload(rt, *core, av, {rows, k}), b = upload(rt, *core, bv, {k, columns});
      auto group = core->openRoot();
      auto before = rt.stats().submissions;
      auto dispatches = rt.stats().dispatches;
      auto task = rt.matmul(*core, group, *kernel, a, b);
      CHECK(task && core->drain());
      CHECK(rt.stats().submissions == before + 1);
      CHECK(rt.stats().dispatches == dispatches + 1);
      auto result = observe(*core, task);
      auto down = rt.download(*core, group, result);
      CHECK(down && core->drain());
      auto host = observe(*core, down);
      auto bytes = host.hostBytes();
      CHECK(bytes && bytes->size() == std::size_t(rows) * columns * 4);
      for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
          Bits actual;
          std::memcpy(&actual, bytes->data() + (std::size_t(r) * columns + c) * 4, 4);
          CHECK(float_proof::equal(actual, expected[(std::uint64_t(r) * k) % 17][c % 13]));
          ++cells;
        }
      for (auto input :
           {std::pair{a, std::span<const Bits>(av)}, std::pair{b, std::span<const Bits>(bv)}}) {
        auto copy = rt.download(*core, group, input.first);
        CHECK(copy && core->drain());
        auto saved = observe(*core, copy);
        auto data = saved.hostBytes();
        CHECK(data && data->size() == input.second.size_bytes());
        CHECK(std::memcmp(data->data(), input.second.data(), data->size()) == 0);
      }
      result = {};
      host = {};
      retire(*core, group);
      ++cases;
      auto stats = rt.stats();
      CHECK(!stats.inFlight && !stats.stagingBuffers && !stats.argumentBytes);
    }
    auto stats = rt.stats();
    CHECK(!stats.bytes && !stats.metalBuffers && !stats.hostBackings && !stats.deviceBackings);
  }
  // Exercise resource ownership after the new kernel is physically submitted.
  {
    auto a = upload(rt, *core, std::vector<Bits>(1024 * 2048, 0x3f800000u), {1024, 2048});
    auto b = upload(rt, *core, std::vector<Bits>(2048 * 1024, 0x3f800000u), {2048, 1024});
    const auto baseline = rt.stats().bytes;
    auto run = [&](bool success) {
      auto group = core->openRoot();
      auto work = rt.matmul(*core, group, *kernel, a, b);
      CHECK(work && core->drain());
      CHECK((core->failure(work).kind == TaskFailure::None) == success);
      if (!success)
        CHECK(core->failure(work).kind == TaskFailure::Fault &&
              core->failure(work).code == std::int64_t(m::Error::Allocation));
      retire(*core, group);
      CHECK(rt.stats().bytes == baseline && !rt.stats().inFlight && !rt.stats().stagingBuffers);
    };
    rt.failAllocationAt(0);
    run(true);
    auto attempts = rt.stats().attempts;
    CHECK(attempts >= 8);
    for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
      rt.failAllocationAt(fail);
      run(false);
    }
    rt.failAllocationAt(0);
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      auto group = core->openRoot();
      rt.holdCompletions(true);
      if (scenario == 2)
        rt.failNextCompletion();
      auto work = rt.matmul(*core, group, *kernel, a, b);
      CHECK(work);
      auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!core->stats().externalPending) {
        core->step();
        CHECK(std::chrono::steady_clock::now() < end);
        std::this_thread::yield();
      }
      CHECK(rt.stats().inFlight == 1 && rt.stats().bytes > baseline);
      if (scenario < 2)
        CHECK(core->cancel(group, scenario == 1));
      rt.holdCompletions(false);
      CHECK(core->drain());
      CHECK(core->failure(work).kind ==
            (scenario == 2 ? TaskFailure::Fault : TaskFailure::Cancelled));
      if (scenario == 2)
        CHECK(core->failure(work).code == std::int64_t(m::Error::Device));
      else
        CHECK(core->failure(group).kind ==
              (scenario == 1 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
      retire(*core, group);
      CHECK(rt.stats().bytes == baseline && !rt.stats().inFlight);
    }
  }
  auto stats = rt.stats();
  CHECK(!stats.bytes && !stats.metalBuffers && !stats.stagingBuffers && !stats.hostBackings &&
        !stats.deviceBackings);
  std::printf("Register matmul: %zu cases, %zu complete-cell APFloat checks, %zu independent "
              "products, %llu preparation checkpoints; admission edges, full root capacity, "
              "immutable inputs, allocation "
              "failures and held cancellation/deadline/device failure passed\n",
              cases, cells, products, (unsigned long long)preparation);
}
