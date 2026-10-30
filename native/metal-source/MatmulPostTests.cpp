#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Matmul post assertion %d: %s\n", __LINE__, #x);                        \
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
int main(int argc, char **argv) {
  CHECK(argc == 2);
  const std::string directory = argv[1];
  auto owner = m::Runtime::create();
  CHECK(owner);
  auto &r = **owner;
  auto d = r.device();
  CHECK(d);
  auto matrix = r.loadMatmulKernel(directory + "/kernel0.metallib", *d);
  CHECK(matrix);
  const std::array<std::uint32_t, 2> reads{0, 2};
  auto post = r.loadElementwiseKernel(directory + "/kernel0-post.metallib", *d, {4, 1}, reads);
  CHECK(post);
  m::Kernel leftMap, rightMap;
#ifdef KALANDRA_PRE_MATMUL
  auto lm = r.loadElementwiseKernel(directory + "/kernel0-left.metallib", *d, {3, 1},
                                    std::array<std::uint32_t, 1>{0});
  auto rm = r.loadElementwiseKernel(directory + "/kernel0-right.metallib", *d, {3, 1},
                                    std::array<std::uint32_t, 1>{2});
  CHECK(lm && rm);
  leftMap = *lm;
  rightMap = *rm;
#endif
  auto composed = [&](TaskCore &core, GroupHandle group, m::Buffer left, m::Buffer right,
                      m::Buffer bias, float scale) {
#ifdef KALANDRA_PRE_MATMUL
    return r.matmul(core, group, *matrix, left, right, *post, std::array{left, left, bias, right},
                    std::array{scale}, leftMap, rightMap, std::array{left, bias, right},
                    std::array{scale});
#else
    return r.matmul(core, group, *matrix, left, right, *post, std::array{left, left, bias, right},
                    std::array{scale});
#endif
  };
  std::uint64_t cases = 0, cells = 0;
  std::uint32_t seed = 0x65ed348b;
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
                     {2, 3, 4},
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
        auto a = values(m * k), b = values(k * n), bias = values(m * n);
        auto ga = upload(r, a, {m, k}), gb = upload(r, b, {k, n}), gc = upload(r, bias, {m, n});
        auto baseline = r.stats().bytes;
        auto core = TaskCore::create();
        CHECK(core);
        auto group = core->openRoot();
        auto work = composed(*core, group, ga, gb, gc, scale);
        CHECK(work && core->drain());
        auto result = observe(*core, work), cached = observe(*core, work);
        CHECK(result.onDevice() && result.shape()[0] == m && result.shape()[1] == n);
        auto matrixLeft = ga, matrixRight = gb;
#ifdef KALANDRA_PRE_MATMUL
        auto leftWork =
            r.elementwise(*core, group, leftMap, std::array{ga, gc, gb}, std::array{scale});
        auto rightWork =
            r.elementwise(*core, group, rightMap, std::array{ga, gc, gb}, std::array{scale});
        CHECK(leftWork && rightWork && core->drain());
        matrixLeft = observe(*core, leftWork);
        matrixRight = observe(*core, rightWork);
#endif
        auto product = r.matmul(*core, group, *matrix, matrixLeft, matrixRight);
        CHECK(product && core->drain());
        auto resident = observe(*core, product);
        auto separate =
            r.elementwise(*core, group, *post, std::array{resident, ga, gc, gb}, std::array{scale});
        CHECK(separate && core->drain());
        auto separateResult = observe(*core, separate);
        auto back = r.download(*core, group, result),
             other = r.download(*core, group, separateResult);
        CHECK(back && other && core->drain());
        auto host = observe(*core, back), otherHost = observe(*core, other);
        auto bytes = host.hostBytes(), otherBytes = otherHost.hostBytes();
        CHECK(bytes && otherBytes && bytes->size() == std::size_t(m * n * 4));
        Invocation cpuContext;
        auto descriptor = [&](const auto &bits, std::array<std::int64_t, 2> dims) {
          auto *data = static_cast<float *>(cpuContext.allocate(bits.size() * 4));
          CHECK(data);
          if (!bits.empty())
            std::memcpy(data, bits.data(), bits.size() * 4);
          return Float32Tensor<2>{data, data, 0, dims, {dims[1], 1}};
        };
        auto ca = descriptor(a, {m, k}), cb = descriptor(b, {k, n}), cc = descriptor(bias, {m, n});
        Float32Tensor<2> cpu{};
        _mlir_ciface_kalandra_cpu_1(&cpu, &ca, &cc, scale, &cb, &cpuContext);
        CHECK(!cpuContext.status());
        for (std::int64_t row = 0; row < m; ++row)
          for (std::int64_t col = 0; col < n; ++col) {
            std::vector<std::uint32_t> terms(k);
            for (std::int64_t i = 0; i < k; ++i) {
              auto left = a[row * k + i], right = b[i * n + col];
#ifdef KALANDRA_PRE_MATMUL
              left = float_proof::oracle({left, std::bit_cast<std::uint32_t>(scale)}, 0);
              right = float_proof::oracle({right, 0x3f800000u}, 3);
#endif
              terms[i] = float_proof::oracle({left, right}, 1);
            }
            auto expected = float_proof::oracle({sum(std::move(terms)), bias[row * n + col]}, 0);
            expected = float_proof::oracle({expected, std::bit_cast<std::uint32_t>(scale)}, 1);
            std::uint32_t actual, split;
            std::memcpy(&actual, bytes->data() + (row * n + col) * 4, 4);
            std::memcpy(&split, otherBytes->data() + (row * n + col) * 4, 4);
            auto cpuBits = std::bit_cast<std::uint32_t>(
                cpu.aligned[cpu.offset + row * cpu.strides[0] + col * cpu.strides[1]]);
            CHECK(float_proof::equal(actual, expected) && float_proof::equal(split, expected) &&
                  float_proof::equal(cpuBits, expected));
            ++cells;
          }
        matrixLeft = {};
        matrixRight = {};
        result = {};
        cached = {};
        resident = {};
        separateResult = {};
        host = {};
        otherHost = {};
        retire(*core, group);
        empty(r, baseline);
        ++cases;
      }
  }
  empty(r);
  auto a = upload(r, std::vector<std::uint32_t>(15, 0x3f800000u), {3, 5}),
       b = upload(r, std::vector<std::uint32_t>(20, 0x3f800000u), {5, 4}),
       bias = upload(r, std::vector<std::uint32_t>(12, 0x3f800000u), {3, 4});
  auto baseline = r.stats().bytes;
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = composed(*core, group, a, b, bias, 2.0f);
    CHECK(work && core->drain());
    if (success) {
      auto result = observe(*core, work);
      CHECK(result.bytes() == 48);
    } else {
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
      Invocation context;
      alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
      CHECK(!core->observe(work, m::Runtime::bufferLayout(), context, slot));
    }
    retire(*core, group);
    empty(r, baseline);
  };
  r.failAllocationAt(0);
  run(true);
  const auto attempts = r.stats().attempts;
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    r.failAllocationAt(i);
    run(false);
  }
  r.failAllocationAt(0);
#ifdef KALANDRA_PRE_MATMUL
  constexpr unsigned stages = 4;
#else
  constexpr unsigned stages = 2;
#endif
  for (unsigned stage = 0; stage < stages; ++stage)
    for (unsigned scenario : {0u, 1u, 2u}) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      auto before = r.stats().submissions;
      r.holdCompletions(true);
      auto work = composed(*core, group, a, b, bias, 2.0f);
      CHECK(work);
      for (unsigned completed = 0; completed < stage; ++completed) {
        until(*core, [&] {
          return r.stats().submissions == before + completed + 1 &&
                 core->stats().externalPending == 1;
        });
        r.holdCompletions(false);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (r.stats().completions < before + completed + 1) {
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
    CHECK(composed(*core, group, a, b, bias, 2.0f));
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r.holdCompletions(false);
    });
    core.reset();
    release.join();
    empty(r, baseline);
  }
  // A post interface must consume the completed result in logical slot zero.
  {
    auto invalid = r.loadElementwiseKernel(directory + "/kernel0-post.metallib", *d, {4, 1},
                                           std::array<std::uint32_t, 2>{1, 2});
    CHECK(invalid);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = r.matmul(*core, group, *matrix, a, b, *invalid, std::array{a, a, bias, b},
                         std::array{2.0f});
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 3);
    retire(*core, group);
    empty(r, baseline);
  }
#ifdef KALANDRA_PRE_MATMUL
  // Invalid map captures fail before submitting any command, releasing earlier packs.
  for (bool wrongKernel : {false, true}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto inputs = std::array{a, bias, b};
    auto work =
        r.matmul(*core, group, *matrix, a, b, *post, std::array{a, a, bias, b}, std::array{2.0f},
                 leftMap, wrongKernel ? *matrix : rightMap,
                 std::span<const m::Buffer>(inputs).first(wrongKernel ? 3 : 2), std::array{2.0f});
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 3);
    CHECK(r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
#endif
  // The returned immutable tensor survives removal of its input and group owners.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = composed(*core, group, a, b, bias, 2.0f);
    CHECK(work && core->drain());
    auto retained = observe(*core, work);
    retire(*core, group);
    a = {};
    b = {};
    bias = {};
    CHECK(r.stats().bytes == 48 && !r.stats().argumentPacks);
    auto next = core->openRoot();
    auto task = r.download(*core, next, retained);
    CHECK(task && core->drain());
    auto host = observe(*core, task);
    auto bytes = host.hostBytes();
    CHECK(bytes);
    for (unsigned i = 0; i < 12; ++i) {
      std::uint32_t bits;
      std::memcpy(&bits, bytes->data() + i * 4, 4);
#ifdef KALANDRA_PRE_MATMUL
      CHECK(bits == 0x40000000u); // (matmul(1 + 2, 1 - 1) + 1) * 2.
#else
      CHECK(bits == 0x41400000u);
#endif
    }
    host = {};
    retained = {};
    retire(*core, next);
    empty(r);
  }
  // Stage one fits this budget; the post output is rejected without publishing
  // the unadjusted matrix. One in-flight slot is sufficient on the success path.
  for (bool limited : {false, true}) {
    auto bounded = m::Runtime::create({.maxBytes = 512, .maxInFlight = 1});
    CHECK(bounded);
    auto &runtime = **bounded;
    auto device = runtime.device();
    CHECK(device);
    auto mul = runtime.loadMatmulKernel(directory + "/kernel0.metallib", *device);
    CHECK(mul);
    auto final =
        runtime.loadElementwiseKernel(directory + "/kernel0-post.metallib", *device, {4, 1}, reads);
    CHECK(final);
    auto left = upload(runtime, std::vector<std::uint32_t>(15, 0x3f800000u), {3, 5});
    auto right = upload(runtime, std::vector<std::uint32_t>(20, 0x3f800000u), {5, 4});
    auto offset = upload(runtime, std::vector<std::uint32_t>(12, 0x3f800000u), {3, 4});
    m::Buffer padding;
    if (limited) {
#ifdef KALANDRA_PRE_MATMUL
      std::vector<std::uint32_t> data(60);
#else
      std::vector<std::uint32_t> data(64);
#endif
      const std::array<std::int64_t, 2> shape{1, std::int64_t(data.size())};
      auto host = runtime.hostTensor(std::as_bytes(std::span(data)), shape, m::Format::Float32);
      CHECK(host);
      padding = *host;
    }
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = runtime.stats().submissions;
#ifdef KALANDRA_PRE_MATMUL
    auto lm = runtime.loadElementwiseKernel(directory + "/kernel0-left.metallib", *device, {3, 1},
                                            std::array<std::uint32_t, 1>{0});
    auto rm = runtime.loadElementwiseKernel(directory + "/kernel0-right.metallib", *device, {3, 1},
                                            std::array<std::uint32_t, 1>{2});
    CHECK(lm && rm);
    auto work = runtime.matmul(*core, group, *mul, left, right, *final,
                               std::array{left, left, offset, right}, std::array{2.0f}, *lm, *rm,
                               std::array{left, offset, right}, std::array{2.0f});
#else
    auto work = runtime.matmul(*core, group, *mul, left, right, *final,
                               std::array{left, left, offset, right}, std::array{2.0f});
#endif
    CHECK(work && core->drain());
    if (limited) {
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 6);
      CHECK(runtime.stats().submissions == before + 1);
    } else {
      auto value = observe(*core, work);
      CHECK(value.bytes() == 48);
    }
    retire(*core, group);
    left = {};
    right = {};
    offset = {};
    padding = {};
    empty(runtime);
  }
  CHECK(generated_main(argc, argv) == 0);
  empty(r);
  std::printf(
      "Matmul composition: %llu CPU/GPU/separate/APFloat cases, %llu cells, %llu allocation "
      "checkpoints, all-stage failure/cancellation and retained output passed\n",
      (unsigned long long)cases, (unsigned long long)cells, (unsigned long long)attempts);
}
