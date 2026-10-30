#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Tensor feedback assertion %d: %s\n", __LINE__, #x);                    \
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
  auto d = r.device();
  CHECK(d);
  const auto directory = std::string(argv[1]);
  auto reduce = r.loadReductionKernel(directory + "/kernel0.metallib", *d);
  auto map = r.loadElementwiseKernel(directory + "/kernel0-map0.metallib", *d, {2, 1});
  CHECK(reduce && map);
  const std::array<m::MatrixProduct, 1> edges{{{0, 0}}};
  const std::array<std::uint32_t, 1> roots{0};
  const std::array<m::MatrixScalar, 1> broadcasts{{{0, 0}}};
  auto submit = [&](TaskCore &core, GroupHandle group, m::Buffer input, float scale) {
    return r.graphTensor(core, group, *reduce, std::array{input}, edges, *reduce, roots,
                         std::array{*map}, std::array{scale}, broadcasts);
  };
  std::uint32_t seed = 0x61a8fbd3;
  std::uint64_t cases = 0;
  for (auto n : {0u, 1u, 2u, 3u, 7u, 64u, 65u, 257u})
    for (unsigned pattern = 0; pattern < 6; ++pattern)
      for (float scale : {3.0f, 0.0f}) {
        std::vector<std::uint32_t> values(n);
        for (auto &x : values) {
          seed ^= seed << 13;
          seed ^= seed >> 17;
          seed ^= seed << 5;
          x = (seed & 0x807fffffu) | ((120 + seed % 15) << 23);
          if (pattern == 1)
            x = seed & 0x807fffffu;
          if (pattern == 2)
            x = (seed & 1) ? 0x80000000u : 0;
          if (pattern == 3)
            x = std::array{0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f800001u,
                           0x3f800000u}[seed % 5];
          if (pattern == 4)
            x = std::array{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu, 0x3f800000u}[seed % 4];
          if (pattern == 5)
            x = seed;
        }
        auto denominator =
            float_proof::oracle({sum(values), std::bit_cast<std::uint32_t>(scale)}, 5);
        auto expected = values;
        for (auto &x : expected)
          x = float_proof::oracle({x, denominator}, 5);
        std::array<std::int64_t, 1> shape{n};
        auto host = r.hostTensor(std::as_bytes(std::span(values)), shape, m::Format::Float32);
        CHECK(host);
        TaskLimits limits;
        limits.workers = std::array{1u, 2u, 4u}[cases % 3];
        auto core = TaskCore::create(limits);
        CHECK(core);
        auto group = core->openRoot();
        auto copy = r.upload(*core, group, *host, *d);
        CHECK(copy && core->drain());
        auto input = observe(*core, copy);
        auto before = r.stats().submissions;
        auto work = submit(*core, group, input, scale);
        CHECK(work && core->drain());
        CHECK(r.stats().submissions == before + (n ? 2 : 1));
        auto output = observe(*core, work), cached = observe(*core, work);
        CHECK(output.shape().size() == 1 && output.shape()[0] == n && !output.hostBytes());
        CHECK(cached.shape()[0] == n);
        auto download = r.download(*core, group, output);
        CHECK(download && core->drain());
        auto result = observe(*core, download);
        compare(result, expected);
        retire(*core, group);
        input = {};
        output = {};
        cached = {};
        result = {};
        host = m::Buffer{};
        empty(r);
        ++cases;
      }
  std::array<std::uint32_t, 3> values{0x40000000, 0x40800000, 0x40c00000};
  std::array<std::int64_t, 1> shape{3};
  auto host = r.hostTensor(std::as_bytes(std::span(values)), shape, m::Format::Float32);
  CHECK(host);
  auto uploads = TaskCore::create();
  auto uploadsGroup = uploads->openRoot();
  auto copy = r.upload(*uploads, uploadsGroup, *host, *d);
  CHECK(copy && uploads->drain());
  auto input = observe(*uploads, copy);
  retire(*uploads, uploadsGroup);
  host = m::Buffer{};
  const auto baseline = r.stats().bytes;
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto work = submit(*core, group, input, 3);
    CHECK(work && core->drain());
    if (success) {
      auto output = observe(*core, work);
      CHECK(output.shape()[0] == 3 && !output.hostBytes());
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
  auto attempts = r.stats().attempts;
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    r.failAllocationAt(i);
    run(false);
  }
  r.failAllocationAt(0);
  for (unsigned stage = 0; stage < 2; ++stage)
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      auto before = r.stats().submissions;
      r.holdCompletions(true);
      auto work = submit(*core, group, input, 3);
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
      Invocation context;
      alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
      CHECK(!core->observe(work, m::Runtime::bufferLayout(), context, slot));
      retire(*core, group);
      empty(r, baseline);
    }
  for (auto bindings : {std::vector<m::MatrixScalar>{}, {{0, 1}}, {{1, 0}}, {{0, 0}, {0, 0}}}) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto before = r.stats().submissions;
    auto work = r.graphTensor(*core, group, *reduce, std::array{input}, edges, *reduce, roots,
                              std::array{*map}, std::array{3.0f}, bindings);
    CHECK(work && core->drain() && core->failure(work).kind == TaskFailure::Fault);
    CHECK(r.stats().submissions == before);
    retire(*core, group);
    empty(r, baseline);
  }
  for (unsigned stage = 0; stage < 2; ++stage) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    const auto before = r.stats().submissions;
    r.holdCompletions(true);
    CHECK(submit(*core, group, input, 3));
    until(*core, [&] { return core->stats().externalPending == 1; });
    if (stage) {
      r.holdCompletions(false);
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (r.stats().completions < before + 1) {
        CHECK(std::chrono::steady_clock::now() < end);
        std::this_thread::yield();
      }
      r.holdCompletions(true);
      until(*core, [&] {
        return r.stats().submissions == before + 2 && core->stats().externalPending == 1;
      });
    }
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r.holdCompletions(false);
    });
    core.reset();
    release.join();
    empty(r, baseline);
  }
  input = {};
  empty(r);
  std::printf(
      "Tensor feedback: %llu numerical cases; %llu allocation checkpoints; six stage failures\n",
      (unsigned long long)cases, (unsigned long long)attempts);
}
