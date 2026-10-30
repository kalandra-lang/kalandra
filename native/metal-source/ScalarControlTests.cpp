#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Scalar control assertion %d: %s\n", __LINE__, #x);                     \
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
m::Buffer uploadValue(m::Runtime &r, std::uint32_t value) {
  std::array<std::int64_t, 1> shape{1};
  auto host = r.hostTensor(std::as_bytes(std::span(&value, 1)), shape, m::Format::Float32);
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
  auto classify = r.loadElementwiseKernel(directory + "/kernel0.metallib", *d, {1, 2});
  auto reduce = r.loadReductionKernel(directory + "/kernel1.metallib", *d);
  auto mapping = r.loadElementwiseKernel(directory + "/kernel1-map0.metallib", *d,
                                         {2, 1, m::ElementwiseBinding::ArgumentBuffer});
  CHECK(classify && reduce && mapping);
  std::vector<float_proof::Pair> pairs;
  const std::array<std::uint32_t, 16> edges{0,          0x80000000, 1,          0x80000001,
                                            0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
                                            0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff,
                                            0x7f800000, 0xff800000, 0x7fc12345, 0x7f800001};
  for (auto a : edges)
    for (auto b : edges)
      pairs.push_back({a, b});
  std::uint32_t seed = 0x61a8fbd3;
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  for (unsigned i = 0; i < 128; ++i) {
    auto a = random();
    pairs.push_back({a, i % 2 ? random() : a + 1});
  }
  auto one = uploadValue(r, 0x3f800000);
  const auto baseline = r.stats().bytes;
  std::size_t cases = 0;
  for (auto pair : pairs) {
    llvm::APFloat a(llvm::APFloat::IEEEsingle(), llvm::APInt(32, pair.a));
    llvm::APFloat b(llvm::APFloat::IEEEsingle(), llvm::APInt(32, pair.b));
    auto cmp = a.compare(b);
    const bool eq = cmp == llvm::APFloat::cmpEqual, lt = cmp == llvm::APFloat::cmpLessThan,
               gt = cmp == llvm::APFloat::cmpGreaterThan;
    const auto code = unsigned(eq) + 2 * unsigned(!eq) + 4 * unsigned(lt) + 8 * unsigned(lt || eq) +
                      16 * unsigned(gt) + 32 * unsigned(gt || eq);
    auto expected = std::bit_cast<std::uint32_t>(float(code));
    auto denominator = gt ? pair.a : pair.b;
    auto normalized = float_proof::oracle({pair.a, denominator}, 5);
    auto input = uploadValue(r, pair.a);
    TaskLimits limits;
    limits.workers = std::array{1u, 2u, 4u}[cases % 3];
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    auto first =
        r.elementwise(*core, group, *classify, std::array{one},
                      std::array{std::bit_cast<float>(pair.a), std::bit_cast<float>(pair.b)});
    CHECK(first && core->drain());
    auto output = observe(*core, first);
    auto downloaded = r.download(*core, group, output);
    CHECK(downloaded && core->drain());
    auto host = observe(*core, downloaded);
    compare(host, std::span(&expected, 1));
    host = {};
    output = {};
    auto second = r.graphTensor(
        *core, group, *reduce, std::array{input}, std::array<m::MatrixProduct, 1>{{{0, 0}}},
        *reduce, std::array<std::uint32_t, 1>{0}, std::array{*mapping},
        std::array{std::bit_cast<float>(pair.b)}, std::array<m::MatrixScalar, 1>{{{0, 0}}});
    CHECK(second && core->drain());
    output = observe(*core, second);
    downloaded = r.download(*core, group, output);
    CHECK(downloaded && core->drain());
    host = observe(*core, downloaded);
    compare(host, std::span(&normalized, 1));
    retire(*core, group);
    input = {};
    output = {};
    host = {};
    empty(r, baseline);
    ++cases;
  }
  auto submit = [&](TaskCore &core, GroupHandle group) {
    return r.graphTensor(core, group, *reduce, std::array{one},
                         std::array<m::MatrixProduct, 1>{{{0, 0}}}, *reduce,
                         std::array<std::uint32_t, 1>{0}, std::array{*mapping}, std::array{2.0f},
                         std::array<m::MatrixScalar, 1>{{{0, 0}}});
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
  for (unsigned stage = 0; stage < 2; ++stage)
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
  std::printf("Conditional graph: %llu allocation checkpoints; six stage failures\n",
              (unsigned long long)attempts);
  one = {};
  empty(r);
  std::printf("Scalar control: %zu APFloat comparison/feedback pairs; all six predicates\n", cases);
}
