#include "../../metal/Metal.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
static void checked(bool ok, int line) {
  if (!ok)
    throw std::runtime_error("Graph benchmark check failed at " + std::to_string(line));
}
#define check(x) checked(bool(x), __LINE__)
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  check(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
static void retire(TaskCore &core, GroupHandle group) {
  check(core.close(group) && core.drain() && core.release(group));
  const auto s = core.stats();
  check(!s.groups && !s.records && !s.retainedOwners && !s.retainedBytes && !s.externalPending);
}
static m::Buffer upload(m::Runtime &r, TaskCore &core, m::Buffer host, m::Device device) {
  const auto group = core.openRoot();
  const auto task = r.upload(core, group, host, device);
  check(task && core.drain());
  auto result = observe(core, task);
  retire(core, group);
  return result;
}
static float sum(std::vector<float> v) {
  while (v.size() > 1) {
    const auto size = v.size();
    for (std::size_t i = 0; i < size / 2; ++i)
      v[i] = v[2 * i] + v[2 * i + 1];
    if (size % 2)
      v[size / 2] = v.back();
    v.resize((size + 1) / 2);
  }
  return v.empty() ? 0 : v[0];
}
// B's columns repeat every 13 entries. Compute each distinct column using the
// full, separately rounded adjacent tree, then expand it. This optimization is
// only in the untimed reference; every GPU stage computes every matrix cell.
static float reference(std::vector<float> a, const std::vector<float> &b, unsigned rows, unsigned n,
                       unsigned layers, bool relu) {
  for (unsigned layer = 0; layer < layers; ++layer) {
    std::vector<float> out(rows * n), terms(n);
    for (unsigned row = 0; row < rows; ++row) {
      for (unsigned col = 0; col < std::min(n, 13u); ++col) {
        for (unsigned k = 0; k < n; ++k)
          terms[k] = a[row * n + k] * b[k * n + col];
        const float value = sum(terms);
        out[row * n + col] = relu && value < 0 ? 0 : value;
      }
      for (unsigned col = 13; col < n; ++col)
        out[row * n + col] = out[row * n + col % 13];
    }
    a = std::move(out);
  }
  return sum(std::move(a));
}
int main(int argc, char **argv) {
  if (argc != 8)
    return 2;
  const std::string directory = argv[1];
  const unsigned rows = std::stoul(argv[2]), n = std::stoul(argv[3]), layers = std::stoul(argv[4]);
  const bool relu = std::stoul(argv[5]);
  const unsigned iterations = std::stoul(argv[6]), rounds = std::stoul(argv[7]);
  check(rows && n && layers && iterations && rounds && layers <= 16 && n <= 4096 && rows <= 512);
  m::Options options;
  options.profile = std::getenv("KALANDRA_GPU_PROFILE") != nullptr;
  options.maxBytes =
      std::max(options.maxBytes, std::uint64_t(n) * n * 12 + std::uint64_t(rows) * n * 32);
  auto made = m::Runtime::create(options);
  check(made);
  auto &r = **made;
  auto device = r.device();
  check(device);
  TaskLimits limits;
  limits.workers = 1;
  auto core = TaskCore::create(limits);
  check(core);
  auto matrix = r.loadMatmulKernel(directory + "/kernel0.metallib", *device);
  auto reduction = r.loadReductionKernel(directory + "/kernel0-reduce.metallib", *device);
  auto mapping = r.loadElementwiseKernel(directory + "/kernel0-map1.metallib", *device, {1, 0});
  check(matrix && reduction && mapping);
  std::vector<float> a(rows * n), b(std::size_t(n) * n);
  for (std::size_t i = 0; i < a.size(); ++i)
    a[i] = (int(i % 17) - 8) / 8.0f;
  for (std::size_t i = 0; i < b.size(); ++i)
    b[i] = (int(i % 13) - 6) / 128.0f;
  const auto expected = reference(a, b, rows, n, layers, relu);
  check(std::isfinite(expected));
  {
    auto ha = r.hostTensor(std::as_bytes(std::span(a)), std::array<std::int64_t, 2>{rows, n},
                           m::Format::Float32);
    auto hb = r.hostTensor(std::as_bytes(std::span(b)), std::array<std::int64_t, 2>{n, n},
                           m::Format::Float32);
    check(ha && hb);
    auto ga = upload(r, *core, *ha, *device), gb = upload(r, *core, *hb, *device);
    std::vector<m::MatrixProduct> products;
    std::vector<m::Kernel> maps;
    unsigned input = 0;
    for (unsigned i = 0; i < layers; ++i) {
      products.push_back({input, 1});
      maps.push_back({});
      input = 1 + products.size();
      if (relu) {
        products.push_back({input, input});
        maps.push_back(*mapping);
        input = 1 + products.size();
      }
    }
    const auto baseline = r.stats().bytes;
    for (int round = -2; round < int(rounds); ++round) {
      const auto before = r.stats();
      double checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < iterations; ++i) {
        const auto group = core->openRoot();
        const auto task = r.matmulGraphReduceSum(*core, group, *matrix, std::array{ga, gb},
                                                 products, *reduction, {}, {}, maps);
        check(task && core->drain());
        Invocation context;
        float value = 0;
        check(core->observe(task, m::Runtime::scalarLayout(), context, &value));
        check(std::bit_cast<std::uint32_t>(value) == std::bit_cast<std::uint32_t>(expected));
        checksum += value;
        retire(*core, group);
      }
      const auto ns =
          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
              .count();
      const auto after = r.stats();
      check(after.bytes == baseline && !after.inFlight && !after.stagingBuffers &&
            !after.argumentPacks && !after.argumentBytes);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g,\"expected\":%.17g,\"submissions\":"
                  "%llu,\"completions\":%llu,\"peak_bytes\":%llu,\"resident_bytes\":%llu,\"gpu_"
                  "ns\":%llu,\"encoding_ns\":%llu,\"dispatches\":%llu}\n",
                  round, ns, checksum, double(expected),
                  (unsigned long long)(after.submissions - before.submissions),
                  (unsigned long long)(after.completions - before.completions),
                  (unsigned long long)after.peakBytes, (unsigned long long)baseline,
                  (unsigned long long)(after.gpuNanoseconds - before.gpuNanoseconds),
                  (unsigned long long)(after.encodingNanoseconds - before.encodingNanoseconds),
                  (unsigned long long)(after.dispatches - before.dispatches));
    }
  }
  const auto s = r.stats();
  check(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings);
}
