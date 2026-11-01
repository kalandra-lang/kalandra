#include "Vulkan.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

using namespace kalandra::runtime::vulkan;
namespace {
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
template <class T> T take(std::expected<T, Error> result, const char *message) {
  if (!result) {
    std::fprintf(stderr, "FAIL: %s (error %d)\n", message, int(result.error()));
    std::exit(1);
  }
  return std::move(*result);
}
void additions(Runtime &runtime) {
  for (std::size_t count : {0u, 1u, 255u, 256u, 257u, 511u, 512u, 513u, 131073u}) {
    std::vector<float> a(count), b(count);
    for (std::size_t i = 0; i < count; ++i) {
      a[i] = float(int(i % 127) - 63) / 32;
      b[i] = float(i % 31) / 16;
    }
    auto left = take(runtime.upload(a), "upload left");
    auto right = take(runtime.upload(b), "upload right");
    if (count)
      a[0] = 999; // Host mutation cannot alias uploaded data.
    auto output = take(runtime.add(left, right), "add");
    auto values = take(runtime.download(output), "download");
    if (count)
      a[0] = -63.0f / 32;
    check(values.size() == count, "add size");
    for (std::size_t i = 0; i < count; ++i)
      check(values[i] == a[i] + b[i], "add exact result");
    auto twice = take(runtime.add(output, right), "resident chain");
    values = take(runtime.download(twice), "chain download");
    for (std::size_t i = 0; i < count; ++i)
      check(values[i] == (a[i] + b[i]) + b[i], "chain exact result");
    check(take(runtime.download(left), "immutable input") == a, "input preserved");
  }
}
void matrices(Runtime &runtime) {
  for (auto shape : {std::array{1u, 1u, 1u},
                     {3u, 5u, 7u},
                     {31u, 65u, 33u},
                     {64u, 64u, 32u},
                     {65u, 67u, 63u},
                     {128u, 128u, 96u},
                     {0u, 5u, 7u},
                     {3u, 0u, 7u},
                     {3u, 5u, 0u}}) {
    auto [m, n, k] = shape;
    std::vector<float> a(m * k), b(n * k);
    for (std::size_t i = 0; i < a.size(); ++i)
      a[i] = float(int(i % 31) - 15) * 0.03125f + std::ldexp(float(i % 7), -20);
    for (std::size_t i = 0; i < b.size(); ++i)
      b[i] = float(int(i % 23) - 11) * 0.0625f - std::ldexp(float(i % 5), -19);
    auto left = take(runtime.upload(a), "matrix left");
    auto right = take(runtime.upload(b), "matrix right");
    auto result = take(runtime.matmulRightTransposed(left, right, m, n, k), "matmul");
    auto values = take(runtime.download(result), "matrix download");
    check(values.size() == m * n, "matrix size");
    for (unsigned row = 0; row < m; ++row)
      for (unsigned col = 0; col < n; ++col) {
        double expected = 0, magnitude = 0;
        for (unsigned inner = 0; inner < k; ++inner) {
          double product = double(a[row * k + inner]) * double(b[col * k + inner]);
          expected += product;
          magnitude += std::abs(product);
        }
        double tolerance = 4 * std::numeric_limits<float>::epsilon() * std::max(1u, k) * magnitude;
        if (!std::isfinite(values[row * n + col]) ||
            std::abs(values[row * n + col] - expected) > tolerance) {
          std::fprintf(stderr, "matrix %u,%u,%u cell %u,%u: %.10g expected %.10g tolerance %.10g\n",
                       m, n, k, row, col, values[row * n + col], expected, tolerance);
          check(false, "matmul independent double reference");
        }
      }
    check(take(runtime.download(left), "matrix input unchanged") == a, "matrix left immutable");
    check(take(runtime.download(right), "matrix right unchanged") == b, "matrix right immutable");
  }
  // Detect reduced-precision operand truncation using an exact one-product case.
  auto precise = take(runtime.upload(std::array{1.0f + std::ldexp(1.0f, -20)}), "precise input");
  auto one = take(runtime.upload(std::array{1.0f}), "one input");
  auto result = take(runtime.matmulRightTransposed(precise, one, 1, 1, 1), "full precision matmul");
  check(take(runtime.download(result), "precise output")[0] == 1.0f + std::ldexp(1.0f, -20),
        "no TF32/FP16 truncation");
}
} // namespace
int main(int argc, char **argv) {
  bool validation = true;
  auto devices = take(Runtime::enumerate(validation), "enumerate");
  check(!devices.empty(), "hardware GPU required; software fallback forbidden");
  std::uint32_t selected =
      argc > 1 ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 0;
  check(selected < devices.size(), "device index");
  auto runtime =
      take(Runtime::create({.validation = validation, .deviceIndex = selected}), "create runtime");
  std::printf("GPU: %s vendor=%x device=%x\n", runtime->device().name.c_str(),
              runtime->device().vendor, runtime->device().device);
  additions(*runtime);
  matrices(*runtime);
  check(runtime->retainedBytes() == 0, "all buffers reclaimed");
  check(!runtime->add({}, {}), "invalid buffers rejected");
  auto a = take(runtime->upload(std::array{1.0f}), "bounds input");
  auto b = take(runtime->upload(std::array{1.0f, 2.0f}), "unequal input");
  check(!runtime->add(a, b), "shape mismatch rejected");
  check(!runtime->matmulRightTransposed(a, b, 1, 2, 2), "matrix extent mismatch rejected");
  auto other = take(Runtime::create({.deviceIndex = selected}), "second context");
  check(!other->download(a) && !other->add(a, a), "foreign context rejected");
  auto limited = take(Runtime::create({.deviceIndex = selected, .maxBytes = 1}), "limited context");
  auto denied = limited->upload(std::array{1.0f});
  check(!denied && denied.error() == Error::Budget && limited->retainedBytes() == 0,
        "bounded allocation rollback");
  auto impossible = Runtime::create({.deviceIndex = UINT32_MAX});
  check(!impossible, "unavailable device rejected");
  std::array<std::thread, 3> workers;
  for (auto &worker : workers)
    worker = std::thread([&] { additions(*runtime); });
  for (auto &worker : workers)
    worker.join();
  check(runtime->validationErrors() == 0 && other->validationErrors() == 0 &&
            limited->validationErrors() == 0,
        "zero validation errors");
  runtime.reset();
  // Buffers independently keep the device/context alive until their final owner.
  a = {};
  b = {};
  std::puts("PASS: exact add/chain, float32 matmul references, tails/empty shapes, immutable "
            "ownership, limits, concurrent calls, validation");
}
