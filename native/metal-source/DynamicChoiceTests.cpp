#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Dynamic choices assertion %d: %s\n", __LINE__, #x);                    \
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
m::Buffer uploadValues(m::Runtime &r, std::span<const std::uint32_t> values) {
  std::array<std::int64_t, 1> shape{std::int64_t(values.size())};
  auto host = r.hostTensor(std::as_bytes(values), shape, m::Format::Float32);
  CHECK(host);
  auto d = r.device();
  CHECK(d);
  auto core = TaskCore::create();
  auto group = core->openRoot();
  auto task = r.upload(*core, group, *host, *d);
  CHECK(task && core->drain());
  auto result = observe(*core, task);
  retire(*core, group);
  return result;
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto &r = **made;
  auto d = r.device();
  CHECK(d);
  auto directory = std::string(argv[1]);
  auto adjust = r.loadElementwiseKernel(directory + "/kernel0.metallib", *d,
                                        {2, 1, m::ElementwiseBinding::ArgumentBuffer});
  auto reduce = r.loadReductionKernel(directory + "/kernel1.metallib", *d);
  auto premap = r.loadElementwiseKernel(directory + "/kernel1-map0.metallib", *d,
                                        {2, 1, m::ElementwiseBinding::ArgumentBuffer});
  auto mapping = r.loadElementwiseKernel(directory + "/kernel1-map1.metallib", *d,
                                         {3, 1, m::ElementwiseBinding::ArgumentBuffer});
  CHECK(adjust && reduce && premap && mapping);
  std::array<std::uint32_t, 3> ones{0x3f800000, 0x3f800000, 0x3f800000};
  auto one = uploadValues(r, ones);
  auto baseline = r.stats().bytes;
  std::size_t cases = 0;
  std::uint32_t seed = 0x61a8fbd3;
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  for (auto n : {0u, 1u, 3u, 7u, 65u, 257u})
    for (unsigned pattern = 0; pattern < 6; ++pattern)
      for (auto factorBits : std::array<std::uint32_t, 8>{0x40000000, 0xc0000000, 0, 0x80000000, 1,
                                                          0x80000001, 0x7fc12345, 0x7f800000}) {
        std::vector<std::uint32_t> values(n);
        for (auto &x : values) {
          auto z = random();
          x = (z & 0x807fffffu) | ((120 + z % 15) << 23);
          if (pattern == 1)
            x = z & 0x807fffffu;
          if (pattern == 2)
            x = (z & 1) ? 0x80000000u : 0;
          if (pattern == 3)
            x = std::array{0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f800001u, 0x3f800000u}[z % 5];
          if (pattern == 4)
            x = std::array{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu, 0x3f800000u}[z % 4];
          if (pattern == 5)
            x = z;
        }
        auto other = values;
        std::reverse(other.begin(), other.end());
        for (std::size_t i = 0; i < other.size(); ++i)
          if (i % 3 == 0)
            other[i] ^= 0x80000000u;
        auto combined = values;
        for (std::size_t i = 0; i < values.size(); ++i)
          combined[i] = float_proof::oracle({values[i], other[i]}, 0);
        auto input = uploadValues(r, values);
        auto alternate = uploadValues(r, other);
        llvm::APFloat factor(llvm::APFloat::IEEEsingle(), llvm::APInt(32, factorBits));
        llvm::APFloat zero(llvm::APFloat::IEEEsingle(), llvm::APInt(32, 0));
        const bool positive = factor.compare(zero) == llvm::APFloat::cmpGreaterThan;
        auto expected = values;
        for (std::size_t i = 0; i < expected.size(); ++i)
          expected[i] = positive ? float_proof::oracle({combined[i], factorBits}, 1)
                                 : float_proof::oracle({other[i], factorBits}, 5);
        TaskLimits limits;
        limits.workers = std::array{1u, 2u, 4u}[cases % 3];
        auto core = TaskCore::create(limits);
        CHECK(core);
        auto group = core->openRoot();
        auto first = r.elementwise(*core, group, *adjust, std::array{input, alternate},
                                   std::array{std::bit_cast<float>(factorBits)});
        CHECK(first && core->drain());
        auto output = observe(*core, first);
        auto download = r.download(*core, group, output);
        CHECK(download && core->drain());
        auto host = observe(*core, download);
        compare(host, expected);
        host = {};
        output = {};
        auto totalBits = sum(combined);
        auto threshold = float(int(cases % 3) - 1);
        auto thresholdBits = std::bit_cast<std::uint32_t>(threshold);
        llvm::APFloat total(llvm::APFloat::IEEEsingle(), llvm::APInt(32, totalBits));
        llvm::APFloat limit(llvm::APFloat::IEEEsingle(), llvm::APInt(32, thresholdBits));
        const bool above = total.compare(limit) == llvm::APFloat::cmpGreaterThan;
        expected = values;
        for (std::size_t i = 0; i < expected.size(); ++i)
          expected[i] = above ? float_proof::oracle({combined[i], totalBits}, 5)
                              : float_proof::oracle({other[i], thresholdBits}, 1);
        auto second =
            r.graphTensor(*core, group, *reduce, std::array{input, alternate},
                          std::array<m::MatrixProduct, 2>{{{0, 1}, {1, 2}}}, *reduce,
                          std::array<std::uint32_t, 1>{2}, std::array{*premap, *mapping},
                          std::array{threshold}, std::array<m::MatrixScalar, 1>{{{1, 0}}});
        CHECK(second && core->drain());
        output = observe(*core, second);
        auto cached = observe(*core, second);
        CHECK(!output.hostBytes() && cached.shape()[0] == n);
        download = r.download(*core, group, output);
        CHECK(download && core->drain());
        host = observe(*core, download);
        compare(host, expected);
        retire(*core, group);
        input = {};
        alternate = {};
        output = {};
        host = {};
        cached = {};
        empty(r, baseline);
        ++cases;
      }
  auto submit = [&](TaskCore &core, GroupHandle group) {
    return r.graphTensor(core, group, *reduce, std::array{one, one},
                         std::array<m::MatrixProduct, 2>{{{0, 1}, {1, 2}}}, *reduce,
                         std::array<std::uint32_t, 1>{2}, std::array{*premap, *mapping},
                         std::array{2.0f}, std::array<m::MatrixScalar, 1>{{{1, 0}}});
  };
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto work = submit(*core, group);
    CHECK(work && core->drain());
    if (success) {
      auto result = observe(*core, work);
      CHECK(!result.hostBytes());
    } else
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
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
  for (unsigned stage = 0; stage < 3; ++stage)
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      const auto before = r.stats().submissions;
      r.holdCompletions(true);
      auto work = submit(*core, group);
      CHECK(work);
      for (unsigned i = 0; i < stage; ++i) {
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
  std::printf("Conditional graph: %llu allocation checkpoints; nine stage failures\n",
              (unsigned long long)attempts);
  one = {};
  empty(r);
  std::printf("Dynamic choices: %zu APFloat arithmetic/feedback cases\n", cases);
}
