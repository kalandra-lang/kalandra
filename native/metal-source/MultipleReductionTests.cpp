#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Multiple reduction assertion %d: %s\n", __LINE__, #x);                 \
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
std::uint32_t scalar(TaskCore &core, TaskHandle task) {
  Invocation context;
  float value = 123;
  CHECK(core.observe(task, m::Runtime::scalarLayout(), context, &value));
  return std::bit_cast<std::uint32_t>(value);
}
std::uint32_t finish(std::uint32_t left, std::uint32_t right, float count) {
  auto bits = std::bit_cast<std::uint32_t>(count);
  return float_proof::oracle({float_proof::oracle({left, right}, 3), bits}, 5);
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto &r = **made;
  auto d = r.device();
  CHECK(d);
  auto matrix = r.loadMatmulKernel(std::string(argv[1]) + "/kernel0.metallib", *d);
  auto reduce = r.loadReductionKernel(std::string(argv[1]) + "/kernel0-reduce.metallib", *d);
  auto epilogue =
      r.loadElementwiseKernel(std::string(argv[1]) + "/kernel0-join.metallib", *d, {2, 1});
  CHECK(matrix && reduce && epilogue);
  const std::array<m::MatrixProduct, 2> chain{{{0, 1}, {3, 2}}};
  const std::array<std::uint32_t, 2> roots{3, 4};
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
    for (unsigned pattern = 0; pattern < 6; ++pattern)
      for (float count : {2.0f, 0.0f}) {
        auto [m, k, n, p] = shape;
        auto values = [&](std::size_t size) {
          std::vector<std::uint32_t> v(size);
          for (auto &x : v) {
            auto z = random();
            x = (z & 0x807fffffu) | ((120 + z % 15) << 23);
            if (pattern == 1)
              x = z & 0x807fffffu;
            if (pattern == 2)
              x = (z & 1) ? 0x80000000u : 0;
            if (pattern == 3)
              x = std::array{0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f800001u,
                             0x3f800000u}[z % 5];
            if (pattern == 4)
              x = std::array{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu, 0x3f800000u}[z % 4];
            if (pattern == 5)
              x = z;
          }
          return v;
        };
        auto a = values(m * k), b = values(k * n), c = values(n * p);
        auto ga = upload(r, a, {m, k}), gb = upload(r, b, {k, n}), gc = upload(r, c, {n, p});
        auto baseline = r.stats().bytes;
        auto expected = finish(sum(multiply(a, b, m, k, n)),
                               sum(multiply(multiply(a, b, m, k, n), c, m, n, p)), count);
        TaskLimits limits;
        limits.workers = std::array{1u, 2u, 4u}[cases % 3];
        auto core = TaskCore::create(limits);
        CHECK(core);
        auto group = core->openRoot();
        std::array inputs{ga, gb, gc};
        auto plan = chain;
        std::array scalars{count};
        auto work = r.graphReductions(*core, group, *matrix, inputs, plan, *reduce, *epilogue,
                                      roots, scalars);
        inputs = {};
        plan = {};
        scalars = {};
        CHECK(work && core->drain());
        CHECK(float_proof::equal(scalar(*core, work), expected));
        CHECK(scalar(*core, work) == scalar(*core, work));
        CHECK(!r.stats().argumentBytes);
        auto first = r.matmul(*core, group, *matrix, ga, gb);
        CHECK(first && core->drain());
        auto second = r.matmul(*core, group, *matrix, observe(*core, first), gc);
        CHECK(second && core->drain());
        auto l = r.reduceSum(*core, group, *reduce, observe(*core, first));
        auto q = r.reduceSum(*core, group, *reduce, observe(*core, second));
        CHECK(l && q && core->drain());
        CHECK(float_proof::equal(finish(scalar(*core, l), scalar(*core, q), count), expected));
        Invocation context;
        auto descriptor = [&](auto &bits, std::array<std::int64_t, 2> dims) {
          auto data = static_cast<float *>(context.allocate(bits.size() * 4));
          CHECK(data);
          if (!bits.empty())
            std::memcpy(data, bits.data(), bits.size() * 4);
          return Float32Tensor<2>{data, data, 0, dims, {dims[1], 1}};
        };
        auto ca = descriptor(a, {m, k}), cb = descriptor(b, {k, n}), cc = descriptor(c, {n, p});
        float cpu = _mlir_ciface_kalandra_cpu_1(&ca, &cb, &cc, count, &context);
        CHECK(!context.status());
        CHECK(float_proof::equal(std::bit_cast<std::uint32_t>(cpu), expected));
        retire(*core, group);
        empty(r, baseline);
        ++cases;
      }
  empty(r);
  // Reduction-only graphs accept independently shaped rank-one inputs.
  for (std::size_t left : {0u, 1u, 13u, 129u})
    for (std::size_t right : {0u, 1u, 7u}) {
      auto vector = [&](std::size_t n, float value) {
        std::vector<float> data(n, value);
        auto host = r.hostTensor(std::as_bytes(std::span(data)),
                                 std::array<std::int64_t, 1>{std::int64_t(n)}, m::Format::Float32);
        CHECK(host);
        auto core = TaskCore::create();
        auto group = core->openRoot();
        auto work = r.upload(*core, group, *host, *d);
        CHECK(work && core->drain());
        auto out = observe(*core, work);
        retire(*core, group);
        return out;
      };
      auto l = vector(left, 1.5f), q = vector(right, 0.5f);
      auto core = TaskCore::create();
      auto group = core->openRoot();
      auto work = r.graphReductions(*core, group, *reduce, std::array{l, q}, {}, *reduce, *epilogue,
                                    std::array<std::uint32_t, 2>{0, 1}, std::array{2.0f});
      CHECK(work && core->drain());
      CHECK(scalar(*core, work) ==
            std::bit_cast<std::uint32_t>((float(left) * 1.5f - float(right) * 0.5f) / 2));
      retire(*core, group);
    }
  empty(r);
  auto a = upload(r, std::vector<std::uint32_t>(4, 0x3f800000u), {2, 2});
  auto baseline = r.stats().bytes;
  const std::array<m::MatrixProduct, 3> shared{{{0, 0}, {1, 1}, {2, 1}}};
  auto submit = [&](TaskCore &core, GroupHandle group) {
    return r.graphReductions(core, group, *matrix, std::array{a}, shared, *reduce, *epilogue,
                             std::array<std::uint32_t, 2>{1, 3}, std::array{2.0f});
  };
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto work = submit(*core, group);
    CHECK(work && core->drain());
    if (success)
      CHECK(scalar(*core, work) == std::bit_cast<std::uint32_t>(-60.0f));
    else {
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
      Invocation context;
      float sentinel = 123;
      CHECK(!core->observe(work, m::Runtime::scalarLayout(), context, &sentinel) &&
            sentinel == 123);
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
  for (unsigned stage = 0; stage < 6; ++stage)
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      auto before = r.stats().submissions;
      r.holdCompletions(true);
      auto work = submit(*core, group);
      CHECK(work);
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
      CHECK(core->failure(work).kind ==
            (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
      if (scenario == 1)
        CHECK(core->failure(group).kind == TaskFailure::DeadlineExceeded);
      retire(*core, group);
      empty(r, baseline);
    }
  for (auto invalid : {m::Kernel{}, *matrix}) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto work = r.graphReductions(*core, group, *matrix, std::array{a}, shared, invalid, *epilogue,
                                  std::array<std::uint32_t, 2>{1, 3}, std::array{2.0f});
    CHECK(work && core->drain());
    CHECK(core->failure(work).code == 3 && r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
  for (auto roots : {std::array<std::uint32_t, 2>{1, 4}, {0, 0}}) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto work = r.graphReductions(*core, group, *matrix, std::array{a}, shared, *reduce, *epilogue,
                                  roots, std::array{2.0f});
    CHECK(work && core->drain());
    CHECK(core->failure(work).code == 3 && r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto work = r.graphReductions(*core, group, *matrix, std::array{a}, shared, *reduce, *matrix,
                                  std::array<std::uint32_t, 2>{1, 3}, std::array{2.0f});
    CHECK(work && core->drain());
    CHECK(core->failure(work).code == 3 && r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    r.holdCompletions(true);
    CHECK(submit(*core, group));
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r.holdCompletions(false);
    });
    core.reset();
    release.join();
    empty(r, baseline);
  }
  a = {};
  empty(r);
  {
    auto z = upload(r, {}, {0, 0});
    auto core = TaskCore::create();
    auto group = core->openRoot();
    std::vector<m::MatrixProduct> plan;
    for (std::uint32_t i = 0; i < 2048; ++i)
      plan.push_back({i, 0});
    auto work = r.graphReductions(*core, group, *matrix, std::array{z}, plan, *reduce, *epilogue,
                                  std::array<std::uint32_t, 2>{1, 2048}, std::array{2.0f});
    CHECK(work && core->drain());
    CHECK(scalar(*core, work) == std::bit_cast<std::uint32_t>(0.0f));
    retire(*core, group);
  }
  empty(r);
  std::printf("Multiple reductions: %llu numerical cases; %llu allocation checkpoints; 18 stage "
              "faults; 2048 empty products\n",
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(attempts));
}
