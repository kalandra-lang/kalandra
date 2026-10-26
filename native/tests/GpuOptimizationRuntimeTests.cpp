#include "../metal-proof/FloatOracle.h"
#include "../metal/Metal.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
using Bits = std::uint32_t;
static std::uint64_t comparisons = 0, cases = 0;
static void check(bool ok, int line) {
  if (!ok)
    throw std::runtime_error("GPU numerical regression line " + std::to_string(line));
}
#define CHECK(x) check(bool(x), __LINE__)
// Metal hardware float32 permits flush-to-zero. Keep the software IEEE oracle
// independent of the generated shader, flushing at each arithmetic boundary.
static Bits flush(Bits bits) { return (bits & 0x7f800000u) == 0 ? bits & 0x80000000u : bits; }
static Bits op(Bits a, Bits b, unsigned operation) {
  return flush(float_proof::oracle({flush(a), flush(b)}, operation));
}
static Bits sum(std::vector<Bits> values) {
  while (values.size() > 1) {
    const auto count = values.size();
    for (std::size_t i = 0; i < count / 2; ++i)
      values[i] = op(values[i * 2], values[i * 2 + 1], 0);
    if (count & 1)
      values[count / 2] = values.back();
    values.resize((count + 1) / 2);
  }
  return values.empty() ? 0 : values.front();
}
static Bits sample(std::size_t i, unsigned pattern) {
  if (pattern == 4)
    return 0x80000000; // Padding an absent leaf with +0 would change this sum.
  if (pattern == 1) {
    constexpr Bits values[] = {0,          0x80000000, 0x3f800000, 0xbf800000,
                               0x7f800000, 0xff800000, 0x7fc12345, 0x00800000,
                               0x00000001, 0x807fffff, 0x7f7fffff};
    return values[i % std::size(values)];
  }
  if (pattern == 3) {
    constexpr Bits values[] = {0x3f800001, 0xc0400000, 0};
    return values[i % std::size(values)];
  }
  if (pattern == 2) {
    constexpr Bits values[] = {0x3f800000, 0x33800000, 0xbf800000, 0x34000000, 0x3f800001,
                               0x3f7fffff, 0x4c000000, 0xcc000000, 0x3eaaaaab, 0xbe124925};
    return values[i % std::size(values)];
  }
  std::uint64_t state = i + 0x9e3779b97f4a7c15ULL;
  state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
  state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;
  state ^= state >> 31;
  return (Bits(state) & 0x807fffffu) | (Bits(97 + (state >> 32) % 61) << 23);
}
static void equal(Bits actual, Bits expected, std::size_t index) {
  if (!float_proof::equal(actual, expected)) {
    std::fprintf(stderr, "case %llu cell %zu: %08x != %08x\n", (unsigned long long)cases, index,
                 actual, expected);
    CHECK(false);
  }
  ++comparisons;
}
static void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  const auto s = core.stats();
  CHECK(!s.groups && !s.records && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
static m::Buffer upload(m::Runtime &r, TaskCore &core, std::span<const Bits> data,
                        std::span<const std::int64_t> shape) {
  auto host = r.hostTensor(std::as_bytes(data), shape, m::Format::Float32);
  CHECK(host);
  auto group = core.openRoot();
  auto task = r.upload(core, group, *host, *r.device());
  CHECK(task && core.drain());
  auto result = observe(core, task);
  retire(core, group);
  return result;
}
static void reduction(m::Runtime &r, TaskCore &core, m::Kernel map, m::Kernel reduce, std::size_t n,
                      unsigned pattern, unsigned kind, std::size_t seed = 0,
                      std::size_t rightPhase = 0) {
  ++cases;
  std::vector<Bits> a(n), b(n), values(n);
  for (std::size_t i = 0; i < n; ++i) {
    a[i] = sample(i + seed, pattern);
    b[i] = sample(i * 3 + 5 + seed + rightPhase, pattern);
    values[i] = kind == 0   ? op(a[i], b[i], 1)
                : kind == 1 ? op(op(a[i], 0x40400000, 1), b[i], 0)
                            : a[i];
  }
  const auto expected = sum(values);
  const auto da = upload(r, core, a, std::array{std::int64_t(n)}),
             db = upload(r, core, b, std::array{std::int64_t(n)});
  const auto group = core.openRoot();
  const auto submissions = r.stats().submissions;
  auto task = kind == 2 ? r.reduceSum(core, group, reduce, da)
                        : r.elementwiseReduceSum(core, group, map, reduce, std::array{da, db}, {});
  CHECK(task && core.drain());
  Invocation context;
  float actual = 0;
  CHECK(core.observe(task, m::Runtime::scalarLayout(), context, &actual));
  equal(std::bit_cast<Bits>(actual), expected, 0);
  if (kind != 2 && n)
    CHECK(r.stats().submissions == submissions + 1);
  if (n <= 260) {
    for (const auto &[input, expectedInput] :
         {std::pair{da, std::cref(a)}, std::pair{db, std::cref(b)}}) {
      auto copy = r.download(core, group, input);
      CHECK(copy && core.drain());
      auto host = observe(core, copy);
      auto bytes = host.hostBytes();
      CHECK(bytes && bytes->size() == expectedInput.get().size() * sizeof(Bits));
      CHECK(bytes->empty() ||
            std::memcmp(bytes->data(), expectedInput.get().data(), bytes->size()) == 0);
    }
  }
  retire(core, group);
}
static void matrix(m::Runtime &r, TaskCore &core, m::Kernel kernel, m::Kernel reduce,
                   std::size_t rows, std::size_t k, std::size_t columns, unsigned pattern) {
  ++cases;
  std::vector<Bits> a(rows * k), b(k * columns), expected(rows * columns), products(k);
  for (std::size_t i = 0; i < a.size(); ++i)
    a[i] = sample(i, pattern);
  for (std::size_t i = 0; i < b.size(); ++i)
    b[i] = sample(i + 9, pattern);
  for (std::size_t row = 0; row < rows; ++row)
    for (std::size_t col = 0; col < columns; ++col) {
      for (std::size_t i = 0; i < k; ++i)
        products[i] = op(a[row * k + i], b[i * columns + col], 1);
      expected[row * columns + col] = sum(products);
    }
  auto da = upload(r, core, a, std::array{std::int64_t(rows), std::int64_t(k)});
  auto db = upload(r, core, b, std::array{std::int64_t(k), std::int64_t(columns)});
  auto group = core.openRoot();
  auto task = r.matmul(core, group, kernel, da, db);
  CHECK(task && core.drain());
  auto result = observe(core, task);
  CHECK(result.shape().size() == 2 && result.shape()[0] == std::int64_t(rows) &&
        result.shape()[1] == std::int64_t(columns));
  auto downloaded = r.download(core, group, result);
  CHECK(downloaded && core.drain());
  auto host = observe(core, downloaded);
  auto bytes = host.hostBytes();
  CHECK(bytes && bytes->size() == expected.size() * 4);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    Bits actual;
    std::memcpy(&actual, bytes->data() + i * 4, 4);
    equal(actual, expected[i], i);
  }
  auto total = r.matmulReduceSum(core, group, kernel, reduce, da, db);
  CHECK(total && core.drain());
  Invocation context;
  float scalar = 0;
  CHECK(core.observe(total, m::Runtime::scalarLayout(), context, &scalar));
  equal(std::bit_cast<Bits>(scalar), sum(expected), expected.size());
  retire(core, group);
}
// Check every cell of the actual timed-input matrices, including 1024x1024.
// Only 17*13 phase pairs exist; their exact integer contractions are independent
// of the shader's floating tree and avoid a billion-operation test oracle.
static void benchmarkMatrix(m::Runtime &r, TaskCore &core, m::Kernel kernel, std::size_t n) {
  ++cases;
  std::vector<Bits> a(n * n), b(n * n);
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i] = std::bit_cast<Bits>(float(int(i % 17) - 8) / 8);
    b[i] = std::bit_cast<Bits>(float(int(i % 13) - 6) / 8);
  }
  std::array<std::array<Bits, 13>, 17> reference{};
  for (std::size_t left = 0; left < 17; ++left)
    for (std::size_t right = 0; right < 13; ++right) {
      std::int64_t total = 0;
      for (std::size_t i = 0; i < n; ++i)
        total += (int((left + i) % 17) - 8) * (int((right + i * n) % 13) - 6);
      reference[left][right] = std::bit_cast<Bits>(float(total) / 64);
    }
  const auto da = upload(r, core, a, std::array{std::int64_t(n), std::int64_t(n)});
  const auto db = upload(r, core, b, std::array{std::int64_t(n), std::int64_t(n)});
  const auto group = core.openRoot();
  const auto task = r.matmul(core, group, kernel, da, db);
  CHECK(task && core.drain());
  const auto output = observe(core, task);
  const auto download = r.download(core, group, output);
  CHECK(download && core.drain());
  const auto host = observe(core, download);
  const auto bytes = host.hostBytes();
  CHECK(bytes && bytes->size() == n * n * 4);
  for (std::size_t row = 0; row < n; ++row)
    for (std::size_t col = 0; col < n; ++col) {
      Bits value;
      std::memcpy(&value, bytes->data() + (row * n + col) * 4, 4);
      equal(value, reference[(row * n) % 17][col % 13], row * n + col);
    }
  retire(core, group);
}
static void lifetime(m::Runtime &r, TaskCore &core, m::Kernel map, m::Kernel reduce,
                     bool matrix = false) {
  const std::vector<Bits> values(16384, 0x3f800000);
  const std::vector<std::int64_t> shape =
      matrix ? std::vector<std::int64_t>{128, 128} : std::vector<std::int64_t>{16384};
  auto input = upload(r, core, values, shape);
  auto submit = [&](GroupHandle group) {
    return matrix ? r.matmulReduceSum(core, group, map, reduce, input, input)
                  : r.elementwiseReduceSum(core, group, map, reduce, std::array{input, input}, {});
  };
  const auto baseline = r.stats().bytes;
  bool success = false;
  unsigned failures = 0;
  for (unsigned point = 1; point < 128; ++point) {
    const auto group = core.openRoot();
    r.failAllocationAt(point);
    auto task = submit(group);
    CHECK(task && core.drain());
    if (core.failure(task).kind == TaskFailure::None)
      success = true;
    else {
      CHECK(core.failure(task).kind == TaskFailure::Fault && core.failure(task).code == 2);
      ++failures;
    }
    retire(core, group);
    CHECK(r.stats().bytes == baseline && !r.stats().inFlight);
    if (success)
      break;
  }
  r.failAllocationAt(0);
  CHECK(success && failures > 5);
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    const auto group = core.openRoot();
    const auto before = r.stats().submissions;
    r.holdCompletions(true);
    auto task = submit(group);
    CHECK(task);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (r.stats().submissions == before) {
      core.step();
      CHECK(std::chrono::steady_clock::now() < limit);
      std::this_thread::yield();
    }
    if (scenario < 2)
      CHECK(core.cancel(group, scenario == 1));
    else
      r.failNextCompletion();
    r.holdCompletions(false);
    CHECK(core.drain());
    CHECK(core.failure(task).kind == (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
    retire(core, group);
    CHECK(r.stats().bytes == baseline && !r.stats().inFlight);
  }
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  auto runtime = m::Runtime::create({.profile = true, .maxBytes = 512ull * 1024 * 1024});
  CHECK(runtime);
  auto &r = **runtime;
  auto core = TaskCore::create();
  CHECK(core);
  const std::string base = argv[1];
  auto dot = r.loadElementwiseKernel(base + "/kernel0.metallib", *r.device(), {2, 0});
  auto axpy = r.loadElementwiseKernel(base + "/kernel1.metallib", *r.device(), {2, 0});
  auto reduce = r.loadReductionKernel(base + "/kernel0-reduce.metallib", *r.device());
  auto mul = r.loadMatmulKernel(base + "/kernel2.metallib", *r.device());
  CHECK(dot && axpy && reduce && mul);
  for (auto n : {0, 1, 2, 3, 63, 64, 65, 127, 128, 129, 255, 256, 257, 16383, 16384, 16385, 262143,
                 262144, 262145})
    for (unsigned pattern : {0, 1, 2})
      for (unsigned kind : {0, 1, 2})
        reduction(r, *core, kind == 1 ? *axpy : *dot, *reduce, n, pattern, kind);
  for (std::size_t seed = 0; seed < 32; ++seed)
    reduction(r, *core, *dot, *reduce, 513, 0, 0, seed * 4099);
  // Exhaust every partial SIMD/workgroup boundary, including negative-zero
  // tails, then cross the next two 128-way reduction-level boundaries.
  for (std::size_t n = 0; n <= 260; ++n)
    for (unsigned pattern : {0u, 2u, 4u})
      reduction(r, *core, *dot, *reduce, n, pattern, 2);
  for (auto n : {16383u, 16384u, 16385u, 2097151u, 2097152u, 2097153u})
    reduction(r, *core, *dot, *reduce, n, 4, 2);
  reduction(r, *core, *dot, *reduce, 4194303, 0, 0);
  // Independently isolate every exceptional pair; mixed NaNs cannot hide errors.
  for (std::size_t a = 0; a < 11; ++a)
    for (std::size_t b = 0; b < 11; ++b)
      for (unsigned kind : {0, 1})
        reduction(r, *core, kind ? *axpy : *dot, *reduce, 1, 1, kind, a,
                  (b + 11 - (5 + a) % 11) % 11);
  // 3*nextafter(1,2)-3 distinguishes separate rounding from fused multiply/add.
  reduction(r, *core, *axpy, *reduce, 1, 3, 1, 0, 2);
  for (auto k : {0,   1,   7,   8,   9,   15,  16,  17,  127, 128, 129, 255, 256, 257, 288,
                 319, 320, 321, 383, 384, 385, 511, 512, 513, 544, 575, 576, 577, 640})
    for (auto rows : {0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33})
      for (auto cols : {0, 1, 7, 8, 9, 17})
        matrix(r, *core, *mul, *reduce, rows, k, cols, 0);
  for (auto k : {1, 7, 8, 9, 17, 127, 128, 129, 256, 257, 288, 320, 384, 512, 576, 640})
    for (unsigned pattern : {1, 2})
      matrix(r, *core, *mul, *reduce, 9, k, 17, pattern);
  matrix(r, *core, *mul, *reduce, 65, 129, 67, 0);
  for (auto n : {128, 256, 512, 1024, 2048})
    benchmarkMatrix(r, *core, *mul, n);
  lifetime(r, *core, *dot, *reduce);
  lifetime(r, *core, *mul, *reduce, true);
  const auto stats = r.stats();
  CHECK(!stats.bytes && !stats.metalBuffers && !stats.inFlight && !stats.argumentBytes);
  std::printf("GPU hardware numerical qualification: %llu cases, %llu exact output comparisons "
              "(APFloat + integer matrix oracle); "
              "per-cell matrices, balanced reductions, tails, rounding, signed zeros, nonfinite "
              "values and FTZ passed\n",
              (unsigned long long)cases, (unsigned long long)comparisons);
}
