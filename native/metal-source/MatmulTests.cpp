#define main generated_main
#include "host.cpp"
#undef main
#include "../metal-proof/FloatOracle.h"
#include "../metal/MatmulLayout.h"
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Matmul assertion %d: %s\n", __LINE__, #x);                             \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
void empty(m::Runtime &r) {
  auto s = r.stats();
  CHECK(!s.bytes && !s.inFlight && !s.metalBuffers && !s.stagingBuffers && !s.hostBackings &&
        !s.deviceBackings);
}
void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
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
m::Buffer upload(m::Runtime &runtime, std::span<const std::uint32_t> bits,
                 std::array<std::int64_t, 2> shape, m::Format format = m::Format::Float32) {
  auto host = runtime.hostTensor(std::as_bytes(bits), shape, format);
  CHECK(host);
  auto d = runtime.device();
  CHECK(d);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto task = runtime.upload(*core, group, *host, *d);
  CHECK(task && core->drain());
  auto result = observe(*core, task);
  retire(*core, group);
  return result;
}
std::uint32_t sum(std::vector<std::uint32_t> values) {
  if (values.empty())
    return 0;
  while (values.size() > 1) {
    auto n = values.size();
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
std::uint64_t failInvocation = 0;
bool injectedConstruct(Invocation &context, void *out, const Packet &packet) {
  context.failAllocationAt(failInvocation);
  return construct(context, out, packet);
}
void sourceChecks(m::Runtime &runtime, const char *directory) {
  for (unsigned workers : {1u, 2u, 4u}) {
    unsigned failures = 0;
    bool success = false;
    for (failInvocation = 1; failInvocation < 64; ++failInvocation) {
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
      if (core->failure(task).kind == TaskFailure::None) {
        Invocation c;
        float a, b;
        CHECK(core->observe(task, scalarLayout, c, &a) && core->observe(task, scalarLayout, c, &b));
        CHECK(std::bit_cast<std::uint32_t>(a) == 0x43230000u && a == b);
        success = true;
      } else {
        CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 2);
        ++failures;
      }
      retire(*core, group);
      empty(runtime);
      if (success)
        break;
    }
    CHECK(success && failures >= 5);
  }
  failInvocation = 0;
}
void conformance(m::Runtime &runtime, const m::Kernel &kernel) {
  std::uint32_t seed = 0x61dfc0ab;
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  std::uint64_t cases = 0, cells = 0, products = 0;
  auto run = [&](std::int64_t m, std::int64_t k, std::int64_t n, unsigned pattern) {
    std::vector<std::uint32_t> a(m * k), b(k * n);
    auto fill = [&](std::vector<std::uint32_t> &values, bool left) {
      for (std::size_t i = 0; i < values.size(); ++i) {
        auto r = random(), bits = (r & 0x807fffffu) | ((120u + r % 15u) << 23);
        if (pattern == 1)
          bits = left ? 0x80000000u : 0x3f800000u;
        if (pattern == 2)
          bits = left ? r & 0x807fffffu : std::array{0x3f800000u, 0x3f000000u, 0x3f800001u}[i % 3];
        if (pattern == 3)
          bits = std::array{0x7f800000u, 0xff800000u, 0x7fc12345u,
                            0x7f800001u, 0x00000000u, 0x80000000u}[i % 6];
        if (pattern == 4)
          bits = left ? std::array{0x60ad78ecu, 0x3f800000u, 0xe0ad78ecu, 0x3f800000u}[i % 4]
                      : 0x3f800000u;
        if (pattern == 5)
          bits = r;
        values[i] = bits;
      }
    };
    fill(a, true);
    fill(b, false);
    if (pattern == 6) {
      CHECK(m == 1 && k == 2 && n == 1);
      a = {0x3f800001u, 0x3f800000u};
      b = {0xbf7ffffeu, 0x3f800000u};
    }
    Invocation context;
    auto *pa = static_cast<float *>(context.allocate(a.size() * 4)),
         *pb = static_cast<float *>(context.allocate(b.size() * 4));
    CHECK(pa && pb);
    if (!a.empty())
      std::memcpy(pa, a.data(), a.size() * 4);
    if (!b.empty())
      std::memcpy(pb, b.data(), b.size() * 4);
    Float32Tensor<2> da{pa, pa, 0, {m, k}, {k, 1}}, db{pb, pb, 0, {k, n}, {n, 1}}, cpu{};
    _mlir_ciface_kalandra_cpu_0(&cpu, &da, &db, &context);
    CHECK(!context.status() && cpu.sizes[0] == m && cpu.sizes[1] == n);
    auto ga = upload(runtime, a, {m, k}), gb = upload(runtime, b, {k, n});
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = runtime.stats().submissions;
    auto task = runtime.matmul(*core, group, kernel, ga, gb);
    CHECK(task && core->drain());
    auto result = observe(*core, task), cached = observe(*core, task);
    CHECK(result.onDevice() && result.shape().size() == 2 && result.shape()[0] == m &&
          result.shape()[1] == n);
    CHECK(runtime.stats().submissions == before + (m * n ? 1 : 0));
    auto download = runtime.download(*core, group, result);
    CHECK(download && core->drain());
    auto host = observe(*core, download);
    auto bytes = host.hostBytes();
    CHECK(bytes && bytes->size() == std::size_t(m * n * 4));
    for (std::int64_t row = 0; row < m; ++row)
      for (std::int64_t col = 0; col < n; ++col) {
        std::vector<std::uint32_t> terms(k);
        for (std::int64_t i = 0; i < k; ++i)
          terms[i] = float_proof::oracle({a[row * k + i], b[i * n + col]}, 1);
        auto expected = sum(std::move(terms));
        std::uint32_t cb, rb;
        std::memcpy(&cb, cpu.aligned + cpu.offset + row * cpu.strides[0] + col * cpu.strides[1], 4);
        std::memcpy(&rb, bytes->data() + (row * n + col) * 4, 4);
        CHECK(float_proof::equal(cb, expected) && float_proof::equal(rb, expected));
        ++cells;
      }
    // Device inputs remain unchanged after all products and reductions.
    auto checkInput = [&](m::Buffer value, const std::vector<std::uint32_t> &expected) {
      auto back = runtime.download(*core, group, value);
      CHECK(back && core->drain());
      auto host = observe(*core, back);
      auto data = host.hostBytes();
      CHECK(data && data->size() == expected.size() * 4);
      CHECK(expected.empty() || std::memcmp(data->data(), expected.data(), data->size()) == 0);
    };
    checkInput(ga, a);
    checkInput(gb, b);
    CHECK(a.empty() || std::memcmp(pa, a.data(), a.size() * 4) == 0);
    CHECK(b.empty() || std::memcmp(pb, b.data(), b.size() * 4) == 0);
    retire(*core, group);
    CHECK(cached.onDevice() && host.hostBytes());
    context.release(cpu.allocated);
    context.release(pa);
    context.release(pb);
    CHECK(!context.live());
    ++cases;
    products += m * k * n;
  };
  for (auto k :
       {0u,  1u,  2u,  3u,   4u,   5u,   7u,   8u,   9u,   15u,  16u,  17u,  31u,   32u,   33u,
        63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 511u, 512u, 513u, 1023u, 1024u, 1025u})
    for (auto shape : {std::array{1u, 1u}, std::array{3u, 5u}, std::array{8u, 9u}})
      for (unsigned pattern = 0; pattern < 6; ++pattern)
        run(shape[0], k, shape[1], pattern);
  for (auto shape : {std::array{0u, 0u, 0u}, std::array{0u, 3u, 5u}, std::array{4u, 3u, 0u},
                     std::array{4u, 0u, 5u}, std::array{1u, 3u, 63u}, std::array{1u, 3u, 64u},
                     std::array{1u, 3u, 65u}})
    run(shape[0], shape[1], shape[2], 0);
  run(1, 2, 1, 6);
  for (unsigned i = 0; i < 128; ++i) {
    auto m = 1 + random() % 7, k = random() % 258, n = 1 + random() % 11;
    run(m, k, n, 0);
  }
  // Actual managed dispatch across both profile paths, with partial tiles.
  run(257, 128, 257, 5);
  run(513, 256, 512, 5);
  empty(runtime);
  std::printf("Matmul: %llu cases, %llu cells, %llu products matched CPU/GPU/APFloat\n",
              (unsigned long long)cases, (unsigned long long)cells, (unsigned long long)products);
}
void lifecycle(m::Runtime &runtime, const m::Kernel &kernel, unsigned rows = 3, unsigned inner = 5,
               unsigned columns = 7) {
  auto a = upload(runtime, std::vector<std::uint32_t>(rows * inner, 0x3f800000u), {rows, inner}),
       b = upload(runtime, std::vector<std::uint32_t>(inner * columns, 0x3f800000u),
                  {inner, columns});
  const auto bytes = std::uint64_t(rows) * columns * 4;
  auto baseline = runtime.stats().bytes;
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto task = runtime.matmul(*core, group, kernel, a, b);
    CHECK(task && core->drain());
    if (success) {
      auto result = observe(*core, task);
      CHECK(result.bytes() == bytes);
    } else
      CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 2);
    retire(*core, group);
    CHECK(runtime.stats().bytes == baseline && !runtime.stats().stagingBuffers &&
          !runtime.stats().inFlight);
  };
  runtime.failAllocationAt(0);
  run(true);
  auto attempts = runtime.stats().attempts;
  CHECK(attempts >= 8);
  for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
    runtime.failAllocationAt(fail);
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
    auto task = runtime.matmul(*core, group, kernel, a, b);
    CHECK(task);
    until(*core, [&] { return core->stats().externalPending == 1; });
    CHECK(runtime.stats().bytes == baseline + bytes + 12 && runtime.stats().stagingBuffers == 1);
    if (scenario < 2)
      CHECK(core->cancel(group, scenario == 1));
    runtime.holdCompletions(false);
    CHECK(core->drain());
    if (scenario < 2)
      CHECK(core->failure(task).kind == TaskFailure::Cancelled &&
            core->failure(group).kind ==
                (scenario == 1 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    else
      CHECK(core->failure(task).kind == TaskFailure::Fault &&
            core->failure(task).code == std::int64_t(m::Error::Device));
    Invocation c;
    alignas(m::Buffer) std::byte out[sizeof(m::Buffer)];
    CHECK(!core->observe(task, m::Runtime::bufferLayout(), c, out));
    retire(*core, group);
    CHECK(runtime.stats().bytes == baseline && !runtime.stats().stagingBuffers &&
          !runtime.stats().inFlight);
  }
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    runtime.holdCompletions(true);
    runtime.matmul(*core, group, kernel, a, b);
    until(*core, [&] { return core->stats().externalPending == 1; });
    std::thread release([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      runtime.holdCompletions(false);
    });
    core.reset();
    release.join();
    CHECK(runtime.stats().bytes == baseline && !runtime.stats().inFlight);
  }
  std::printf("Matmul allocation checkpoints swept: %llu\n", (unsigned long long)attempts);
}
void admission(m::Runtime &runtime, const m::Kernel &kernel) {
  const std::array<std::uint32_t, 4> bits{0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
  auto input = upload(runtime, bits, {2, 2});
  auto reject = [&](m::Kernel selected, m::Buffer a, m::Buffer b, std::int64_t code) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto before = runtime.stats().submissions;
    auto task = runtime.matmul(*core, group, selected, a, b);
    CHECK(task && core->drain());
    CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == code);
    CHECK(runtime.stats().submissions == before);
    retire(*core, group);
  };
  reject({}, input, input, 3);
  reject(kernel, {}, input, 3);
  reject(kernel, input, {}, 3);
  auto wrong = upload(runtime, bits, {1, 4});
  reject(kernel, input, wrong, 3);
  auto integers = upload(runtime, bits, {2, 2}, m::Format::UInt32);
  reject(kernel, integers, input, 3);
  auto host = runtime.hostTensor(std::as_bytes(std::span(bits)), std::array<std::int64_t, 2>{2, 2},
                                 m::Format::Float32);
  CHECK(host);
  reject(kernel, *host, input, 7);
  const std::int64_t length = bits.size();
  auto vectorHost =
      runtime.hostTensor(std::as_bytes(std::span(bits)), {&length, 1}, m::Format::Float32);
  CHECK(vectorHost);
  auto device = runtime.device();
  CHECK(device);
  auto vectorCore = TaskCore::create();
  CHECK(vectorCore);
  auto vectorGroup = vectorCore->openRoot();
  auto uploaded = runtime.upload(*vectorCore, vectorGroup, *vectorHost, *device);
  CHECK(uploaded && vectorCore->drain());
  auto vector = observe(*vectorCore, uploaded);
  retire(*vectorCore, vectorGroup);
  reject(kernel, vector, input, 3);
  reject(kernel, input, vector, 3);
  auto foreign = m::Runtime::create();
  CHECK(foreign);
  reject(kernel, upload(**foreign, bits, {2, 2}), input, 7);
  auto wide = upload(runtime, {}, {0, 4294967296ll}), tall = upload(runtime, {}, {4294967296ll, 0});
  reject(kernel, wide, tall, 3);
  auto a = upload(runtime, {}, {65536, 0}), b = upload(runtime, {}, {0, 65536});
  reject(kernel, a, b, 3);
  // Aliasing is valid: inputs remain immutable and output has fresh storage.
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto task = runtime.matmul(*core, group, kernel, input, input);
  CHECK(task && core->drain());
  auto result = observe(*core, task);
  auto down = runtime.download(*core, group, result);
  CHECK(down && core->drain());
  auto copied = observe(*core, down);
  auto bytes = copied.hostBytes();
  CHECK(bytes && bytes->size() == 16);
  const std::array<std::uint32_t, 4> expected{0x40e00000u, 0x41200000u, 0x41700000u, 0x41b00000u};
  CHECK(std::memcmp(bytes->data(), expected.data(), 16) == 0);
  retire(*core, group);
  // Kernel-kind admission is independent of matching tensor/buffer slots.
  core = TaskCore::create();
  CHECK(core);
  group = core->openRoot();
  auto before = runtime.stats().submissions;
  task = runtime.elementwise(*core, group, kernel, input, input);
  CHECK(task && core->drain());
  CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 3);
  CHECK(runtime.stats().submissions == before);
  retire(*core, group);
}
void limitsAndOwners(const char *directory) {
  const auto path = std::string(directory) + "/kernel0.metallib";
  auto made = m::Runtime::create({.maxBytes = 512, .maxInFlight = 1});
  CHECK(made);
  auto &runtime = **made;
  auto device = runtime.device();
  CHECK(device);
  auto kernel = runtime.loadMatmulKernel(path, *device);
  CHECK(kernel);
  {
    auto a = upload(runtime, std::vector<std::uint32_t>(15, 0x3f800000u), {3, 5}),
         b = upload(runtime, std::vector<std::uint32_t>(35, 0x3f800000u), {5, 7});
    {
      std::array<std::byte, 220> padding{};
      const std::int64_t n = padding.size();
      auto held = runtime.hostTensor(padding, {&n, 1}, m::Format::UInt8);
      CHECK(held);
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      auto task = runtime.matmul(*core, group, *kernel, a, b);
      CHECK(task && core->drain());
      CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 6);
      retire(*core, group);
      CHECK(runtime.stats().bytes == 420 && !runtime.stats().stagingBuffers &&
            !runtime.stats().inFlight);
    }
    auto first = TaskCore::create(), second = TaskCore::create();
    CHECK(first && second);
    auto ga = first->openRoot(), gb = second->openRoot();
    runtime.holdCompletions(true);
    auto ta = runtime.matmul(*first, ga, *kernel, a, b);
    CHECK(ta);
    until(*first, [&] { return first->stats().externalPending == 1; });
    auto tb = runtime.matmul(*second, gb, *kernel, a, b);
    CHECK(tb && second->drain());
    CHECK(second->failure(tb).kind == TaskFailure::Fault && second->failure(tb).code == 6);
    retire(*second, gb);
    runtime.holdCompletions(false);
    CHECK(first->drain());
    auto value = observe(*first, ta);
    CHECK(value.bytes() == 84);
    value = {};
    retire(*first, ga);
  }
  empty(runtime);
  auto separate = m::Runtime::create();
  CHECK(separate);
  auto facade = std::move(*separate);
  device = facade->device();
  CHECK(device);
  kernel = facade->loadMatmulKernel(path, *device);
  CHECK(kernel);
  auto input = upload(*facade, std::array<std::uint32_t, 1>{0x40000000u}, {1, 1});
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto task = facade->matmul(*core, group, *kernel, input, input);
  CHECK(task);
  input = {};
  *kernel = {};
  *device = {};
  facade.reset();
  CHECK(core->drain());
  auto result = observe(*core, task);
  retire(*core, group);
  core.reset();
  CHECK(result.onDevice() && result.bytes() == 4);
}
void selection() {
  using L = m::MatmulLayout;
  CHECK(m::selectMatmulLayout(256, 128, 256) == L::Rows2);
  CHECK(m::selectMatmulLayout(257, 257, 257) == L::Rows2);
  CHECK(m::selectMatmulLayout(512, 256, 512) == L::Quad);
  CHECK(m::selectMatmulLayout(513, 513, 513) == L::Quad);
  CHECK(m::selectMatmulLayout(256, 127, 256) == L::Scalar);
  CHECK(m::selectMatmulLayout(256, 1026, 256) == L::Scalar);
  CHECK(m::selectMatmulLayout(255, 256, 257) == L::Scalar);
  CHECK(m::selectMatmulLayout(514, 256, 514) == L::Scalar);
  CHECK(m::selectMatmulLayout(1, 256, 65536) == L::Scalar);
  CHECK(m::selectMatmulLayout(65536, 256, 1) == L::Rows2);
  CHECK(m::selectMatmulLayout(512, 128, 512) == L::Rows2);
  CHECK(m::matmulWorkItems(L::Rows2, 257, 257) == 129 * 257);
  CHECK(m::matmulWorkItems(L::Quad, 513, 513) == 257 * 257);
  CHECK(m::matmulWorkItems(L::Scalar, 0xffffffffu, 1) == 0xffffffffu);
  CHECK(m::matmulWorkItems(L::Quad, 0xffffffffu, 1) == 0x80000000ull);
}
void bundleFailures(m::Runtime &runtime, const char *directory, m::Device device) {
  const auto path = std::string(directory) + "/kernel0.metallib";
  runtime.failAllocationAt(0);
  {
    auto value = runtime.loadMatmulKernel(path, device);
    CHECK(value);
  }
  auto attempts = runtime.stats().attempts;
  CHECK(attempts >= 2);
  for (std::uint64_t i = 1; i <= attempts; ++i) {
    runtime.failAllocationAt(i);
    auto failed = runtime.loadMatmulKernel(path, device);
    CHECK(!failed && failed.error() == m::Error::Allocation);
  }
  runtime.failAllocationAt(0);
  {
    auto value = runtime.loadMatmulKernel(path, device);
    CHECK(value);
  }
  std::printf("Matmul bundle preparation checkpoints swept: %llu\n", (unsigned long long)attempts);
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto &r = **made;
  sourceChecks(r, argv[1]);
  selection();
  auto device = r.device();
  CHECK(device);
  auto path = std::string(argv[1]) + "/kernel0.metallib";
  bundleFailures(r, argv[1], *device);
  auto kernel = r.loadMatmulKernel(path, *device);
  CHECK(kernel);
  CHECK(!r.loadElementwiseKernel(path, *device) && !r.loadReductionKernel(path, *device));
  admission(r, *kernel);
  empty(r);
  limitsAndOwners(argv[1]);
  conformance(r, *kernel);
  lifecycle(r, *kernel);
  lifecycle(r, *kernel, 256, 128, 256);
  lifecycle(r, *kernel, 512, 256, 512);
  empty(r);
  std::puts("Strict GPU matmul and rank-two source bridge checks passed");
}
