#include "Async.h"
#include "kalandra/CudaArtifact.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace gpu = kalandra::runtime::cuda;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, gpu::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
std::vector<std::byte> product(gpu::Runtime &runtime, const gpu::Kernel &kernel, unsigned m,
                               unsigned n, unsigned k, const std::vector<float> &a,
                               const std::vector<float> &b,
                               gpu::FailurePoint failure = gpu::FailurePoint::None) {
  auto left = take(runtime.upload(a), "left"), right = take(runtime.upload(b), "right");
  const auto before = runtime.retainedBytes();
  const std::array inputs{left, right};
  const std::array words{m, k, n, UINT32_MAX};
  runtime.injectFailure(failure);
  auto result =
      take(runtime.execute(kernel, inputs, words, std::uint64_t(m) * n * 4, 1), "product");
  check(result.parameters == std::vector<unsigned>(words.begin(), words.end()),
        "parameters immutable");
  check(take(runtime.download(left), "left read") == a ||
            take(runtime.downloadBytes(left), "left bits") ==
                std::vector<std::byte>(std::as_bytes(std::span(a)).begin(),
                                       std::as_bytes(std::span(a)).end()),
        "left immutable");
  check(take(runtime.downloadBytes(right), "right bits") ==
            std::vector<std::byte>(std::as_bytes(std::span(b)).begin(),
                                   std::as_bytes(std::span(b)).end()),
        "right immutable");
  auto bytes = take(runtime.downloadBytes(result.output), "output");
  result = {};
  check(runtime.retainedBytes() == before, "output/parameters/scratch retired at completion");
  return bytes;
}
void numerical(const char *path) {
  auto runtime = take(gpu::Runtime::create(), "runtime");
  auto kernel = take(runtime->loadMatmulKernel(path), "catalog");
  // Independent dyadic and literal low-FP32-bit/subnormal oracles. Wide geometry
  // is exercised with short K to keep CPU reference and safety checks bounded.
  for (auto shape :
       {std::array{64u, 64u, 16u}, std::array{64u, 128u, 64u}, std::array{1024u, 1024u, 16u}}) {
    auto [m, n, k] = shape;
    for (unsigned dataset = 0; dataset < 5; ++dataset) {
      std::vector<float> a(std::size_t(m) * k), b(std::size_t(k) * n);
      for (unsigned i = 0; i < m; ++i)
        for (unsigned t = 0; t < k; ++t)
          a[std::size_t(i) * k + t] =
              dataset == 0   ? float(int((i * 17 + t * 13) % 19) - 9) / 16
              : dataset == 1 ? (t ? 0 : 1 + (i % 11) * 0x1p-23f)
              : dataset == 2 ? (t ? 0
                                  : std::bit_cast<float>(
                                        std::array{1u, 0x80000001u, 0x400000u, 0x800001u}[i % 4]))
                             : std::bit_cast<float>(std::uint32_t(
                                   ((i + t) % 2) << 31 | ((121 + (i + t) % 13) << 23) |
                                   ((i * 17 + t * 13) * 2654435761u & 0x7fffff)));
      for (unsigned t = 0; t < k; ++t)
        for (unsigned j = 0; j < n; ++j)
          b[std::size_t(t) * n + j] = dataset == 0   ? float(int((t * 7 + j * 11) % 23) - 11) / 16
                                      : dataset == 1 ? (t ? 0 : 1 + (j % 13) * 0x1p-23f)
                                      : dataset == 2 ? (t ? 0 : (j % 2 ? -1.0f : 1.0f))
                                                     : float(int((t * 7 + j * 11) % 23) - 11) / 16;
      if (dataset == 4) {
        constexpr std::array bits{0u,          0x80000000u, 1u,          0x80000001u, 0x7f7fffffu,
                                  0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u};
        for (unsigned i = 0; i < m; ++i)
          for (unsigned t = 0; t < k; ++t)
            a[std::size_t(i) * k + t] = std::bit_cast<float>(bits[(i + t) % bits.size()]);
        for (unsigned t = 0; t < k; ++t)
          for (unsigned j = 0; j < n; ++j)
            b[std::size_t(t) * n + j] = std::bit_cast<float>(bits[(t + j + 3) % bits.size()]);
      }
      auto fast = product(*runtime, kernel, m, n, k, a, b);
      auto slow = product(*runtime, kernel, m, n, k, a, b, gpu::FailurePoint::ScratchAllocate);
      check(fast == slow, "budget/OOM fallback has bit-identical arithmetic");
      for (unsigned i = 0; i < m; ++i)
        for (unsigned j = 0; j < n; ++j) {
          long double exact = 0, condition = 0;
          for (unsigned t = 0; t < k; ++t) {
            auto v =
                static_cast<long double>(a[std::size_t(i) * k + t]) * b[std::size_t(t) * n + j];
            exact += v;
            condition += std::abs(v);
          }
          float actual;
          std::copy_n(fast.data() + (std::size_t(i) * n + j) * 4, 4,
                      reinterpret_cast<std::byte *>(&actual));
          if (dataset == 4) {
            std::array<float, 8> partial{};
            const auto slice = (k + 39) / 40 * 8;
            for (unsigned part = 0; part < 5; ++part) {
              const auto begin = part * slice;
              if (begin >= k)
                continue;
              const auto length = std::min(slice, k - begin);
              for (unsigned t = 0; t < length; ++t)
                partial[part] = std::fma(a[std::size_t(i) * k + begin + t],
                                         b[std::size_t(begin + t) * n + j], partial[part]);
              for (unsigned t = length; t % 8; ++t)
                partial[part] = std::fma(0.0f, 0.0f, partial[part]);
            }
            for (unsigned step = 1; step < 8; step *= 2)
              for (unsigned part = 0; part < 8; part += step * 2)
                partial[part] += partial[part + step];
            if (std::isnan(partial[0]))
              check(std::isnan(actual) && (std::bit_cast<unsigned>(actual) & 0x400000),
                    "IEEE quiet NaN classification");
            else
              check(std::bit_cast<unsigned>(actual) == std::bit_cast<unsigned>(partial[0]),
                    "independent ordered IEEE oracle");
          } else if (dataset < 3) {
            auto expected = float(exact);
            if (dataset == 2)
              check(actual == expected, "literal subnormal witness");
            else
              check(std::bit_cast<unsigned>(actual) == std::bit_cast<unsigned>(expected),
                    "independent exact oracle");
          } else {
            constexpr long double u = 0x1p-24L;
            auto bound = (k + 8) * u / (1 - (k + 8) * u) * condition;
            check(std::isfinite(actual) &&
                      std::abs(static_cast<long double>(actual) - exact) <= bound,
                  "independent long-double forward-error oracle");
          }
        }
    }
  }
  auto stats = runtime->dispatchStats();
  check(stats.splitMatmuls == 15 && stats.scratchFreeMatmuls == 15,
        "actual fast/fallback dispatch counts");
  check(!runtime->retainedBytes(), "all numerical tensor resources retired");
  // Real quota exhaustion, not only injected OOM: preserve mandatory storage
  // and choose the same arithmetic without an extra tensor allocation.
  auto probe = take(runtime->blankBytes(1), "quota granularity");
  auto granularity = runtime->retainedBytes();
  probe = {};
  auto limited = take(gpu::Runtime::create({.maxBytes = 4 * granularity + 16}), "bounded runtime");
  auto bounded = take(limited->loadMatmulKernel(path), "bounded kernel");
  std::vector<float> a(64 * 16, 1), b(16 * 64, 1);
  auto output = product(*limited, bounded, 64, 64, 16, a, b);
  check(output == product(*runtime, kernel, 64, 64, 16, a, b),
        "real byte budget preserves numerical bits");
  check(limited->dispatchStats().scratchFreeMatmuls == 1 && !limited->retainedBytes(),
        "real byte budget fallback retires");
  auto tight =
      take(gpu::Runtime::create(
               {.maxBytes = 4 * granularity +
                            ((64 * 64 * 4 * 5 + granularity - 1) / granularity) * granularity + 8}),
           "tight fast runtime");
  auto tightKernel = take(tight->loadMatmulKernel(path), "tight fast kernel");
  check(product(*tight, tightKernel, 64, 64, 16, a, b) == output,
        "scratch retires before mandatory readback quota");
  check(tight->dispatchStats().splitMatmuls == 1 && !tight->retainedBytes(),
        "tight fast quota retired");
  // Full planes exceed the quota, but bounded row bands fit. Cover a final
  // 64-row tail as well as the wide tile; both must match the full-plane bits.
  for (auto shape : {std::array{2112u, 128u, 64u}, std::array{1024u, 1024u, 16u}}) {
    auto [m, n, k] = shape;
    const auto charge = [&](std::uint64_t bytes) {
      return (bytes + granularity - 1) / granularity * granularity;
    };
    const auto tableBytes = std::uint64_t((m + 255) / 256) * 16;
    const auto mandatory = charge(std::uint64_t(m) * k * 4) + charge(std::uint64_t(k) * n * 4) +
                           charge(std::uint64_t(m) * n * 4) + charge(16);
    const auto bound =
        mandatory + charge(std::uint64_t(256) * n * 4 * 5) + charge(tableBytes) + tableBytes + 16;
    check(bound < mandatory + charge(std::uint64_t(m) * n * 4 * 5),
          "test quota requires banding at actual allocation granularity");
    auto bands = take(gpu::Runtime::create({.maxBytes = bound}), "band budget");
    auto bandKernel = take(bands->loadMatmulKernel(path), "band catalog");
    std::vector<float> aa(std::size_t(m) * k), bb(std::size_t(k) * n);
    for (std::size_t i = 0; i < aa.size(); ++i)
      aa[i] = std::bit_cast<float>(std::uint32_t((i % 2) << 31) | 0x3f000000u |
                                   (std::uint32_t(i * 2654435761u) & 0x7fffff));
    for (std::size_t i = 0; i < bb.size(); ++i)
      bb[i] = std::bit_cast<float>(std::uint32_t((i % 3 == 0) << 31) | 0x3f000000u |
                                   (std::uint32_t(i * 2246822519u) & 0x7fffff));
    check(product(*bands, bandKernel, m, n, k, aa, bb) ==
              product(*runtime, kernel, m, n, k, aa, bb),
          "banded workspace preserves full-plane arithmetic including row tails");
    auto stats = bands->dispatchStats();
    check(stats.bandedMatmuls == 1 && stats.splitMatmuls == 1 && !stats.scratchFreeMatmuls,
          "actual bounded band dispatch");
    check(!bands->retainedBytes(), "band workspace and parameter table retired");
    auto left = take(bands->upload(aa), "band fault left"),
         right = take(bands->upload(bb), "band fault right");
    auto before = bands->retainedBytes();
    bands->injectFailure(gpu::FailurePoint::ReduceLaunch);
    auto failed = bands->execute(bandKernel, std::array{left, right},
                                 std::array{m, k, n, UINT32_MAX}, std::uint64_t(m) * n * 4, 1);
    check(!failed && failed.error() == gpu::Error::Device && !bands->healthy() &&
              bands->retainedBytes() == before,
          "failed band submission settles before workspace/table retirement");
  }
  // Compute Sanitizer also treats intentional API rejection as an API error.
  // Keep that negative test in normal/host-sanitized runs, and run the rest
  // with strict device checks rather than suppressing CUDA API errors globally.
  if (!std::getenv("KALANDRA_CATALOG_SKIP_NEGATIVE_ADMISSION")) {
    std::ifstream source(path, std::ios::binary);
    std::string text(std::istreambuf_iterator<char>(source), {});
    auto artifact = decodeCudaArtifact(text);
    check(bool(artifact), "V3 fixture decode");
    auto invalidPath = std::filesystem::path(path).string() + ".missing-reducer.test.ptx";
    const auto expected = artifact->entry + "_reduce";
    for (std::size_t pos = 0; (pos = artifact->ptx.find(expected, pos)) != std::string::npos;) {
      artifact->ptx.replace(pos, expected.size(), expected + "_missing");
      pos += expected.size() + 8;
    }
    {
      std::ofstream out(invalidPath);
      out << encodeCudaArtifact(*artifact);
    }
    auto admission = take(gpu::Runtime::create({.maxModules = 1}), "admission runtime");
    auto rejected = admission->loadKernel(invalidPath);
    std::filesystem::remove(invalidPath);
    check(!rejected && rejected.error() == gpu::Error::Invalid,
          "missing auxiliary function rejected");
    check(bool(admission->loadKernel(path)), "failed catalog admission returns module quota");
  }
  // One catalog consumes one module slot regardless of the four entry points.
  auto ceiling = take(gpu::Runtime::create({.maxModules = 1}), "module ceiling");
  auto module = take(ceiling->loadKernel(path), "one module catalog");
  check(bool(ceiling->loadKernel(path)), "cached catalog shares module admission");
  auto companion = ceiling->loadMatmulKernel(path);
  check(!companion && companion.error() == gpu::Error::Budget,
        "companion cannot exceed module quota");
  auto foreign = runtime->execute(module, std::span<const gpu::Buffer>{}, {}, 0, 1);
  check(!foreign && foreign.error() == gpu::Error::Invalid, "foreign catalog rejected");
}
void failures(const char *path) {
  for (auto point : {gpu::FailurePoint::ScratchDevice, gpu::FailurePoint::Launch,
                     gpu::FailurePoint::ReduceLaunch, gpu::FailurePoint::WaitAfterCompletion}) {
    auto runtime = take(gpu::Runtime::create(), "failure runtime");
    auto kernel = take(runtime->loadMatmulKernel(path), "failure catalog");
    std::vector<float> values(64 * 64, 1);
    auto left = take(runtime->upload(values), "failure left"),
         right = take(runtime->upload(values), "failure right");
    const auto before = runtime->retainedBytes();
    runtime->injectFailure(point);
    auto result = runtime->execute(kernel, std::array{left, right},
                                   std::array{64u, 64u, 64u, UINT32_MAX}, 64 * 64 * 4, 1);
    check(!result && result.error() == gpu::Error::Device,
          "real/injected device errors never fall back");
    check(!runtime->healthy() && runtime->retainedBytes() == before,
          "settled failure returns temporary quota and poisons context");
    left = {};
    right = {};
    check(!runtime->retainedBytes(), "failed launch operands retired after settlement");
  }
}
void cancellation(const char *path) {
  for (unsigned workers : {1u, 2u, 4u})
    for (bool deadline : {false, true}) {
      std::shared_ptr<gpu::Runtime> runtime = take(gpu::Runtime::create(), "async runtime");
      auto kernel = take(runtime->loadMatmulKernel(path), "async catalog");
      std::vector<float> values(64 * 64, 1);
      auto left = take(runtime->upload(values), "async left"),
           right = take(runtime->upload(values), "async right");
      auto before = runtime->retainedBytes();
      auto core = TaskCore::create({.workers = workers});
      auto async = gpu::AsyncRuntime::create(runtime);
      check(bool(core) && bool(async), "async services");
      auto group = core->openRoot();
      async->holdCompletions(true);
      auto task = async->execute(*core, group, kernel, std::array{left, right},
                                 std::array{64u, 64u, 64u, UINT32_MAX}, 64 * 64 * 4, 1);
      check(bool(task) && core->close(group), "async submit");
      auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!async->stats().held && std::chrono::steady_clock::now() < until) {
        core->step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // Always release the hold before asserting so a failed test cannot strand
      // the completion thread during unwinding.
      auto held = async->stats().held;
      auto cancelled = core->cancel(group, deadline);
      auto unsettled = !core->settled(group);
      async->holdCompletions(false);
      check(held == 1 && cancelled && unsettled,
            "cancellation awaits completed primitive acknowledgement");
      check(core->drain() && core->failure(group).kind == (deadline ? TaskFailure::DeadlineExceeded
                                                                    : TaskFailure::Cancelled),
            "cancel/deadline precedence");
      check(core->release(group) && !async->stats().pending && !async->stats().captureBytes,
            "async captures retired");
      check(runtime->retainedBytes() == before, "cancelled output and scratch retired");
    }
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected V3 artifact with fixed-tree companion");
    numerical(argv[1]);
    failures(argv[1]);
    cancellation(argv[1]);
    std::cout << "PASS: production catalog numerical oracles; bit-identical OOM/quota fallback; "
                 "module/ownership bounds; partial/reducer settlement; cancellation/deadline "
                 "workers1/2/4\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
