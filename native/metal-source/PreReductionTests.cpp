#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Pre-reduction assertion %d: %s\n", __LINE__, #x);                      \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
void empty(m::Runtime &r, std::optional<std::uint64_t> baseline = {}) {
  auto s = r.stats();
  CHECK(s.bytes == baseline.value_or(0) && !s.stagingBuffers && !s.inFlight && !s.argumentBytes &&
        !s.argumentPacks);
  if (!baseline)
    CHECK(!s.metalBuffers && !s.hostBackings && !s.deviceBackings);
}
void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.retainedOwners && !s.retainedBytes && !s.externalPending);
}
m::Buffer buffer(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
std::uint32_t scalar(TaskCore &core, TaskHandle task) {
  Invocation context;
  float result = 0;
  CHECK(core.observe(task, m::Runtime::scalarLayout(), context, &result));
  return std::bit_cast<std::uint32_t>(result);
}
m::Buffer upload(m::Runtime &r, std::span<const std::uint32_t> bits, bool matrix) {
  const auto n = std::int64_t(bits.size());
  const std::array<std::int64_t, 2> shape{1, n};
  auto host = r.hostTensor(std::as_bytes(bits), matrix ? std::span(shape) : std::span(&n, 1),
                           m::Format::Float32);
  CHECK(host);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto device = r.device();
  CHECK(device);
  auto task = r.upload(*core, group, *host, *device);
  CHECK(task && core->drain());
  auto result = buffer(*core, task);
  retire(*core, group);
  return result;
}
std::uint32_t oracle(std::vector<std::uint32_t> bits, std::array<float, 2> scalars) {
  for (auto &v : bits) {
    v = float_proof::oracle({v, std::bit_cast<std::uint32_t>(scalars[0])}, 0);
    v = float_proof::oracle({v, std::bit_cast<std::uint32_t>(scalars[1])}, 1);
  }
  if (bits.empty())
    bits.push_back(0);
  while (bits.size() > 1) {
    const auto n = bits.size();
    for (std::size_t i = 0; i < n / 2; ++i)
      bits[i] = float_proof::oracle({bits[2 * i], bits[2 * i + 1]}, 0);
    if (n % 2)
      bits[n / 2] = bits.back();
    bits.resize((n + 1) / 2);
  }
#ifdef KALANDRA_POST_REDUCTION
  auto shifted = float_proof::oracle({bits[0], std::bit_cast<std::uint32_t>(scalars[0])}, 0);
  return float_proof::oracle({shifted ^ 0x80000000u, std::bit_cast<std::uint32_t>(scalars[1])}, 5);
#else
  return bits[0];
#endif
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
  auto runtime = m::Runtime::create();
  CHECK(runtime);
  auto &r = **runtime;
  auto device = r.device();
  CHECK(device);
  const std::string directory = argv[1];
  auto map = r.loadElementwiseKernel(directory + "/kernel0.metallib", *device, {1, 2});
  CHECK(map);
  auto reduce = r.loadReductionKernel(directory + "/kernel0-reduce.metallib", *device);
  CHECK(reduce);
  m::Kernel post;
#ifdef KALANDRA_POST_REDUCTION
  auto loadedPost = r.loadElementwiseKernel(directory + "/kernel0-post.metallib", *device, {1, 2});
  CHECK(loadedPost);
  post = *loadedPost;
#endif
  const std::array<std::uint32_t, 14> edges{
      0,           0x80000000u, 1,           0x80000001u, 0x007fffffu, 0x00800000u, 0x3f800001u,
      0xbf800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc01234u, 0x7f801234u};
  std::vector<std::array<float, 2>> parameterCases{{2, 3}, {-0.0f, 1}, {1e-38f, -1.5f}};
#ifdef KALANDRA_POST_REDUCTION
  parameterCases.push_back({0.0f, 0.0f}); // IEEE division by zero, including 0/0.
#endif
  std::uint64_t cases = 0;
  for (bool matrix : {false, true})
    for (unsigned n : {0u, 1u, 2u, 3u, 5u, 17u, 65u, 129u, 1025u}) {
      for (unsigned pattern = 0; pattern < 4; ++pattern) {
        std::vector<std::uint32_t> bits(n);
        for (unsigned i = 0; i < n; ++i)
          bits[i] = pattern == 0   ? 0x3f800000u + i
                    : pattern == 1 ? edges[i % edges.size()]
                    : pattern == 2 ? 0x80000000u
                                   : i * 2654435761u;
        auto input = upload(r, bits, matrix);
        const auto baseline = r.stats().bytes;
        for (auto parameters : parameterCases) {
          auto core = TaskCore::create();
          CHECK(core);
          auto group = core->openRoot();
          auto graph = r.elementwiseReduceSum(*core, group, *map, *reduce, std::array{input},
                                              parameters, post);
          auto mapped = r.elementwise(*core, group, *map, std::array{input}, parameters);
          CHECK(graph && mapped && core->drain());
          auto intermediate = buffer(*core, mapped);
          auto separate = r.reduceSum(*core, group, *reduce, intermediate, post,
                                      post ? std::span(parameters) : std::span<const float>{});
          CHECK(separate && core->drain());
          auto expected = oracle(bits, parameters);
          CHECK(float_proof::equal(scalar(*core, graph), expected));
          CHECK(float_proof::equal(scalar(*core, separate), expected));
          CHECK(scalar(*core, graph) == scalar(*core, graph)); // cached scalar observations
          intermediate = {};
          retire(*core, group);
          empty(r, baseline);
          ++cases;
        }
      }
    }
  empty(r);
  // Every preparation point in either stage must settle without publishing a scalar.
  auto input = upload(r, std::vector<std::uint32_t>(65, 0x3f800000u), false);
  const auto baseline = r.stats().bytes;
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = r.elementwiseReduceSum(*core, group, *map, *reduce, std::array{input},
                                       std::array{2.0f, 3.0f}, post);
    CHECK(work && core->drain());
    if (success)
      CHECK(scalar(*core, work) ==
            oracle(std::vector<std::uint32_t>(65, 0x3f800000u), {2.0f, 3.0f}));
    else {
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
      Invocation context;
      float unpublished = 123;
      CHECK(!core->observe(work, m::Runtime::scalarLayout(), context, &unpublished) &&
            unpublished == 123);
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
#ifdef KALANDRA_POST_REDUCTION
  // Empty reduction identity still reaches the GPU epilogue, including every
  // seed-buffer preparation failure. No intermediate scalar may be published.
  {
    auto emptyInput = upload(r, {}, false);
    auto runEmpty = [&](bool success) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      auto work = r.reduceSum(*core, group, *reduce, emptyInput, post, std::array{2.0f, 3.0f});
      CHECK(work && core->drain());
      if (success)
        CHECK(scalar(*core, work) == oracle({}, {2.0f, 3.0f}));
      else
        CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 2);
      retire(*core, group);
      empty(r, baseline);
    };
    r.failAllocationAt(0);
    runEmpty(true);
    auto emptyAttempts = r.stats().attempts;
    for (std::uint64_t i = 1; i <= emptyAttempts; ++i) {
      r.failAllocationAt(i);
      runEmpty(false);
    }
    r.failAllocationAt(0);
    std::printf("Empty reduction epilogue allocation checkpoints: %llu\n",
                (unsigned long long)emptyAttempts);
  }
#endif
  // Hold acknowledgement independently in both stages: cancellation, deadline, device fault.
  for (unsigned stage : {0u, 1u})
    for (unsigned scenario : {0u, 1u, 2u}) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      const auto before = r.stats().submissions;
      r.holdCompletions(stage == 0);
      auto work = r.elementwiseReduceSum(*core, group, *map, *reduce, std::array{input},
                                         std::array{2.0f, 3.0f}, post);
      CHECK(work);
      if (stage == 1) {
        // Acknowledging map queues scheduler work but does not start reduction until step().
        until(*core, [&] {
          return r.stats().submissions == before + 1 && core->stats().externalPending == 1;
        });
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (r.stats().completions < before + 1) {
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
  // Destroying the scheduler must wait for an outstanding external lease.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    r.holdCompletions(true);
    CHECK(r.elementwiseReduceSum(*core, group, *map, *reduce, std::array{input},
                                 std::array{2.0f, 3.0f}, post));
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      r.holdCompletions(false);
    });
    core.reset();
    release.join();
    empty(r, baseline);
  }
  // Invalid/missing second kernels never publish a value with the buffer layout.
  for (auto invalid : {m::Kernel{}, *map}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto work = r.elementwiseReduceSum(*core, group, *map, invalid, std::array{input},
                                       std::array{2.0f, 3.0f}, post);
    CHECK(work && core->drain());
    CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 3);
    retire(*core, group);
    empty(r, baseline);
  }
  input = {};
  empty(r);
  // One in-flight slot suffices: the stages acknowledge serially. A later-stage
  // memory-budget failure must reclaim the mapped tensor and publish no scalar.
  for (bool limited : {false, true}) {
    auto bounded = m::Runtime::create({.maxBytes = 1024, .maxInFlight = 1});
    CHECK(bounded);
    auto &runtime = **bounded;
    auto d = runtime.device();
    CHECK(d);
    auto mapping = runtime.loadElementwiseKernel(directory + "/kernel0.metallib", *d, {1, 2});
    CHECK(mapping);
    auto reduction = runtime.loadReductionKernel(directory + "/kernel0-reduce.metallib", *d);
    CHECK(reduction);
    m::Kernel boundedPost;
#ifdef KALANDRA_POST_REDUCTION
    auto loaded = runtime.loadElementwiseKernel(directory + "/kernel0-post.metallib", *d, {1, 2});
    CHECK(loaded);
    boundedPost = *loaded;
#endif
    auto source = upload(runtime, std::vector<std::uint32_t>(65, 0x3f800000u), false);
    m::Buffer padding;
    if (limited) {
      std::vector<std::uint32_t> data(70);
      const std::int64_t size = 70;
      auto host = runtime.hostTensor(std::as_bytes(std::span(data)), std::span(&size, 1),
                                     m::Format::Float32);
      CHECK(host);
      padding = *host;
    }
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    const auto before = runtime.stats().submissions;
    auto work = runtime.elementwiseReduceSum(*core, group, *mapping, *reduction, std::array{source},
                                             std::array{2.0f, 3.0f}, boundedPost);
    CHECK(work && core->drain());
    if (limited) {
      CHECK(core->failure(work).kind == TaskFailure::Fault && core->failure(work).code == 6);
      CHECK(runtime.stats().submissions == before + 1);
    } else
      CHECK(scalar(*core, work) ==
            oracle(std::vector<std::uint32_t>(65, 0x3f800000u), {2.0f, 3.0f}));
    retire(*core, group);
    source = {};
    padding = {};
    empty(runtime);
  }
  CHECK(generated_main(argc, argv) == 0);
  empty(r);
  std::printf("%s: %llu graph/separate/APFloat cases, %llu allocation checkpoints, "
              "both-stage cancellation/failure passed\n",
              post ? "Post-reduction" : "Pre-reduction", (unsigned long long)cases,
              (unsigned long long)attempts);
}
