#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Matmul reduction assertion %d: %s\n", __LINE__, #x);                   \
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
std::uint32_t scalar(TaskCore &core, TaskHandle task) {
  Invocation context;
  float value = 123;
  CHECK(core.observe(task, m::Runtime::scalarLayout(), context, &value));
  return std::bit_cast<std::uint32_t>(value);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  const std::string directory = argv[1];
  auto owner = m::Runtime::create();
  CHECK(owner);
  auto &r = **owner;
  auto d = r.device();
  CHECK(d);
  auto matrix = r.loadMatmulKernel(directory + "/kernel0.metallib", *d);
  auto reduce = r.loadReductionKernel(directory + "/kernel0-reduce.metallib", *d);
#ifdef KALANDRA_MATRIX_ADJUSTMENT
  constexpr unsigned logicalInputs = 3;
  auto adjust = r.loadElementwiseKernel(directory + "/kernel0-post.metallib", *d, {4, 1},
                                        std::array<std::uint32_t, 2>{0, 3});
  CHECK(adjust);
  m::Buffer defaultBias;
  constexpr float retainedValue = 384.0f;
#else
  constexpr unsigned logicalInputs = 2;
  constexpr float retainedValue = 180.0f;
#endif
  auto left = r.loadElementwiseKernel(directory + "/kernel0-left.metallib", *d, {logicalInputs, 1},
                                      std::array<std::uint32_t, 1>{0});
  auto right = r.loadElementwiseKernel(directory + "/kernel0-right.metallib", *d,
                                       {logicalInputs, 1}, std::array<std::uint32_t, 1>{1});
  CHECK(matrix && reduce && left && right);
#ifdef KALANDRA_MATRIX_SCALAR
  auto finalScalar = r.loadElementwiseKernel(directory + "/kernel0-scalar.metallib", *d, {1, 1});
  CHECK(finalScalar);
#endif
  auto finishScalar = [](std::uint32_t bits, float scale) {
#ifdef KALANDRA_MATRIX_SCALAR
    auto sum = float_proof::oracle({bits, std::bit_cast<std::uint32_t>(scale)}, 0);
    return float_proof::oracle({sum ^ 0x80000000u, std::bit_cast<std::uint32_t>(scale)}, 5);
#else
    (void)scale;
    return bits;
#endif
  };
  auto graph = [&](TaskCore &core, GroupHandle group, m::Buffer a, m::Buffer b, float scale,
                   m::Buffer bias = {}) {
#ifdef KALANDRA_MATRIX_ADJUSTMENT
    if (!bias)
      bias = defaultBias;
    return r.matmulReduceSum(core, group, *matrix, *reduce, a, b, *left, *right,
                             std::array{a, b, bias}, std::array{scale}, *adjust,
                             std::array{a, a, b, bias}, std::array{scale}
#ifdef KALANDRA_MATRIX_SCALAR
                             ,
                             *finalScalar, std::array{scale}
#endif
    );
#else
    (void)bias;
    return r.matmulReduceSum(core, group, *matrix, *reduce, a, b, *left, *right, std::array{a, b},
                             std::array{scale});
#endif
  };
  std::uint64_t cases = 0;
  std::uint32_t seed = 0x7a351b43;
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  for (auto shape : {std::array<std::int64_t, 3>{0, 3, 2},
                     {2, 3, 0},
                     {2, 0, 3},
                     {1, 1, 1},
                     {3, 5, 3},
                     {4, 5, 4},
                     {9, 17, 7},
                     {17, 33, 16}}) {
    auto [m, k, n] = shape;
    for (unsigned pattern = 0; pattern < 6; ++pattern)
      for (float scale : {-1.5f, 0.0f}) {
        auto values = [&](std::size_t size) {
          std::vector<std::uint32_t> bits(size);
          for (auto &v : bits) {
            auto x = random();
            v = (x & 0x807fffffu) | ((120 + x % 15) << 23);
            if (pattern == 1)
              v = x & 0x807fffffu;
            if (pattern == 2)
              v = (x & 1) ? 0x80000000u : 0;
            if (pattern == 3)
              v = std::array{0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f800001u,
                             0x3f800000u}[x % 5];
            if (pattern == 4)
              v = std::array{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu, 0x3f800000u}[x % 4];
            if (pattern == 5)
              v = x;
          }
          return bits;
        };
        auto a = values(m * k), b = values(k * n);
        auto ga = upload(r, a, {m, k}), gb = upload(r, b, {k, n});
#ifdef KALANDRA_MATRIX_ADJUSTMENT
        auto bias = values(m * n);
        auto gc = upload(r, bias, {m, n});
#else
        m::Buffer gc;
#endif
        const auto baseline = r.stats().bytes;
        for (bool mapped : {false, true}) {
          auto core = TaskCore::create();
          CHECK(core);
          auto group = core->openRoot();
#ifdef KALANDRA_MATRIX_ADJUSTMENT
          auto work =
              mapped ? graph(*core, group, ga, gb, scale, gc)
                     : r.matmulReduceSum(*core, group, *matrix, *reduce, ga, gb, {}, {}, {}, {},
                                         *adjust, std::array{ga, ga, gb, gc}, std::array{scale}
#ifdef KALANDRA_MATRIX_SCALAR
                                         ,
                                         *finalScalar, std::array{scale}
#endif
                       );
#else
          auto work = mapped ? graph(*core, group, ga, gb, scale)
                             : r.matmulReduceSum(*core, group, *matrix, *reduce, ga, gb);
#endif
          CHECK(work && core->drain());
          auto al = ga, br = gb;
          if (mapped) {
#ifdef KALANDRA_MATRIX_ADJUSTMENT
            auto mapInputs = std::array{ga, gb, gc};
#else
            auto mapInputs = std::array{ga, gb};
#endif
            auto l = r.elementwise(*core, group, *left, mapInputs, std::array{scale});
            auto q = r.elementwise(*core, group, *right, mapInputs, std::array{scale});
            CHECK(l && q && core->drain());
            al = observe(*core, l);
            br = observe(*core, q);
          }
          auto mul = r.matmul(*core, group, *matrix, al, br);
          CHECK(mul && core->drain());
          auto resident = observe(*core, mul);
#ifdef KALANDRA_MATRIX_ADJUSTMENT
          auto adjusted = r.elementwise(*core, group, *adjust, std::array{resident, ga, gb, gc},
                                        std::array{scale});
          CHECK(adjusted && core->drain());
          resident = observe(*core, adjusted);
#endif
          auto separate = r.reduceSum(*core, group, *reduce, resident
#ifdef KALANDRA_MATRIX_SCALAR
                                      ,
                                      *finalScalar, std::array{scale}
#endif
          );
          CHECK(separate && core->drain());
          Invocation cpuContext;
          auto descriptor = [&](const auto &bits, std::array<std::int64_t, 2> dims) {
            auto *data = static_cast<float *>(cpuContext.allocate(bits.size() * 4));
            CHECK(data);
            if (!bits.empty())
              std::memcpy(data, bits.data(), bits.size() * 4);
            return Float32Tensor<2>{data, data, 0, dims, {dims[1], 1}};
          };
          auto ca = descriptor(a, {m, k}), cb = descriptor(b, {k, n});
#ifdef KALANDRA_MATRIX_ADJUSTMENT
          auto cc = descriptor(bias, {m, n});
          auto cpu = mapped ? _mlir_ciface_kalandra_cpu_0(&ca, scale, &cb, &cc, &cpuContext)
                            : _mlir_ciface_kalandra_cpu_1(&ca, &cb, &cc, scale, &cpuContext);
#else
          auto cpu = mapped ? _mlir_ciface_kalandra_cpu_0(&ca, scale, &cb, &cpuContext)
                            : _mlir_ciface_kalandra_cpu_1(&ca, &cb, &cpuContext);
#endif
          CHECK(!cpuContext.status());
          std::vector<std::uint32_t> products;
          for (std::int64_t row = 0; row < m; ++row)
            for (std::int64_t col = 0; col < n; ++col) {
              std::vector<std::uint32_t> terms(k);
              for (std::int64_t i = 0; i < k; ++i) {
                auto x = a[row * k + i], y = b[i * n + col];
                if (mapped) {
                  x = float_proof::oracle({x, std::bit_cast<std::uint32_t>(scale)}, 0);
                  y = float_proof::oracle({y, 0x3f800000u}, 3);
                }
                terms[i] = float_proof::oracle({x, y}, 1);
              }
              auto cell = sum(std::move(terms));
#ifdef KALANDRA_MATRIX_ADJUSTMENT
              cell = float_proof::oracle({cell, bias[row * n + col]}, 0);
              cell = float_proof::oracle({cell, std::bit_cast<std::uint32_t>(scale)}, 1);
#endif
              products.push_back(cell);
            }
          auto expected = sum(std::move(products));
          expected = finishScalar(expected, scale);
          CHECK(float_proof::equal(scalar(*core, work), expected));
          CHECK(float_proof::equal(scalar(*core, separate), expected));
          CHECK(float_proof::equal(std::bit_cast<std::uint32_t>(cpu), expected));
          CHECK(scalar(*core, work) == scalar(*core, work));
          al = {};
          br = {};
          resident = {};
          retire(*core, group);
          empty(r, baseline);
          ++cases;
        }
      }
  }
  empty(r);
  auto a = upload(r, std::vector<std::uint32_t>(15, 0x3f800000u), {3, 5});
  auto b = upload(r, std::vector<std::uint32_t>(20, 0x40000000u), {5, 4});
#ifdef KALANDRA_MATRIX_ADJUSTMENT
  defaultBias = upload(r, std::vector<std::uint32_t>(12, 0x3f800000u), {3, 4});
#endif
  auto baseline = r.stats().bytes;
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = graph(*core, group, a, b, 2);
    CHECK(work && core->drain());
    if (success)
      CHECK(scalar(*core, work) == finishScalar(std::bit_cast<std::uint32_t>(retainedValue), 2.0f));
    else {
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
      Invocation context;
      float value = 123;
      CHECK(!core->observe(work, m::Runtime::scalarLayout(), context, &value) && value == 123);
    }
    retire(*core, group);
    empty(r, baseline);
  };
  r.failAllocationAt(0);
  run(true);
  auto attempts = r.stats().attempts;
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    r.failAllocationAt(i);
    run(false);
  }
  r.failAllocationAt(0);
#ifdef KALANDRA_MATRIX_ADJUSTMENT
  constexpr unsigned stages = 5;
#else
  constexpr unsigned stages = 4;
#endif
  for (unsigned stage = 0; stage < stages; ++stage)
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      auto before = r.stats().submissions;
      r.holdCompletions(true);
      auto work = graph(*core, group, a, b, 2);
      CHECK(work);
      for (unsigned j = 0; j < stage; ++j) {
        until(*core, [&] {
          return r.stats().submissions == before + j + 1 && core->stats().externalPending == 1;
        });
        r.holdCompletions(false);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (r.stats().completions < before + j + 1) {
          CHECK(std::chrono::steady_clock::now() < end);
          std::this_thread::yield();
        }
        r.holdCompletions(true);
      }
      until(*core, [&] {
        return r.stats().submissions == before + stage + 1 && core->stats().externalPending == 1;
      });
      if (scenario < 2)
        CHECK(core->cancel(group, scenario == 1));
      else
        r.failNextCompletion();
      r.holdCompletions(false);
      CHECK(core->drain());
      CHECK(core->failure(work).kind ==
            (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
      if (scenario == 1)
        CHECK(core->failure(group).kind == TaskFailure::DeadlineExceeded);
      retire(*core, group);
      empty(r, baseline);
    }
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    r.holdCompletions(true);
    CHECK(graph(*core, group, a, b, 2));
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r.holdCompletions(false);
    });
    core.reset();
    release.join();
    empty(r, baseline);
  }
  for (auto invalid : {m::Kernel{}, *matrix}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto work = r.matmulReduceSum(*core, group, *matrix, invalid, a, b);
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 3);
    CHECK(r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
#ifdef KALANDRA_MATRIX_ADJUSTMENT
  // The matrix adjustment must consume the completed matrix in logical slot zero.
  {
    auto invalid = r.loadElementwiseKernel(directory + "/kernel0-post.metallib", *d, {4, 1},
                                           std::array<std::uint32_t, 2>{1, 3});
    CHECK(invalid);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto work = r.matmulReduceSum(*core, group, *matrix, *reduce, a, b, {}, {}, {}, {}, *invalid,
                                  std::array{a, a, b, defaultBias}, std::array{2.0f});
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 3);
    CHECK(r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
#endif
#ifdef KALANDRA_MATRIX_SCALAR
  // Final scalar captures cannot be confused with the matrix adjustment pack.
  for (bool wrongKernel : {false, true}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    const auto parameters = std::array{2.0f, 3.0f};
    auto work = r.matmulReduceSum(*core, group, *matrix, *reduce, a, b, {}, {}, {}, {}, {}, {}, {},
                                  wrongKernel ? *adjust : *finalScalar,
                                  std::span<const float>(parameters).first(wrongKernel ? 1 : 2));
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 3);
    CHECK(r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
#endif
  // Final scalars remain observable after all original inputs are released.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = graph(*core, group, a, b, 2);
    CHECK(work && core->drain());
    a = {};
    b = {};
#ifdef KALANDRA_MATRIX_ADJUSTMENT
    defaultBias = {};
#endif
    empty(r);
    CHECK(scalar(*core, work) == finishScalar(std::bit_cast<std::uint32_t>(retainedValue), 2.0f));
    retire(*core, group);
  }
  // A single in-flight slot suffices; reject a later-stage allocation after matrix acknowledgement.
  for (bool limited : {false, true}) {
    auto owner = m::Runtime::create({.maxBytes = 512, .maxInFlight = 1});
    CHECK(owner);
    auto &rt = **owner;
    auto dev = rt.device();
    CHECK(dev);
    auto mul = rt.loadMatmulKernel(directory + "/kernel0.metallib", *dev);
    auto red = rt.loadReductionKernel(directory + "/kernel0-reduce.metallib", *dev);
    CHECK(mul && red);
    auto x = upload(rt, std::vector<std::uint32_t>(15, 0x3f800000u), {3, 5});
    auto y = upload(rt, std::vector<std::uint32_t>(20, 0x40000000u), {5, 4});
    m::Buffer padding;
#ifdef KALANDRA_MATRIX_ADJUSTMENT
    auto adjustment = rt.loadElementwiseKernel(directory + "/kernel0-post.metallib", *dev, {4, 1},
                                               std::array<std::uint32_t, 2>{0, 3});
    CHECK(adjustment);
    auto bias = upload(rt, std::vector<std::uint32_t>(12, 0x3f800000u), {3, 4});
#ifdef KALANDRA_MATRIX_SCALAR
    auto scalarFinal =
        rt.loadElementwiseKernel(directory + "/kernel0-scalar.metallib", *dev, {1, 1});
    CHECK(scalarFinal);
#endif
    constexpr unsigned paddingWords = 66;
#else
    constexpr unsigned paddingWords = 78;
#endif
    if (limited) {
      std::vector<std::uint32_t> data(paddingWords);
      std::array<std::int64_t, 2> dims{1, paddingWords};
      auto host = rt.hostTensor(std::as_bytes(std::span(data)), dims, m::Format::Float32);
      CHECK(host);
      padding = *host;
    }
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = rt.stats().submissions;
#ifdef KALANDRA_MATRIX_ADJUSTMENT
    auto work = rt.matmulReduceSum(*core, group, *mul, *red, x, y, {}, {}, {}, {}, *adjustment,
                                   std::array{x, x, y, bias}, std::array{2.0f}
#ifdef KALANDRA_MATRIX_SCALAR
                                   ,
                                   *scalarFinal, std::array{2.0f}
#endif
    );
#else
    auto work = rt.matmulReduceSum(*core, group, *mul, *red, x, y);
#endif
    CHECK(work && core->drain());
    if (limited) {
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 6);
      CHECK(rt.stats().submissions == before + 1);
    } else
#ifdef KALANDRA_MATRIX_ADJUSTMENT
      CHECK(scalar(*core, work) == finishScalar(std::bit_cast<std::uint32_t>(264.0f), 2.0f));
#else
      CHECK(scalar(*core, work) == std::bit_cast<std::uint32_t>(120.0f));
#endif
    retire(*core, group);
    x = {};
    y = {};
    padding = {};
#ifdef KALANDRA_MATRIX_ADJUSTMENT
    bias = {};
#endif
    empty(rt);
  }
  CHECK(generated_main(argc, argv) == 0);
  empty(r);
  std::printf("Matrix reduction: %llu CPU/GPU/separate/APFloat cases, %llu allocation checkpoints, "
              "all-stage cancellation/failure passed\n",
              (unsigned long long)cases, (unsigned long long)attempts);
}
