#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Reduction assertion %d: %s\n", __LINE__, #x);                          \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
void empty(m::Runtime &r) {
  auto s = r.stats();
  CHECK(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings && !s.stagingBuffers &&
        !s.inFlight);
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
  float value = 0;
  CHECK(core.observe(task, m::Runtime::scalarLayout(), context, &value));
  return std::bit_cast<std::uint32_t>(value);
}
m::Buffer upload(m::Runtime &runtime, std::span<const std::uint32_t> bits,
                 std::span<const std::int64_t> shape = {}) {
  const std::int64_t n = bits.size();
  auto host = runtime.hostTensor(std::as_bytes(bits), shape.empty() ? std::span{&n, 1} : shape,
                                 m::Format::Float32);
  CHECK(host);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto device = runtime.device();
  CHECK(device);
  auto task = runtime.upload(*core, group, *host, *device);
  CHECK(task && core->drain());
  auto result = buffer(*core, task);
  retire(*core, group);
  return result;
}
std::uint32_t oracle(std::vector<std::uint32_t> values) {
  if (values.empty())
    return 0;
  while (values.size() > 1) {
    const auto n = values.size();
    for (std::size_t i = 0; i < n / 2; ++i)
      values[i] = float_proof::oracle({values[i * 2], values[i * 2 + 1]}, 0);
    if (n % 2)
      values[n / 2] = values.back();
    values.resize((n + 1) / 2);
  }
  return values[0];
}
template <class F> void until(TaskCore &core, F condition) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!condition()) {
    core.step();
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
std::uint64_t invocationFailure = 0;
bool injectedConstruct(Invocation &context, void *out, const Packet &packet) {
  context.failAllocationAt(invocationFailure);
  return construct(context, out, packet);
}
void sourceChecks(m::Runtime &runtime, const char *directory) {
  for (unsigned workers : {1u, 2u, 4u}) {
    unsigned failures = 0;
    bool succeeded = false;
    for (invocationFailure = 1; invocationFailure <= 64; ++invocationFailure) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      PacketData packet{&runtime, directory};
      auto injected = entry;
      injected.construct = injectedConstruct;
      auto task = core->submit(group, injected, {entry.packetLayout, &packet, sizeof(packet)});
      CHECK(task && core->drain());
      auto failure = core->failure(task);
      if (failure.kind == TaskFailure::None) {
        CHECK(scalar(*core, task) == 0x41f80000u && scalar(*core, task) == 0x41f80000u);
        succeeded = true;
      } else {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
        ++failures;
      }
      retire(*core, group);
      empty(runtime);
      if (succeeded)
        break;
    }
    CHECK(succeeded && failures >= 2);
  }
  invocationFailure = 0;
}
void conformance(m::Runtime &runtime, const m::Kernel &kernel) {
  std::uint64_t cases = 0, elements = 0;
  auto run = [&](std::vector<std::uint32_t> values,
                 std::optional<std::array<std::int64_t, 2>> shape = {}) {
    const auto expected = oracle(values);
    auto input =
        upload(runtime, values, shape ? std::span(*shape) : std::span<const std::int64_t>{});
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = runtime.stats().submissions;
    auto task = runtime.reduceSum(*core, group, kernel, input);
    CHECK(task && core->drain());
    auto result = scalar(*core, task);
    CHECK(float_proof::equal(result, expected));
    CHECK(scalar(*core, task) == result);
    CHECK(runtime.stats().submissions == before + (values.empty() ? 0 : 1));
    if (values.size() == 1)
      CHECK(result == values[0]);
    Invocation context;
    const auto n = std::int64_t(values.size());
    auto *data = static_cast<float *>(context.allocate(n * 4));
    CHECK(data);
    if (n)
      std::memcpy(data, values.data(), n * 4);
    float cpu;
    if (shape) {
      Float32Tensor<2> descriptor{data, data, 0, {(*shape)[0], (*shape)[1]}, {(*shape)[1], 1}};
      cpu = _mlir_ciface_kalandra_cpu_3(&descriptor, &context);
    } else {
      Float32Tensor<1> descriptor{data, data, 0, {n}, {1}};
      cpu = _mlir_ciface_kalandra_cpu_0(&descriptor, &context);
    }
    CHECK(!context.status());
    CHECK(float_proof::equal(std::bit_cast<std::uint32_t>(cpu), expected));
    CHECK(!n || std::memcmp(data, values.data(), n * 4) == 0);
    context.release(data);
    CHECK(!context.live());
    auto download = runtime.download(*core, group, input);
    CHECK(download && core->drain());
    auto host = buffer(*core, download);
    auto bytes = host.hostBytes();
    CHECK(bytes && bytes->size() == values.size() * 4);
    CHECK(values.empty() || std::memcmp(bytes->data(), values.data(), bytes->size()) == 0);
    retire(*core, group);
    ++cases;
    elements += values.size();
  };
  const std::array<std::uint32_t, 10> edges{0u,          0x80000000u, 1u,          0x80000001u,
                                            0x007fffffu, 0x00800000u, 0x7f800000u, 0xff800000u,
                                            0x7fc12345u, 0x7f800001u};
  for (auto bits : edges) {
    run({bits});
    run({bits}, std::array<std::int64_t, 2>{1, 1});
  }
  for (auto a : edges)
    for (auto b : edges)
      run({a, b});
  std::uint32_t seed = 0x17219abc;
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  for (std::size_t n :
       {0u,    1u,    2u,    3u,    4u,    5u,    7u,     8u,     9u,     31u,    32u,   33u,
        63u,   64u,   65u,   127u,  128u,  129u,  255u,   256u,   257u,   1023u,  1024u, 1025u,
        4095u, 4096u, 4097u, 8191u, 8192u, 8193u, 65535u, 65536u, 65537u, 131073u}) {
    for (unsigned pattern = 0; pattern < 6; ++pattern) {
      std::vector<std::uint32_t> values(n);
      for (std::size_t i = 0; i < n; ++i) {
        auto bits = random();
        if (pattern == 0)
          bits = (bits & 0x807fffffu) | ((120u + (bits % 15u)) << 23);
        if (pattern == 1)
          bits = 0x80000000u;
        if (pattern == 2)
          bits = (bits & 0x807fffffu);
        if (pattern == 3)
          bits = std::array{0x7f7fffffu, 0x7f7fffffu, 0xff7fffffu}[i % 3];
        if (pattern == 4)
          bits = std::array{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu, 0x3f800000u}[i % 4];
        if (pattern == 5)
          bits = edges[i % edges.size()];
        values[i] = bits;
      }
      run(std::move(values));
    }
  }
  for (unsigned trial = 0; trial < 256; ++trial) {
    std::vector<std::uint32_t> values(random() % 8194);
    for (auto &bits : values) {
      auto value = random();
      bits = (value & 0x807fffffu) | ((64u + (value % 127u)) << 23);
    }
    run(std::move(values));
  }
  const std::vector<std::uint32_t> crossing{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu,
                                            0x3f800000u, 0x3f800000u, 0x3f800000u};
  CHECK(oracle(crossing) == 0x40000000u); // Per-row reduction would incorrectly yield 3.
  run(crossing, std::array<std::int64_t, 2>{2, 3});
  // Flat adjacent pairs cross row boundaries when columns are odd; never sum each row first.
  for (auto shape :
       {std::array{0, 0}, std::array{0, 7}, std::array{7, 0}, std::array{1, 1}, std::array{1, 65},
        std::array{65, 1}, std::array{2, 3}, std::array{3, 3}, std::array{3, 5}, std::array{17, 65},
        std::array{65, 17}, std::array{257, 513}}) {
    for (unsigned pattern = 0; pattern < 6; ++pattern) {
      std::vector<std::uint32_t> values(shape[0] * shape[1]);
      for (std::size_t i = 0; i < values.size(); ++i) {
        auto bits = random();
        if (pattern == 0)
          bits = (bits & 0x807fffffu) | ((120u + bits % 15u) << 23);
        if (pattern == 1)
          bits = 0x80000000u;
        if (pattern == 2)
          bits &= 0x807fffffu;
        if (pattern == 3)
          bits = std::array{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu, 0x3f800000u}[i % 4];
        if (pattern == 4)
          bits = edges[i % edges.size()];
        values[i] = bits; // Pattern 5 includes arbitrary encodings and NaN payloads.
      }
      run(std::move(values), std::array<std::int64_t, 2>{shape[0], shape[1]});
    }
  }
  empty(runtime);
  std::printf("Reduction: %llu CPU/GPU/reference cases, %llu input elements, zero mismatches\n",
              (unsigned long long)cases, (unsigned long long)elements);
}
void lifecycle(m::Runtime &runtime, const m::Kernel &kernel, bool matrix = false) {
  std::vector<std::uint32_t> values(65, 0x3f800000u);
  const std::array<std::int64_t, 2> shape{5, 13};
  auto input = upload(runtime, values, matrix ? std::span(shape) : std::span<const std::int64_t>{});
  const auto baseline = runtime.stats().bytes;
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto task = runtime.reduceSum(*core, group, kernel, input);
    CHECK(task && core->drain());
    if (success)
      CHECK(scalar(*core, task) == 0x42820000u);
    else
      CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 2);
    retire(*core, group);
    CHECK(runtime.stats().bytes == baseline && !runtime.stats().stagingBuffers &&
          !runtime.stats().inFlight);
  };
  runtime.failAllocationAt(0);
  run(true);
  auto attempts = runtime.stats().attempts;
  CHECK(attempts > 8);
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    runtime.failAllocationAt(i);
    run(false);
  }
  runtime.failAllocationAt(0);
  for (unsigned scenario : {0u, 1u, 2u}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    runtime.holdCompletions(true);
    if (scenario == 2)
      runtime.failNextCompletion();
    auto task = runtime.reduceSum(*core, group, kernel, input);
    CHECK(task);
    until(*core, [&] { return core->stats().externalPending == 1; });
    // 65 -> 33 -> 17 -> 9 -> 5 -> 3 -> 2 -> 1 reuses 33- and 17-word outputs.
    // Seven immutable parameter words and one scalar readback remain live.
    // Apple-family devices share one parameter allocation; other GPUs keep
    // offset-zero bindings. The payload budget is identical for either layout.
    auto device = runtime.device();
    CHECK(device);
    const auto staging = device->name().starts_with("Apple") ? 2u : 8u;
    CHECK(runtime.stats().bytes == baseline + 200 + 28 + 4 &&
          runtime.stats().stagingBuffers == staging && runtime.stats().metalBuffers == 3 + staging);
    if (scenario < 2)
      CHECK(core->cancel(group, scenario == 1));
    runtime.holdCompletions(false);
    CHECK(core->drain());
    if (scenario < 2) {
      CHECK(core->failure(task).kind == TaskFailure::Cancelled);
      CHECK(core->failure(group).kind ==
            (scenario == 1 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    } else
      CHECK(core->failure(task).kind == TaskFailure::Fault &&
            core->failure(task).code == std::int64_t(m::Error::Device));
    Invocation context;
    float unpublished = 0;
    CHECK(!core->observe(task, m::Runtime::scalarLayout(), context, &unpublished));
    retire(*core, group);
    CHECK(runtime.stats().bytes == baseline && !runtime.stats().stagingBuffers &&
          !runtime.stats().inFlight);
  }
  // Destruction waits for acknowledgement while a task owns reused scratch buffers.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    runtime.holdCompletions(true);
    runtime.reduceSum(*core, group, kernel, input);
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      runtime.holdCompletions(false);
    });
    core.reset();
    release.join();
    CHECK(runtime.stats().bytes == baseline && !runtime.stats().inFlight &&
          !runtime.stats().stagingBuffers);
  }
  std::printf("Reduction rank-%u allocation checkpoints swept: %llu\n", matrix ? 2 : 1,
              (unsigned long long)attempts);
}
void concurrent(m::Runtime &runtime, const m::Kernel &kernel) {
  auto a = upload(runtime, std::vector<std::uint32_t>(65, 0x3f800000u));
  const std::array<std::int64_t, 2> shape{3, 43};
  auto b = upload(runtime, std::vector<std::uint32_t>(129, 0x40000000u), shape);
  const auto baseline = runtime.stats().bytes;
  auto first = TaskCore::create(), second = TaskCore::create();
  CHECK(first && second);
  auto ga = first->openRoot(), gb = second->openRoot();
  runtime.holdCompletions(true);
  auto ta = runtime.reduceSum(*first, ga, kernel, a);
  auto tb = runtime.reduceSum(*second, gb, kernel, b);
  CHECK(ta && tb);
  until(*first, [&] { return first->stats().externalPending == 1; });
  until(*second, [&] { return second->stats().externalPending == 1; });
  // Simultaneous invocations own independent output pairs (33/17 and 65/33 words).
  CHECK(runtime.stats().bytes == baseline + 232 + 428 && runtime.stats().inFlight == 2);
  CHECK(first->cancel(ga));
  runtime.holdCompletions(false);
  CHECK(first->drain() && second->drain());
  CHECK(first->failure(ta).kind == TaskFailure::Cancelled);
  CHECK(scalar(*second, tb) == 0x43810000u);
  retire(*first, ga);
  retire(*second, gb);
  CHECK(runtime.stats().bytes == baseline && !runtime.stats().stagingBuffers &&
        !runtime.stats().inFlight);
}
void admission(m::Runtime &runtime, const m::Kernel &kernel, const char *directory) {
  auto device = runtime.device();
  CHECK(device);
  auto elementwise =
      runtime.loadElementwiseKernel(std::string(directory) + "/kernel0.metallib", *device, {1, 1});
  CHECK(elementwise);
  std::array<std::uint32_t, 2> bits{0x3f800000u, 0x40000000u};
  const std::int64_t shape[] = {1, 2};
  auto host =
      runtime.hostTensor(std::as_bytes(std::span(bits)), {shape + 1, 1}, m::Format::Float32);
  CHECK(host);
  const std::int64_t rankedShape[] = {1, 1, 2};
  auto ranked = runtime.hostTensor(std::as_bytes(std::span(bits)), rankedShape, m::Format::Float32);
  CHECK(ranked);
  const std::int64_t rank8[] = {1, 1, 1, 1, 1, 1, 1, 2};
  auto maximumRank = runtime.hostTensor(std::as_bytes(std::span(bits)), rank8, m::Format::Float32);
  CHECK(maximumRank);
  const std::int64_t rank9[] = {1, 1, 1, 1, 1, 1, 1, 1, 2};
  CHECK(!runtime.hostTensor(std::as_bytes(std::span(bits)), rank9, m::Format::Float32));
  auto integers =
      runtime.hostTensor(std::as_bytes(std::span(bits)), {shape + 1, 1}, m::Format::UInt32);
  CHECK(integers);
  auto scalarHost =
      runtime.hostTensor(std::as_bytes(std::span(bits).first(1)), {}, m::Format::Float32);
  CHECK(scalarHost);
  auto valid = upload(runtime, bits);
  auto reject = [&](m::Kernel pipeline, m::Buffer input, std::int64_t code) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = runtime.stats().submissions;
    auto task = runtime.reduceSum(*core, group, pipeline, input);
    CHECK(task && core->drain());
    if (core->failure(task).kind != TaskFailure::Fault || core->failure(task).code != code)
      std::fprintf(stderr, "reduction admission rank=%zu: actual kind=%u code=%lld expected=%lld\n",
                   input.shape().size(), unsigned(core->failure(task).kind),
                   (long long)core->failure(task).code, (long long)code);
    CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == code);
    CHECK(runtime.stats().submissions == before);
    retire(*core, group);
  };
  reject({}, valid, 3);
  reject(kernel, {}, 3);
  reject(*elementwise, valid, 3);
  reject(kernel, *host, 7);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto a = runtime.upload(*core, group, *ranked, *device),
       b = runtime.upload(*core, group, *integers, *device);
  auto c = runtime.upload(*core, group, *scalarHost, *device);
  auto d = runtime.upload(*core, group, *maximumRank, *device);
  CHECK(core->drain());
  reject(kernel, buffer(*core, c), 3);
  for (auto admitted : {a, d}) {
    const auto before = runtime.stats().submissions;
    auto sum = runtime.reduceSum(*core, group, kernel, buffer(*core, admitted));
    CHECK(sum && core->drain());
    CHECK(scalar(*core, sum) == 0x40400000u);
    CHECK(runtime.stats().submissions == before + 1);
  }
  reject(kernel, buffer(*core, b), 3);
  retire(*core, group);
  auto foreign = m::Runtime::create();
  CHECK(foreign);
  reject(kernel, upload(**foreign, bits), 7);
  // A reduction pipeline must not be admitted as an elementwise pipeline, even with matching slots.
  core = TaskCore::create();
  CHECK(core);
  group = core->openRoot();
  auto before = runtime.stats().submissions;
  auto task =
      runtime.elementwise(*core, group, kernel, std::array{valid}, std::span<const float>{});
  CHECK(task && core->drain());
  CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 3);
  CHECK(runtime.stats().submissions == before);
  retire(*core, group);
}
void droppedOwners(const char *directory) {
  auto made = m::Runtime::create();
  CHECK(made);
  auto runtime = std::move(*made);
  auto device = runtime->device();
  CHECK(device);
  auto kernel = runtime->loadReductionKernel(std::string(directory) + "/kernel1.metallib", *device);
  CHECK(kernel);
  auto input =
      upload(*runtime, std::array<std::uint32_t, 3>{0x3f800000u, 0x40000000u, 0x40400000u});
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto task = runtime->reduceSum(*core, group, *kernel, input);
  CHECK(task);
  input = {};
  *kernel = {};
  *device = {};
  runtime.reset();
  CHECK(core->drain() && scalar(*core, task) == 0x40c00000u);
  retire(*core, group);
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto &runtime = **made;
  sourceChecks(runtime, argv[1]);
  auto device = runtime.device();
  CHECK(device);
  auto path = std::string(argv[1]) + "/kernel1.metallib";
  auto kernel = runtime.loadReductionKernel(path, *device);
  CHECK(kernel);
  CHECK(!runtime.loadElementwiseKernel(path, *device, {1, 0}));
  CHECK(!runtime.loadReductionKernel(std::string(argv[1]) + "/kernel0.metallib", *device));
  admission(runtime, *kernel, argv[1]);
  empty(runtime);
  droppedOwners(argv[1]);
  conformance(runtime, *kernel);
  lifecycle(runtime, *kernel);
  lifecycle(runtime, *kernel, true);
  concurrent(runtime, *kernel);
  empty(runtime);
  // Force the scratch/readback budget after upload, without charging dead upload staging.
  auto limited = m::Runtime::create({.maxBytes = 1024});
  CHECK(limited);
  auto ld = (*limited)->device();
  CHECK(ld);
  auto lk = (*limited)->loadReductionKernel(path, *ld);
  CHECK(lk);
  for (unsigned paddingBytes : {532u, 533u}) {
    std::vector<std::uint32_t> values(65, 0x3f800000u);
    auto input = upload(**limited, values);
    // Input 260 + padding 532 + two outputs 200 + seven counts 28 + readback 4 = 1024.
    std::vector<std::byte> padding(paddingBytes);
    const std::int64_t n = padding.size();
    auto held = (*limited)->hostTensor(padding, {&n, 1}, m::Format::UInt8);
    CHECK(held);
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto task = (*limited)->reduceSum(*core, group, *lk, input);
    CHECK(task && core->drain());
    if (paddingBytes == 532)
      CHECK(scalar(*core, task) == 0x42820000u);
    else
      CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 6);
    retire(*core, group);
    CHECK((*limited)->stats().bytes == 260 + paddingBytes && !(*limited)->stats().stagingBuffers);
  }
  empty(**limited);
  auto bounded = m::Runtime::create({.maxInFlight = 1});
  CHECK(bounded);
  auto bd = (*bounded)->device();
  CHECK(bd);
  auto bk = (*bounded)->loadReductionKernel(path, *bd);
  CHECK(bk);
  {
    auto input =
        upload(**bounded, std::array<std::uint32_t, 3>{0x3f800000u, 0x40000000u, 0x40400000u});
    auto first = TaskCore::create(), second = TaskCore::create();
    CHECK(first && second);
    auto ga = first->openRoot(), gb = second->openRoot();
    (*bounded)->holdCompletions(true);
    auto a = (*bounded)->reduceSum(*first, ga, *bk, input);
    CHECK(a);
    until(*first, [&] { return first->stats().externalPending == 1; });
    auto b = (*bounded)->reduceSum(*second, gb, *bk, input);
    CHECK(b && second->drain());
    CHECK(second->failure(b).kind == TaskFailure::Fault && second->failure(b).code == 6);
    retire(*second, gb);
    (*bounded)->holdCompletions(false);
    CHECK(first->drain() && scalar(*first, a) == 0x40c00000u);
    retire(*first, ga);
  }
  empty(**bounded);
  std::puts("GPU fixed-tree reduction, scalar results and failure/cancellation cleanup passed");
}
