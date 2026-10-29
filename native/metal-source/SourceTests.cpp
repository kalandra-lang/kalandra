#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Source GPU assertion %d: %s\n", __LINE__, #x);                         \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
void empty(m::Runtime &r) {
  auto s = r.stats();
  CHECK(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings && !s.inFlight &&
        !s.stagingBuffers);
}
void clean(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group));
  CHECK(core.drain());
  CHECK(core.release(group));
  auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.retainedBytes && !s.externalPending && !s.retainedOwners);
}
m::Buffer observe(TaskCore &core, TaskHandle t) {
  Invocation c;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(t, m::Runtime::bufferLayout(), c, slot));
  auto *p = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto value = *p;
  p->~Buffer();
  return value;
}
template <class P> void until(P predicate) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
std::uint32_t expected(float_proof::Pair p) {
#ifdef KALANDRA_SOURCE_COMPOSED
  return float_proof::oracle(
      {float_proof::oracle({p.a, float_proof::oracle({p.b, p.b}, 1)}, 0), 0x3f000000}, 0);
#else
  return float_proof::oracle(p, 2);
#endif
}
std::uint64_t comparisons = 0;
void check(m::Buffer b, std::span<const float_proof::Pair> pairs) {
  CHECK(!b.onDevice() && b.bytes() == pairs.size() * 4);
  auto bytes = b.hostBytes();
  CHECK(bytes);
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    std::uint32_t bits;
    std::memcpy(&bits, bytes->data() + i * 4, 4);
    CHECK(float_proof::equal(bits, expected(pairs[i])));
    ++comparisons;
  }
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  CHECK(generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto r = std::move(*made);
  // Actual source-generated supervisor runs on each worker count and retains cached outcomes.
  for (unsigned workers : {1u, 2u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.ready = 2;
    auto core = TaskCore::create(limits);
    auto group = core->openRoot();
    PacketData p{r.get(), argv[1]};
    auto t = core->submit(group, entry, {entry.packetLayout, &p, sizeof(p)});
    CHECK(core->drain());
    auto b = observe(*core, t);
    auto alias = observe(*core, t);
    CHECK(alias.hostBytes()->data() == b.hostBytes()->data());
    clean(*core, group);
    std::array<float_proof::Pair, 3> pairs{
        {{0x3f800000, 0x40800000}, {0x40000000, 0x40a00000}, {0x40400000, 0x40c00000}}};
    check(b, pairs);
  }
  empty(*r);
  // Two independent upload admissions need two ready slots. A smaller budget fails visibly.
  {
    TaskLimits limits;
    limits.ready = 1;
    auto core = TaskCore::create(limits);
    auto group = core->openRoot();
    PacketData p{r.get(), argv[1]};
    auto t = core->submit(group, entry, {entry.packetLayout, &p, sizeof(p)});
    CHECK(core->drain());
    CHECK(core->failure(t).kind != TaskFailure::None);
    clean(*core, group);
    empty(*r);
  }
  // Every source-runtime preparation failure unwinds the generated frame and its nested group.
  r->failAllocationAt(0);
  std::uint64_t attempts = 0;
  {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    PacketData p{r.get(), argv[1]};
    auto t = core->submit(group, entry, {entry.packetLayout, &p, sizeof(p)});
    CHECK(core->drain());
    CHECK(core->failure(t).kind == TaskFailure::None);
    attempts = r->stats().attempts;
    clean(*core, group);
    empty(*r);
  }
  for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
    r->failAllocationAt(fail);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    PacketData p{r.get(), argv[1]};
    auto t = core->submit(group, entry, {entry.packetLayout, &p, sizeof(p)});
    CHECK(core->drain());
    CHECK(core->failure(t).kind != TaskFailure::None);
    clean(*core, group);
    empty(*r);
  }
  r->failAllocationAt(0);
  // Shared snapshots omit upload commands. Arm the actual compute before
  // scheduling, and qualify both shared and private/blit upload policies.
  for (bool sharedUploads : {false, true}) {
    auto scenarioMade = m::Runtime::create({.sharedUploads = sharedUploads});
    CHECK(scenarioMade);
    auto r = std::move(*scenarioMade);
    auto target = r->device();
    CHECK(target);
    const auto computeOffset = sharedUploads && target->hasUnifiedMemory() ? 1u : 3u;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      PacketData p{r.get(), argv[1]};
      const auto compute = r->stats().submissions + computeOffset;
      r->holdCompletionAt(compute);
      auto t = core->submit(group, entry, {entry.packetLayout, &p, sizeof(p)});
      until([&] {
        core->step();
        return r->stats().submissions == compute && core->stats().externalPending == 1;
      });
      if (scenario == 2)
        r->failNextCompletion();
      if (scenario == 3) {
        std::atomic<bool> entered{false}, done{false};
        std::thread drop([&] {
          entered = true;
          core.reset();
          done = true;
        });
        until([&] { return entered.load(); });
        CHECK(!done);
        CHECK(r->stats().bytes);
        r->holdCompletionAt(0);
        drop.join();
        CHECK(done);
        empty(*r);
        continue;
      }
      if (scenario < 2) {
        CHECK(core->cancel(group, scenario == 1));
        CHECK(!core->drain(0));
        CHECK(!core->settled(group));
      }
      r->holdCompletionAt(0);
      CHECK(core->drain());
      CHECK(core->failure(t).kind != TaskFailure::None);
      if (scenario < 2)
        CHECK(core->failure(group).kind ==
              (scenario == 1 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
      clean(*core, group);
      empty(*r);
    }
  }
  // The source kernel accepts dynamic arrays; compare it against an independent IEEE oracle.
  auto kernel = r->loadElementwiseKernel(std::string(argv[1]) + "/kernel0.metallib", *r->device());
  CHECK(kernel);
  CHECK(!r->loadBinary32Kernel(std::string(argv[1]) + "/kernel0.metallib", *r->device()));
  auto corpus = float_proof::corpus();
  auto run = [&](std::span<const float_proof::Pair> pairs) {
    auto core = TaskCore::create();
    auto group = core->openRoot();
    std::vector<std::uint32_t> a, b;
    for (auto p : pairs) {
      a.push_back(p.a);
      b.push_back(p.b);
    }
    auto length = std::int64_t(pairs.size());
    auto ha = r->hostTensor(std::as_bytes(std::span(a)), {&length, 1}, m::Format::Float32);
    auto hb = r->hostTensor(std::as_bytes(std::span(b)), {&length, 1}, m::Format::Float32);
    CHECK(ha && hb);
    auto ta = r->upload(*core, group, *ha, *r->device()),
         tb = r->upload(*core, group, *hb, *r->device());
    CHECK(core->drain());
    auto da = observe(*core, ta), db = observe(*core, tb);
    auto work = r->elementwise(*core, group, *kernel, da, db);
    CHECK(core->drain());
    auto device = observe(*core, work);
    CHECK(device.onDevice());
    auto down = r->download(*core, group, device);
    CHECK(core->drain());
    auto result = observe(*core, down);
    clean(*core, group);
    check(result, pairs);
    auto verify = TaskCore::create();
    auto g = verify->openRoot();
    auto va = r->download(*verify, g, da), vb = r->download(*verify, g, db);
    CHECK(verify->drain());
    auto aa = observe(*verify, va), bb = observe(*verify, vb);
    if (!pairs.empty()) {
      CHECK(!std::memcmp(aa.hostBytes()->data(), a.data(), a.size() * 4));
      CHECK(!std::memcmp(bb.hostBytes()->data(), b.data(), b.size() * 4));
    }
    clean(*verify, g);
  };
  for (std::size_t n : {0u, 1u, 63u, 64u, 65u, 4097u, 8193u})
    run(std::span(corpus).first(n));
  for (std::size_t i = 0; i < corpus.size(); i += 8193)
    run(std::span(corpus).subspan(i, std::min<std::size_t>(8193, corpus.size() - i)));
  empty(*r);
  std::printf("Source-generated GPU passed: %llu APFloat comparisons; generated supervisor, "
              "failures, cancellation, and cleanup\n",
              (unsigned long long)comparisons);
}
