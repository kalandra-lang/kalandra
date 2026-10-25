#include "../metal-proof/FloatOracle.h"
#include "../metal/Metal.h"
#include "ReluCases.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <new>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
using Bits = std::uint32_t;
#define CHECK(x) relu_tests::check(bool(x))
static std::size_t cells = 0, cases = 0, attempts = 0;
static Bits flush(Bits b) { return (b & 0x7f800000u) == 0 ? b & 0x80000000u : b; }
static Bits op(Bits a, Bits b, unsigned operation) {
  return flush(float_proof::oracle({flush(a), flush(b)}, operation));
}
static Bits sum(std::vector<Bits> values) {
  while (values.size() > 1) {
    auto count = values.size();
    for (std::size_t i = 0; i < count / 2; ++i)
      values[i] = op(values[2 * i], values[2 * i + 1], 0);
    if (count & 1)
      values[count / 2] = values.back();
    values.resize((count + 1) / 2);
  }
  return values.empty() ? 0 : values[0];
}
static void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  const auto s = core.stats();
  CHECK(!s.groups && !s.records && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
static m::Buffer observe(TaskCore &core, TaskHandle work) {
  Invocation ctx;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(work, m::Runtime::bufferLayout(), ctx, slot));
  auto p = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *p;
  p->~Buffer();
  return result;
}
static m::Buffer upload(m::Runtime &rt, TaskCore &core, std::span<const Bits> bits,
                        std::array<std::int64_t, 2> shape) {
  auto host = rt.hostTensor(std::as_bytes(bits), shape, m::Format::Float32);
  CHECK(host);
  auto group = core.openRoot();
  auto work = rt.upload(core, group, *host, *rt.device());
  CHECK(work && core.drain());
  auto out = observe(core, work);
  retire(core, group);
  return out;
}
static float scalar(TaskCore &core, TaskHandle work) {
  Invocation ctx;
  float out = 0;
  CHECK(core.observe(work, m::Runtime::scalarLayout(), ctx, &out));
  return out;
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  const std::string directory = argv[1];
  for (bool tight : {false, true}) {
    auto owner = m::Runtime::create(
        {.profile = true, .maxBytes = tight ? 512u : 512u * 1024u * 1024u, .maxInFlight = 1});
    CHECK(owner);
    auto &rt = **owner;
    auto device = rt.device();
    CHECK(device);
    auto matrix = rt.loadMatmulKernel(directory + "/kernel0.metallib", *device);
    auto post = rt.loadElementwiseKernel(directory + "/kernel0-post.metallib", *device, {4, 1},
                                         std::array<std::uint32_t, 2>{0, 3});
    auto reduce = rt.loadReductionKernel(directory + "/kernel0-reduce.metallib", *device);
    CHECK(matrix && post && reduce);
    auto core = TaskCore::create();
    CHECK(core);
    for (auto shape : std::vector<std::array<std::int64_t, 3>>{{0, 3, 2},
                                                               {2, 0, 3},
                                                               {2, 3, 0},
                                                               {1, 1, 1},
                                                               {3, 5, 4},
                                                               {9, 17, 7},
                                                               {17, 33, 16},
                                                               {33, 129, 35},
                                                               {17, 512, 17},
                                                               {33, 576, 9}}) {
      if (tight && shape != std::array<std::int64_t, 3>{3, 5, 4})
        continue;
      auto [rows, k, columns] = shape;
      for (unsigned pattern = 0; pattern < 3; ++pattern) {
        auto make = [&](std::size_t n, unsigned phase) {
          std::vector<Bits> out(n);
          for (std::size_t i = 0; i < n; ++i) {
            if (pattern == 0)
              out[i] = std::bit_cast<Bits>(float(int((i + phase) % 17) - 8) / 8);
            else {
              constexpr Bits edge[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345,
                                       1, 0x80000001, 0x3f800001, 0xbf800000};
              out[i] = pattern == 1
                           ? edge[(i + phase) % std::size(edge)]
                           : std::bit_cast<Bits>(float(int((i * 37 + phase) % 101) - 50) / 37);
            }
          }
          return out;
        };
        auto av = make(rows * k, 1), bv = make(k * columns, 7), cv = make(rows * columns, 3);
        auto a = upload(rt, *core, av, {rows, k}), b = upload(rt, *core, bv, {k, columns}),
             bias = upload(rt, *core, cv, {rows, columns});
        std::vector<Bits> expected(rows * columns), products(k);
        for (std::int64_t r = 0; r < rows; ++r)
          for (std::int64_t c = 0; c < columns; ++c) {
            for (std::int64_t i = 0; i < k; ++i)
              products[i] = op(av[r * k + i], bv[i * columns + c], 1);
            auto x = op(op(sum(products), cv[r * columns + c], 0), std::bit_cast<Bits>(0.25f), 1);
            expected[r * columns + c] = Bits(relu_tests::expected(NumericType::Float32, x));
          }
        auto baseline = rt.stats().bytes;
        auto tensor = [&](GroupHandle group) {
          return rt.matmul(*core, group, *matrix, a, b, *post, std::array{a, a, b, bias},
                           std::array{0.25f});
        };
        auto pipeline = [&](GroupHandle group) {
          return rt.matmulReduceSum(*core, group, *matrix, *reduce, a, b, {}, {}, {}, {}, *post,
                                    std::array{a, a, b, bias}, std::array{0.25f});
        };
        auto group = core->openRoot();
        auto before = rt.stats().submissions;
        auto work = tensor(group);
        CHECK(work && core->drain());
        auto result = observe(*core, work);
        if (rows * columns)
          CHECK(rt.stats().submissions - before == (tight ? 2u : 1u));
        auto download = rt.download(*core, group, result);
        CHECK(download && core->drain());
        auto host = observe(*core, download);
        auto bytes = host.hostBytes();
        CHECK(bytes && bytes->size() == expected.size() * 4);
        for (std::size_t i = 0; i < expected.size(); ++i) {
          Bits actual;
          std::memcpy(&actual, bytes->data() + i * 4, 4);
          CHECK(float_proof::equal(actual, expected[i]));
          ++cells;
        }
        result = {};
        host = {};
        retire(*core, group);
        CHECK(rt.stats().bytes == baseline);
        group = core->openRoot();
        before = rt.stats().submissions;
        rt.failAllocationAt(0);
        work = pipeline(group);
        CHECK(work && core->drain());
        CHECK(float_proof::equal(std::bit_cast<Bits>(scalar(*core, work)), sum(expected)));
        if (rows * columns)
          CHECK(rt.stats().submissions - before == (tight ? 2u : 1u));
        auto checkpoints = rt.stats().attempts;
        retire(*core, group);
        CHECK(rt.stats().bytes == baseline);
        ++cases;
        if (shape == std::array<std::int64_t, 3>{3, 5, 4} && pattern == 0 && !tight) {
          for (std::uint64_t fail = 1; fail <= checkpoints; ++fail) {
            rt.failAllocationAt(fail);
            group = core->openRoot();
            work = pipeline(group);
            CHECK(work && core->drain());
            CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
            retire(*core, group);
            CHECK(rt.stats().bytes == baseline);
            ++attempts;
          }
          rt.failAllocationAt(0);
          for (unsigned scenario = 0; scenario < 3; ++scenario) {
            rt.holdCompletions(true);
            if (scenario == 2)
              rt.failNextCompletion();
            group = core->openRoot();
            before = rt.stats().submissions;
            work = pipeline(group);
            CHECK(work);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (rt.stats().submissions == before) {
              CHECK(std::chrono::steady_clock::now() < deadline);
              core->step();
              std::this_thread::yield();
            }
            CHECK(rt.stats().inFlight == 1 && rt.stats().bytes > baseline && !core->settled(work));
            if (scenario < 2)
              CHECK(core->cancel(group, scenario == 1));
            for (unsigned i = 0; i < 100; ++i)
              core->step();
            CHECK(rt.stats().bytes > baseline);
            rt.holdCompletions(false);
            CHECK(core->drain());
            CHECK(core->failure(work).kind ==
                  (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
            retire(*core, group);
            CHECK(rt.stats().bytes == baseline);
          }
        }
      }
    }
    if (!tight) {
      for (std::int64_t n : {128, 256, 512, 1024, 2048}) {
        std::vector<Bits> av(n * n), bv(n * n), cv(n * n, std::bit_cast<Bits>(0.125f));
        for (std::size_t i = 0; i < av.size(); ++i) {
          av[i] = std::bit_cast<Bits>(float(int(i % 17) - 8) / 8);
          bv[i] = std::bit_cast<Bits>(float(int(i % 13) - 6) / 8);
        }
        std::array<std::array<Bits, 13>, 17> reference{};
        for (std::int64_t a = 0; a < 17; ++a)
          for (std::int64_t b = 0; b < 13; ++b) {
            std::int64_t numerator = 0;
            for (std::int64_t k = 0; k < n; ++k)
              numerator += ((a + k) % 17 - 8) * ((b + k * n) % 13 - 6);
            reference[a][b] = std::bit_cast<Bits>(float(std::max(INT64_C(0), numerator + 8)) / 256);
          }
        auto a = upload(rt, *core, av, {n, n}), b = upload(rt, *core, bv, {n, n}),
             bias = upload(rt, *core, cv, {n, n});
        auto group = core->openRoot();
        auto before = rt.stats().submissions;
        auto work = rt.matmul(*core, group, *matrix, a, b, *post, std::array{a, a, b, bias},
                              std::array{0.25f});
        CHECK(work && core->drain() && rt.stats().submissions == before + 1);
        auto result = observe(*core, work);
        auto download = rt.download(*core, group, result);
        CHECK(download && core->drain());
        auto host = observe(*core, download);
        auto bytes = host.hostBytes();
        CHECK(bytes && bytes->size() == std::size_t(n * n * 4));
        for (std::int64_t r = 0; r < n; ++r)
          for (std::int64_t c = 0; c < n; ++c) {
            Bits actual;
            std::memcpy(&actual, bytes->data() + (r * n + c) * 4, 4);
            CHECK(actual == reference[(r * n) % 17][c % 13]);
            ++cells;
          }
        result = {};
        host = {};
        retire(*core, group);
        ++cases;
      }
    }
    auto s = rt.stats();
    CHECK(!s.bytes && !s.inFlight && !s.argumentBytes && !s.argumentPacks && !s.metalBuffers &&
          !s.stagingBuffers);
  }
  std::printf("Projection/bias/ReLU: %zu APFloat/integer-reference cases, %zu exact cells, %zu "
              "allocation failures; "
              "one-slot batching, tight-budget fallback, held "
              "cancellation/deadline/completion-failure and cleanup passed\n",
              cases, cells, attempts);
}
