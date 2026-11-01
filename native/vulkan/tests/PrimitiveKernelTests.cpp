#include "Vulkan.h"
#include "kalandra/NumericType.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::vulkan;
namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> result, const char *message) {
  if (!result) {
    std::fprintf(stderr, "%s: error %d\n", message, int(result.error()));
    throw std::runtime_error(message);
  }
  return std::move(*result);
}
// All normal inputs/reference sums are small integers, exactly representable in
// every tested type. No GPU implementation is reused by the independent oracle.
std::uint64_t encode(NumericDescription type, unsigned value) {
  if (type.type == NumericType::Float32)
    return std::bit_cast<std::uint32_t>(float(value));
  if (type.type == NumericType::Float64)
    return std::bit_cast<std::uint64_t>(double(value));
  return value;
}
void store(std::vector<std::byte> &data, unsigned index, NumericDescription type,
           std::uint64_t value) {
  std::memcpy(data.data() + index * type.bytes(), &value, type.bytes());
}
void reductions(Runtime &runtime, const Kernel &kernel, NumericDescription type) {
  for (unsigned count : {0u, 1u, 2u, 3u, 63u, 64u, 65u, 129u, 257u}) {
    std::vector<std::byte> raw(count * type.bytes());
    unsigned sum = 0;
    for (unsigned i = 0; i < count; ++i) {
      const unsigned value = i % 5 == 0;
      store(raw, i, type, encode(type, value));
      sum += value;
    }
    auto original = take(runtime.uploadBytes(raw), "reduction upload");
    auto current = original;
    auto cells = count;
    do {
      const auto next = std::max(1u, (cells + 1) / 2);
      std::array inputs{current};
      std::array words{cells, UINT32_MAX};
      auto step = take(runtime.execute(kernel, inputs, words, next * type.bytes(),
                                       std::max(1u, (next + 63) / 64)),
                       "reduction level");
      check(step.parameters.back() == UINT32_MAX, "normal reduction fault");
      current = std::move(step.output);
      cells = next;
    } while (cells > 1);
    std::vector<std::byte> expected(type.bytes());
    store(expected, 0, type, encode(type, sum));
    check(take(runtime.downloadBytes(current), "reduction result") == expected,
          "independent exact reduction reference");
    check(take(runtime.downloadBytes(original), "reduction original") == raw,
          "immutable reduction input");
  }
}
void matrices(Runtime &runtime, const Kernel &kernel, NumericDescription type) {
  for (auto shape : std::array<std::array<unsigned, 3>, 7>{
           {{0, 3, 2}, {3, 0, 2}, {3, 4, 0}, {1, 1, 1}, {3, 5, 7}, {9, 13, 3}, {65, 3, 7}}}) {
    const auto [rows, columns, inner] = shape;
    std::vector<std::byte> a(rows * inner * type.bytes()), b(inner * columns * type.bytes()),
        expected(rows * columns * type.bytes());
    for (unsigned i = 0; i < rows * inner; ++i)
      store(a, i, type, encode(type, i % 3));
    for (unsigned i = 0; i < inner * columns; ++i)
      store(b, i, type, encode(type, i % 4));
    for (unsigned row = 0; row < rows; ++row)
      for (unsigned column = 0; column < columns; ++column) {
        unsigned sum = 0;
        for (unsigned k = 0; k < inner; ++k)
          sum += ((row * inner + k) % 3) * ((k * columns + column) % 4);
        store(expected, row * columns + column, type, encode(type, sum));
      }
    auto left = take(runtime.uploadBytes(a), "matrix left");
    auto right = take(runtime.uploadBytes(b), "matrix right");
    std::array inputs{left, right};
    // Shared fixed-tree shader ABI is M, K, N, fault (row-major operands).
    std::array words{rows, inner, columns, UINT32_MAX};
    auto result = take(runtime.execute(kernel, inputs, words, expected.size(),
                                       std::max(1u, (rows * columns + 63) / 64)),
                       "matrix kernel");
    check(result.parameters.back() == UINT32_MAX, "normal matrix fault");
    check(take(runtime.downloadBytes(result.output), "matrix result") == expected,
          "independent exact row-major matrix reference");
    check(take(runtime.downloadBytes(left), "matrix original left") == a &&
              take(runtime.downloadBytes(right), "matrix original right") == b,
          "immutable matrix inputs");
  }
}
void faults(Runtime &runtime, const Kernel &reduce, const Kernel &matrix, NumericDescription type) {
  if (type.family == NumericFamily::BinaryFloat)
    return;
  const auto maximum = type.family == NumericFamily::UnsignedInteger
                           ? (UINT64_MAX >> (64 - type.bits))
                           : (UINT64_MAX >> (65 - type.bits));
  std::vector<std::byte> raw(2 * type.bytes());
  store(raw, 0, type, maximum);
  store(raw, 1, type, 1);
  auto input = take(runtime.uploadBytes(raw), "overflow reduction input");
  std::array inputs{input};
  std::array words{2u, UINT32_MAX};
  auto result = take(runtime.execute(reduce, inputs, words, type.bytes(), 1), "overflow reduce");
  check(result.parameters.back() == 0, "reduction checked-overflow lane");
  raw.resize(type.bytes());
  auto left = take(runtime.uploadBytes(raw), "overflow matrix left");
  store(raw, 0, type, 2);
  auto right = take(runtime.uploadBytes(raw), "overflow matrix right");
  std::array matrixInputs{left, right};
  std::array matrixWords{1u, 1u, 1u, UINT32_MAX};
  result =
      take(runtime.execute(matrix, matrixInputs, matrixWords, type.bytes(), 1), "overflow matrix");
  check(result.parameters.back() == 0, "matrix checked-overflow lane");
}
void uint32Products(Runtime &runtime, const Kernel &matrix) {
  constexpr std::array<std::uint32_t, 9> boundaries{0,     1,          2,          65535,     65536,
                                                    65537, 0x7fffffff, 0xfffffffe, UINT32_MAX};
  auto product = [&](std::uint32_t a, std::uint32_t b) {
    auto left = take(runtime.uploadBytes(std::as_bytes(std::span(&a, 1))), "uint32 product left");
    auto right = take(runtime.uploadBytes(std::as_bytes(std::span(&b, 1))), "uint32 product right");
    std::array inputs{left, right};
    std::array words{1u, 1u, 1u, UINT32_MAX};
    auto result = take(runtime.execute(matrix, inputs, words, 4, 1), "uint32 boundary product");
    const std::uint64_t expected = std::uint64_t(a) * b;
    check(result.parameters.back() == (expected > UINT32_MAX ? 0u : UINT32_MAX),
          "uint32 native high-word overflow agrees with uint64 oracle");
    if (expected <= UINT32_MAX) {
      auto bytes = take(runtime.downloadBytes(result.output), "uint32 boundary output");
      std::uint32_t actual;
      std::memcpy(&actual, bytes.data(), 4);
      check(actual == expected, "uint32 native low-word exact result");
    }
  };
  for (auto a : boundaries)
    for (auto b : boundaries)
      product(a, b);
  std::uint32_t random = 0x217948ab;
  for (unsigned i = 0; i < 128; ++i) {
    random = random * 1664525 + 1013904223;
    auto a = random;
    random = random * 1664525 + 1013904223;
    // Include non-overflowing products rather than mostly full-width faults.
    product(i % 2 ? a & 65535 : a, i % 2 ? random & 65535 : random);
  }
}
int run(int argc, char **argv) {
  if (argc != 2 && argc != 3)
    return 2;
  auto runtime =
      take(Runtime::create(
               {.deviceIndex = argc == 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0}),
           "runtime");
  for (auto type : numericTypes) {
    std::fprintf(stderr, "Checking %.*s source primitives\n", int(type.name.size()),
                 type.name.data());
    auto root = std::filesystem::path(argv[1]);
    auto reduce =
        take(runtime->loadKernel(root / "reduce" / type.name / "kernel0.spv"), "load reduction");
    auto matrix =
        take(runtime->loadKernel(root / "matmul" / type.name / "kernel0.spv"), "load matrix");
    reductions(*runtime, reduce, type);
    matrices(*runtime, matrix, type);
    faults(*runtime, reduce, matrix, type);
    if (type.type == NumericType::UInt32)
      uint32Products(*runtime, matrix);
    check(!runtime->retainedBytes(), "primitive allocation retirement");
    std::printf("PASS: %.*s source reduction/matmul/empty/tails/overflow\n", int(type.name.size()),
                type.name.data());
  }
  check(!runtime->validationErrors(), "zero primitive Vulkan validation errors");
  std::printf("PASS: typed source primitives on %s\n", runtime->device().name.c_str());
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
