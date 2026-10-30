#define main generated_main
#include "host.cpp"
#undef main
#include <chrono>
#include <thread>
#if defined(KALANDRA_SHARED_HELPERS) || defined(KALANDRA_SCALAR_ENTRY) ||                          \
    defined(KALANDRA_MATRIX_ENTRY) || defined(KALANDRA_DIFFERENCES) ||                             \
    defined(KALANDRA_ARGUMENTS) || defined(KALANDRA_UNUSED_INPUTS) ||                              \
    defined(KALANDRA_INDIRECT_ARGUMENTS) || defined(KALANDRA_TENSOR_NEGATION) ||                   \
    defined(KALANDRA_DIVISION)
#include "../metal-proof/FloatOracle.h"
#endif
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Tensor result assertion %d: %s\n", __LINE__, #x);                      \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
std::uint64_t failureAttempt = 0;
bool injectedConstruct(Invocation &context, void *frame, const Packet &packet) {
  context.failAllocationAt(failureAttempt);
  return construct(context, frame, packet);
}
void empty(m::Runtime &runtime) {
  auto s = runtime.stats();
  CHECK(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings && !s.inFlight &&
        !s.stagingBuffers && !s.argumentBytes && !s.argumentPacks);
}
void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group));
  CHECK(core.drain());
  CHECK(core.release(group));
  auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
void verify(const m::Buffer &value) {
#if defined(KALANDRA_MATRIX_ENTRY) || defined(KALANDRA_DIFFERENCES) ||                             \
    defined(KALANDRA_ARGUMENTS) || defined(KALANDRA_INDIRECT_ARGUMENTS) ||                         \
    defined(KALANDRA_TENSOR_NEGATION) || defined(KALANDRA_DIVISION)
  CHECK(!value.onDevice() && value.shape().size() == 2 && value.shape()[0] == 2 &&
        value.shape()[1] == 3);
  auto bytes = value.hostBytes();
  CHECK(bytes && bytes->size() == 24);
  std::array<std::uint32_t, 6> bits{};
  std::memcpy(bits.data(), bytes->data(), 24);
#ifdef KALANDRA_DIVISION
  CHECK((bits == std::array<std::uint32_t, 6>{0x40c00000u, 0x40c00000u, 0x40c00000u, 0x40c00000u,
                                              0x40c00000u, 0x40c00000u}));
#elif defined(KALANDRA_TENSOR_NEGATION) || defined(KALANDRA_DIVISION)
  CHECK((bits == std::array<std::uint32_t, 6>{0xc0000000u, 0xc0800000u, 0xc0c00000u, 0xc1000000u,
                                              0xc1200000u, 0xc1400000u}));
#elif defined(KALANDRA_INDIRECT_ARGUMENTS)
  CHECK((bits == std::array<std::uint32_t, 6>{0x42280000u, 0x42a40000u, 0x42f40000u, 0x43220000u,
                                              0x434a0000u, 0x43720000u}));
#elif defined(KALANDRA_ARGUMENTS)
  CHECK((bits == std::array<std::uint32_t, 6>{0x42180000u, 0x42300000u, 0x42480000u, 0x42600000u,
                                              0x42780000u, 0x42880000u}));
#elif defined(KALANDRA_DIFFERENCES)
  CHECK((bits == std::array<std::uint32_t, 6>{0xc0400000u, 0xc0400000u, 0xc0400000u, 0xc0400000u,
                                              0xc0400000u, 0xc0400000u}));
#else
  CHECK((bits == std::array<std::uint32_t, 6>{0x41a00000u, 0x41d80000u, 0x42080000u, 0x42240000u,
                                              0x42400000u, 0x425c0000u}));
#endif
#else
  CHECK(!value.onDevice() && value.shape().size() == 1 && value.shape()[0] == 3);
  auto bytes = value.hostBytes();
  CHECK(bytes && bytes->size() == 12);
  std::array<std::uint32_t, 3> bits{};
  std::memcpy(bits.data(), bytes->data(), 12);
#ifdef KALANDRA_SCALAR_ENTRY
  CHECK((bits == std::array<std::uint32_t, 3>{0x41880000u, 0x41d00000u, 0x420c0000u}));
#elif defined(KALANDRA_SHARED_HELPERS)
  CHECK((bits == std::array<std::uint32_t, 3>{0x41300000u, 0x41c80000u, 0x422c0000u}));
#else
  CHECK((bits == std::array<std::uint32_t, 3>{0x40a00000u, 0x41400000u, 0x41a80000u}));
#endif
#endif
}
template <class F> void until(TaskCore &core, F condition) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!condition()) {
    core.step();
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
#ifdef KALANDRA_SHARED_HELPERS
void sharedConformance(m::Runtime &runtime, const char *directory) {
  auto selected = runtime.device();
  CHECK(selected);
  std::array<m::Kernel, 3> kernels;
  for (std::size_t i = 0; i < 3; ++i) {
    auto kernel = runtime.loadElementwiseKernel(
        std::string(directory) + "/kernel" + std::to_string(i) + ".metallib", *selected);
    CHECK(kernel);
    kernels[i] = *kernel;
  }
  auto corpus = float_proof::corpus();
  std::uint64_t comparisons = 0;
  auto run = [&](std::span<const float_proof::Pair> pairs) {
    Invocation context;
    const auto n = std::int64_t(pairs.size());
    auto *a = static_cast<float *>(context.allocate(n * 4));
    auto *b = static_cast<float *>(context.allocate(n * 4));
    CHECK(a && b);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      std::memcpy(a + i, &pairs[i].a, 4);
      std::memcpy(b + i, &pairs[i].b, 4);
    }
    Float32Tensor<1> left{a, a, 0, {n}, {1}}, right{b, b, 0, {n}, {1}};
    auto ha = runtime.fromCpu(context, left.view()), hb = runtime.fromCpu(context, right.view());
    CHECK(ha && hb);
    auto core = TaskCore::create();
    auto group = core->openRoot();
    auto ta = runtime.upload(*core, group, *ha, *selected),
         tb = runtime.upload(*core, group, *hb, *selected);
    CHECK(core->drain());
    auto da = observe(*core, ta), db = observe(*core, tb);
    for (unsigned op = 0; op < 3; ++op) {
      Float32Tensor<1> result{};
      if (op == 0)
        _mlir_ciface_kalandra_cpu_3(&result, &left, &right, &context);
      else if (op == 1)
        _mlir_ciface_kalandra_cpu_1(&result, &left, &right, &context);
      else
        _mlir_ciface_kalandra_cpu_2(&result, &left, &right, &context);
      CHECK(!context.status() && result.sizes[0] == n && result.strides[0] == 1);
      auto work = runtime.elementwise(*core, group, kernels[op], da, db);
      CHECK(core->drain());
      auto device = observe(*core, work);
      auto download = runtime.download(*core, group, device);
      CHECK(core->drain());
      auto gpu = observe(*core, download);
      auto bytes = gpu.hostBytes();
      CHECK(bytes && bytes->size() == pairs.size() * 4);
      for (std::size_t i = 0; i < pairs.size(); ++i) {
        std::uint32_t cpuBits, gpuBits, oldA, oldB;
        std::memcpy(&cpuBits, result.aligned + result.offset + i, 4);
        std::memcpy(&gpuBits, bytes->data() + 4 * i, 4);
        std::memcpy(&oldA, a + i, 4);
        std::memcpy(&oldB, b + i, 4);
        auto expected =
            op == 0 ? float_proof::oracle({float_proof::oracle(pairs[i], 2), 0x3f000000u}, 0)
                    : float_proof::oracle(pairs[i], op == 1 ? 0 : 1);
        CHECK(float_proof::equal(cpuBits, expected) && float_proof::equal(gpuBits, expected));
        CHECK(float_proof::equal(cpuBits, gpuBits));
        CHECK(oldA == pairs[i].a && oldB == pairs[i].b);
        ++comparisons;
      }
      context.release(result.allocated);
    }
    retire(*core, group);
    context.release(a);
    context.release(b);
    CHECK(!context.status() && !context.live() && context.allocations() == context.releases());
  };
  for (std::size_t n : {0u, 1u, 2u, 63u, 64u, 65u, 4097u, 8193u})
    run(std::span(corpus).first(n));
  for (std::size_t i = 0; i < corpus.size(); i += 8193)
    run(std::span(corpus).subspan(i, std::min<std::size_t>(8193, corpus.size() - i)));
  empty(runtime);
  // Runtime equality guards run before reading mismatched tensor operands.
  Invocation context;
  auto *a = static_cast<float *>(context.allocate(12)),
       *b = static_cast<float *>(context.allocate(8));
  CHECK(a && b);
  Float32Tensor<1> left{a, a, 0, {3}, {1}}, right{b, b, 0, {2}, {1}}, result{};
  _mlir_ciface_kalandra_cpu_3(&result, &left, &right, &context);
  CHECK(context.status() == 1);
  context.settle();
  CHECK(!context.live());
  std::printf(
      "Shared helper: %llu CPU/GPU pairs matched APFloat; NaNs compared by classification\n",
      (unsigned long long)comparisons);
}
#endif
#ifdef KALANDRA_SCALAR_ENTRY
#include "ScalarEntryChecks.h"
#endif
#ifdef KALANDRA_MATRIX_ENTRY
#include "MatrixElementwiseChecks.h"
#endif
#ifdef KALANDRA_DIFFERENCES
#include "DifferenceChecks.h"
#endif
#ifdef KALANDRA_ARGUMENTS
#include "ArgumentChecks.h"
#endif
#ifdef KALANDRA_UNUSED_INPUTS
#include "UnusedInputChecks.h"
#endif
#ifdef KALANDRA_INDIRECT_ARGUMENTS
#include "IndirectArgumentChecks.h"
#endif
#ifdef KALANDRA_TENSOR_NEGATION
#include "TensorNegationChecks.h"
#endif
#ifdef KALANDRA_DIVISION
#include "DivisionChecks.h"
#endif
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto runtime = std::move(*made);
  for (unsigned workers : {1u, 2u, 4u}) {
    bool success = false;
    unsigned failures = 0;
    for (std::uint64_t fail = 1; fail <= 128; ++fail) {
      failureAttempt = fail;
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      PacketData packet{runtime.get(), argv[1]};
      auto injected = entry;
      injected.construct = injectedConstruct;
      auto task = core->submit(group, injected, {entry.packetLayout, &packet, sizeof(packet)});
      CHECK(task && core->drain());
      auto failure = core->failure(task);
      if (failure.kind == TaskFailure::None) {
        auto first = observe(*core, task), second = observe(*core, task);
        CHECK(first.hostBytes()->data() == second.hostBytes()->data());
        // Returned CPU backing survives the invocation, scheduler and all GPU intermediates.
        retire(*core, group);
        core.reset();
        verify(first);
        verify(second);
        first = {};
        second = {};
        success = true;
      } else {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
        retire(*core, group);
        ++failures;
      }
      empty(*runtime);
      if (success)
        break;
    }
#ifdef KALANDRA_SCALAR_ENTRY
    CHECK(success && failures == 4);
#else
    CHECK(success && failures >= 8);
#endif
    std::printf("%u workers: %u CPU allocation failures unwound\n", workers, failures);
  }
  failureAttempt = 0;
  // Include CPU-result lease construction in the Metal allocation failure sweep.
  runtime->failAllocationAt(0);
  auto run = [&](bool succeeds) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    if (succeeds) {
      CHECK(core->failure(task).kind == TaskFailure::None);
      verify(observe(*core, task));
    } else
      CHECK(core->failure(task).kind != TaskFailure::None);
    retire(*core, group);
    empty(*runtime);
  };
  run(true);
  auto attempts = runtime->stats().attempts;
  CHECK(attempts > 0);
  for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
    runtime->failAllocationAt(fail);
    run(false);
  }
  runtime->failAllocationAt(0);
  for (bool sharedUploads : {false, true}) {
    auto scenarioMade = m::Runtime::create({.sharedUploads = sharedUploads});
    CHECK(scenarioMade);
    auto runtime = std::move(*scenarioMade);
    auto target = runtime->device();
    CHECK(target);
#if defined(KALANDRA_ARGUMENTS) || defined(KALANDRA_UNUSED_INPUTS)
    constexpr unsigned uploads = 3;
#elif defined(KALANDRA_SCALAR_ENTRY)
    constexpr unsigned uploads = 1;
#else
    constexpr unsigned uploads = 2;
#endif
    // A shared upload is a CPU copy, not a command-buffer completion.
    const auto computeOffset = (sharedUploads && target->hasUnifiedMemory() ? 0u : uploads) + 1;
    for (unsigned scenario : {0u, 1u, 2u}) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      PacketData packet{runtime.get(), argv[1]};
      const auto compute = runtime->stats().submissions + computeOffset;
      runtime->holdCompletionAt(compute);
      auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
      CHECK(task);
      until(*core, [&] {
        return runtime->stats().submissions == compute && core->stats().externalPending == 1;
      });
      if (scenario == 2)
        runtime->failNextCompletion();
      if (scenario < 2)
        CHECK(core->cancel(group, scenario == 1));
      runtime->holdCompletionAt(0);
      CHECK(core->drain());
      CHECK(core->failure(task).kind != TaskFailure::None);
      retire(*core, group);
      empty(*runtime);
    }
  }
#ifdef KALANDRA_MATRIX_ENTRY
  matrixConformance(*runtime, argv[1]);
#endif
#ifdef KALANDRA_SHARED_HELPERS
  sharedConformance(*runtime, argv[1]);
#endif
#ifdef KALANDRA_SCALAR_ENTRY
  scalarConformance(*runtime, argv[1]);
#endif
#ifdef KALANDRA_DIFFERENCES
  differenceConformance<1>(*runtime, argv[1]);
  differenceConformance<2>(*runtime, argv[1]);
#endif
#ifdef KALANDRA_ARGUMENTS
  argumentConformance<1>(*runtime, argv[1]);
  argumentConformance<2>(*runtime, argv[1]);
  argumentLifecycle(*runtime, argv[1]);
#endif
#ifdef KALANDRA_UNUSED_INPUTS
  unusedInputConformance(*runtime, argv[1]);
#endif
#ifdef KALANDRA_INDIRECT_ARGUMENTS
  indirectConformance<1>(*runtime, argv[1]);
  indirectConformance<2>(*runtime, argv[1]);
  indirectLifecycle(*runtime, argv[1]);
#endif
#ifdef KALANDRA_TENSOR_NEGATION
  tensorNegationConformance<1>(*runtime, argv[1]);
  tensorNegationConformance<2>(*runtime, argv[1]);
#endif
#ifdef KALANDRA_DIVISION
  divisionConformance<1>(*runtime, argv[1]);
  divisionConformance<2>(*runtime, argv[1]);
#endif
  // The retained CPU results share the registry budget with transfers and GPU work.
  auto limited = m::Runtime::create({.maxBytes = 16});
  CHECK(limited);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  PacketData packet{limited->get(), argv[1]};
  auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
  CHECK(task && core->drain());
  CHECK(core->failure(task).kind == TaskFailure::Fault &&
        core->failure(task).code == std::int64_t(m::Error::Budget));
  retire(*core, group);
  empty(**limited);
  std::puts(
      "CPU tensor return ownership, cached results, failure cleanup and GPU cancellation passed");
}
