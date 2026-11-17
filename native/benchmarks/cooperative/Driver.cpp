// Compile this same driver against each snapshot's own headers and libraries.
// Timings include task admission, dispatch, observation and task retirement.
#if defined(BENCH_METAL)
#include "Metal.h"
namespace m = kalandra::runtime::metal;
#elif defined(BENCH_CUDA)
#include "Language.h"
namespace raw = kalandra::runtime::cuda;
namespace m = raw::language;
#else
#include "Language.h"
namespace raw = kalandra::runtime::vulkan;
namespace m = raw::language;
#endif
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>
#if !defined(BENCH_METAL) && !defined(BENCH_CUDA) && defined(BENCH_WORKGROUP)
#include "VulkanStencilCheck.h"
#endif
using namespace kalandra;
using namespace kalandra::runtime;
static void check(bool ok) {
  if (!ok)
    throw std::runtime_error("benchmark correctness/ownership check failed");
}
template <class T, class E> T take(std::expected<T, E> value) {
  check(bool(value));
  return std::move(*value);
}
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  check(core.failure(task).kind == TaskFailure::None);
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  check(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *v = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto value = *v;
  std::destroy_at(v);
  return value;
}
static void retire(TaskCore &core, GroupHandle group) {
  check(core.close(group) && core.drain() && core.release(group));
  auto s = core.stats();
  check(!s.records && !s.retainedBytes && !s.externalPending);
}
int main(int argc, char **argv) {
  try {
    if (argc != 5)
      return 2;
    const std::size_t n = std::stoull(argv[2]), count = std::stoull(argv[3]),
                      rounds = std::stoull(argv[4]);
    check(n > 0 && n <= 4194304 && count > 0 && rounds > 0);
    m::Options options;
    options.maxBytes = 128 * 1024 * 1024;
#if defined(BENCH_METAL)
    options.cachePreparedOutputs = false;
#endif
    auto runtime = take(m::Runtime::create(options));
    auto &r = *runtime;
    auto device = take(r.device());
    auto core = TaskCore::create({.workers = 1});
    check(bool(core));
    std::vector<float> input(n), expected(n);
    for (std::size_t i = 0; i < n; ++i)
      input[i] = float(int(i % 37) - 18) * 0.25f;
    for (std::size_t i = 0; i < n; ++i) {
#ifdef BENCH_WORKGROUP
      expected[i] = ((input[i ? i - 1 : 0] + input[i]) + input[std::min(i + 1, n - 1)]) / 3.0f;
#else
      expected[i] = (input[i] + 1.0f) * 2.0f;
#endif
    }
#if !defined(BENCH_METAL) && !defined(BENCH_CUDA) && defined(BENCH_WORKGROUP)
    std::vector<benchmark::StencilInterval> permitted(n);
    for (std::size_t i = 0; i < n; ++i)
      permitted[i] = benchmark::stencilInterval(input[i ? i - 1 : 0], input[i],
                                                input[std::min(i + 1, n - 1)], expected[i]);
#endif
    Invocation owner;
    const std::array shape{std::int64_t(n)};
    auto cpu = take(copyCpuTensor<NumericType::Float32, 1>(owner, NumericType::Float32,
                                                           std::as_bytes(std::span(input)), shape));
    auto host = take(r.fromCpu(owner, cpu.view()));
    auto uploadGroup = core->openRoot();
    auto upload = r.upload(*core, uploadGroup, host, device);
    check(upload && core->drain());
    auto resident = observe(*core, upload);
    retire(*core, uploadGroup);
    const std::array tensorTypes{NumericType::Float32};
#ifdef BENCH_WORKGROUP
    WorkgroupLaunch launch{{256}, {1}, {{{WorkgroupShapeAction::Extent, 0, 0, 0}}}, 258 * 4};
    auto kernel =
        take(r.loadWorkgroupKernel(argv[1], device, NumericType::Float32, tensorTypes, {}, launch));
    const auto kind = NumericProgramKind::Workgroup;
#else
    auto kernel = take(r.loadNumericGraphKernel(argv[1], device, NumericType::Float32, {1, 0},
                                                tensorTypes, {}, std::array<std::uint32_t, 1>{0},
                                                std::array<std::uint8_t, 1>{0}));
    const auto kind = NumericProgramKind::Map;
#endif
    const std::array<std::uint32_t, 1> operands{0};
    m::NumericInstruction instruction{kind, kernel, operands, {}, 1};
    auto program =
        take(r.prepareNumericProgram(1, std::span(&instruction, 1), 1, NumericType::Float32, false
#if !defined(BENCH_METAL)
                                     ,
                                     device
#endif
                                     ));
    auto invoke = [&] {
      auto group = core->openRoot();
      auto task = r.executeNumericProgram(*core, group, program, std::array{resident});
      check(task && core->drain());
      auto result = observe(*core, task);
      retire(*core, group);
      return result;
    };
    // A fixed one-second warmup precedes the two reported warmup batches.
    // This lets clock/power transitions settle for both snapshots equally.
    auto warmUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < warmUntil) {
      auto result = invoke();
      check(result.bytes() == n * 4);
    }
    for (int round = -2; round < int(rounds); ++round) {
      m::Buffer result;
      double checksum = 0;
      auto start = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < count; ++i) {
        result = invoke();
        checksum += result.bytes();
      }
      auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                    .count();
      auto g = core->openRoot();
      auto task = r.download(*core, g, result);
      check(task && core->drain());
      auto downloaded = observe(*core, task);
#if defined(BENCH_METAL)
      auto bytes = take(downloaded.hostBytes());
#else
      auto bytes = take(raw::hostBytes(downloaded));
#endif
      check(bytes.size() == n * 4);
      for (std::size_t i = 0; i < n; ++i) {
        float actual;
        std::memcpy(&actual, bytes.data() + i * 4, 4);
#if !defined(BENCH_METAL) && !defined(BENCH_CUDA) && defined(BENCH_WORKGROUP)
        const auto ordered = benchmark::orderedFloatBits(std::bit_cast<std::uint32_t>(actual));
        check(ordered >= permitted[i][0] && ordered <= permitted[i][1]);
#else
        check(std::bit_cast<std::uint32_t>(actual) == std::bit_cast<std::uint32_t>(expected[i]));
#endif
      }
      retire(*core, g);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.0f}\n", round, ns, checksum);
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
