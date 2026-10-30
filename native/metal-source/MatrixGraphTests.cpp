#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Matrix graph assertion %d: %s\n", __LINE__, #x);                       \
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
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto &r = **made;
  auto device = r.device();
  CHECK(device);
  auto kernel = r.loadMatmulKernel(std::string(argv[1]) + "/kernel0.metallib", *device);
  CHECK(kernel);
  const std::array<m::MatrixProduct, 2> chain{{{0, 1}, {3, 2}}};
  std::uint32_t seed = 0x61a8fbd3;
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  std::uint64_t cases = 0;
  for (auto shape : {std::array<std::int64_t, 4>{0, 3, 2, 4},
                     {3, 0, 2, 4},
                     {3, 2, 0, 4},
                     {3, 2, 4, 0},
                     {1, 1, 1, 1},
                     {3, 5, 7, 3},
                     {8, 9, 8, 9},
                     {9, 17, 7, 5}})
    for (bool special : {false, true}) {
      auto [m, k, n, p] = shape;
      std::vector<std::uint32_t> a(m * k), b(k * n), c(n * p);
      for (auto *v : {&a, &b, &c})
        for (auto &x : *v)
          x = special ? random() : (0x3f000000u | (random() & 0x007fffffu));
      auto ga = upload(r, a, {m, k}), gb = upload(r, b, {k, n}), gc = upload(r, c, {n, p});
      auto baseline = r.stats().bytes;
      auto expected = multiply(multiply(a, b, m, k, n), c, m, n, p);
      for (unsigned workers : {1u, 2u, 4u}) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        CHECK(core);
        auto group = core->openRoot();
        std::array inputs{ga, gb, gc};
        auto plan = chain;
        auto work = r.matmulGraph(*core, group, *kernel, inputs, plan);
        // Capture copies outlive and are independent of these submission arrays.
        inputs = {};
        plan = {};
        CHECK(work && core->drain());
        auto resident = observe(*core, work), alias = observe(*core, work);
        CHECK(resident.onDevice() && resident.shape()[0] == m && resident.shape()[1] == p);
        CHECK(r.stats().argumentBytes == 0);
        auto download = r.download(*core, group, resident);
        CHECK(download && core->drain());
        compare(observe(*core, download), expected);
        auto first = r.matmul(*core, group, *kernel, ga, gb);
        CHECK(first && core->drain());
        auto second = r.matmul(*core, group, *kernel, observe(*core, first), gc);
        CHECK(second && core->drain());
        auto separate = r.download(*core, group, observe(*core, second));
        CHECK(separate && core->drain());
        compare(observe(*core, separate), expected);
        Invocation context;
        auto descriptor = [&](auto &bits, std::array<std::int64_t, 2> dims) {
          auto data = static_cast<float *>(context.allocate(bits.size() * 4));
          CHECK(data);
          if (!bits.empty())
            std::memcpy(data, bits.data(), bits.size() * 4);
          return Float32Tensor<2>{data, data, 0, dims, {dims[1], 1}};
        };
        auto ca = descriptor(a, {m, k}), cb = descriptor(b, {k, n}), cc = descriptor(c, {n, p});
        Float32Tensor<2> cpu;
        _mlir_ciface_kalandra_cpu_1(&cpu, &ca, &cb, &cc, &context);
        CHECK(!context.status());
        for (std::size_t i = 0; i < expected.size(); ++i)
          CHECK(float_proof::equal(std::bit_cast<std::uint32_t>(cpu.aligned[i]), expected[i]));
        retire(*core, group);
        // A retained result survives task/group destruction.
        auto retained = TaskCore::create();
        auto g = retained->openRoot();
        auto t = r.download(*retained, g, alias);
        CHECK(t && retained->drain());
        compare(observe(*retained, t), expected);
        retire(*retained, g);
        resident = {};
        alias = {};
        empty(r, baseline);
        ++cases;
      }
    }
  empty(r);
  auto a = upload(r, std::vector<std::uint32_t>(4, 0x3f800000u), {2, 2});
  auto baseline = r.stats().bytes;
  // Shared product is read twice. Three products consume it without early release.
  const std::array<m::MatrixProduct, 3> shared{{{0, 0}, {1, 1}, {2, 1}}};
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto task = r.matmulGraph(*core, group, *kernel, std::array{a}, shared);
    CHECK(task && core->drain());
    if (success) {
      auto t = r.download(*core, group, observe(*core, task));
      CHECK(t && core->drain());
      compare(observe(*core, t),
              std::array<std::uint32_t, 4>{0x42000000, 0x42000000, 0x42000000, 0x42000000});
    } else
      CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 2);
    retire(*core, group);
    empty(r, baseline);
  };
  // Measure only graph checkpoints (download is outside the injection sweep).
  r.failAllocationAt(0);
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto task = r.matmulGraph(*core, group, *kernel, std::array{a}, shared);
    CHECK(task && core->drain());
    CHECK(core->failure(task).kind == TaskFailure::None);
    retire(*core, group);
  }
  auto attempts = r.stats().attempts;
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    r.failAllocationAt(i);
    run(false);
  }
  r.failAllocationAt(0);
  run(true);
  for (unsigned stage = 0; stage < 3; ++stage)
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      auto before = r.stats().submissions;
      r.holdCompletions(true);
      auto t = r.matmulGraph(*core, group, *kernel, std::array{a}, shared);
      CHECK(t);
      for (unsigned i = 0; i < stage; ++i) {
        until(*core, [&] {
          return r.stats().submissions == before + i + 1 && core->stats().externalPending == 1;
        });
        r.holdCompletions(false);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (r.stats().completions < before + i + 1) {
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
      CHECK(core->failure(t).kind == (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
      if (scenario == 1)
        CHECK(core->failure(group).kind == TaskFailure::DeadlineExceeded);
      retire(*core, group);
      empty(r, baseline);
    }
  // Preflight the entire graph before submitting any command.
  for (auto plan :
       {std::vector<m::MatrixProduct>{}, {{0, 1}}, {{1, 0}}, {{0, 0}, {0, 0}}, {{0, 0}, {2, 1}}}) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto t = r.matmulGraph(*core, group, *kernel, std::array{a}, plan);
    CHECK(t && core->drain());
    CHECK(core->failure(t).code == 3 && r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    r.holdCompletions(true);
    CHECK(r.matmulGraph(*core, group, *kernel, std::array{a}, shared));
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r.holdCompletions(false);
    });
    core.reset();
    release.join();
    empty(r, baseline);
  }
  // Late shape mismatches and wrong residency fail before the first dispatch.
  {
    auto bad = upload(r, std::vector<std::uint32_t>(6, 0x3f800000u), {3, 2});
    auto host =
        r.hostTensor(std::as_bytes(std::span<const std::uint32_t>(std::array<std::uint32_t, 4>{})),
                     std::array<std::int64_t, 2>{2, 2}, m::Format::Float32);
    CHECK(host);
    for (bool residency : {false, true}) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      auto before = r.stats().submissions;
      std::array inputs{a, a, residency ? *host : bad};
      auto t = r.matmulGraph(*core, group, *kernel, inputs, chain);
      CHECK(t && core->drain());
      CHECK(core->failure(t).code == (residency ? 7 : 3) && r.stats().submissions == before);
      retire(*core, group);
    }
  }
  empty(r, baseline);
  a = {};
  empty(r);
  // Long empty chains use iterative acknowledgement, not recursive runtime calls.
  {
    auto z = upload(r, {}, {0, 0});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    std::vector<m::MatrixProduct> plan;
    for (std::uint32_t i = 0; i < 2048; ++i)
      plan.push_back({i, 0});
    auto t = r.matmulGraph(*core, group, *kernel, std::array{z}, plan);
    CHECK(t && core->drain());
    CHECK(observe(*core, t).shape()[0] == 0);
    retire(*core, group);
  }
  empty(r);
  // Last-use reclamation permits three products with only two live outputs.
  {
    m::Options options;
    options.maxBytes = 60;
    options.maxInFlight = 1;
    auto bounded = m::Runtime::create(options);
    CHECK(bounded);
    auto &limited = **bounded;
    auto d = limited.device();
    CHECK(d);
    auto k = limited.loadMatmulKernel(std::string(argv[1]) + "/kernel0.metallib", *d);
    CHECK(k);
    auto input = upload(limited, std::vector<std::uint32_t>(4, 0x3f800000u), {2, 2});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    const std::array<m::MatrixProduct, 3> plan{{{0, 0}, {1, 0}, {2, 0}}};
    auto t = limited.matmulGraph(*core, group, *k, std::array{input}, plan);
    CHECK(t && core->drain());
    CHECK(core->failure(t).kind == TaskFailure::None);
    retire(*core, group);
    input = {};
    empty(limited);
  }
  {
    m::Options options;
    options.maxArgumentBytes = 1;
    auto limited = m::Runtime::create(options);
    CHECK(limited);
    auto &rt = **limited;
    auto d = rt.device();
    CHECK(d);
    auto k = rt.loadMatmulKernel(std::string(argv[1]) + "/kernel0.metallib", *d);
    CHECK(k);
    auto input = upload(rt, std::vector<std::uint32_t>(4, 0x3f800000u), {2, 2});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto before = rt.stats().submissions;
    auto t = rt.matmulGraph(*core, group, *k, std::array{input}, shared);
    CHECK(t && core->drain());
    CHECK(core->failure(t).code == 6 && rt.stats().submissions == before);
    retire(*core, group);
    input = {};
    empty(rt);
  }
  std::printf("Matrix graph conformance: %llu cases; %llu allocation checkpoints; 9 stage faults; "
              "2048 empty products\n",
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(attempts));
}
