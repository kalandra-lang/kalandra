#include "../runtime/Runtime.h"
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
struct Matrix {
  float *allocated;
  float *aligned;
  std::int64_t offset;
  std::int64_t sizes[2];
  std::int64_t strides[2];
};
extern "C" float _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *,
                                                  kalandra::runtime::Invocation *);
// Independent host reference: force each elementary operation to float32,
// then reduce an explicitly materialized row-major list by adjacent pairs.
static float balanced(std::vector<float> values) {
  if (values.empty())
    return 0.0f;
  while (values.size() > 1) {
    std::vector<float> next;
    for (std::size_t i = 0; i < values.size(); i += 2) {
      if (i + 1 == values.size())
        next.push_back(values[i]);
      else {
        volatile float rounded = values[i] + values[i + 1];
        next.push_back(static_cast<float>(rounded));
      }
    }
    values = std::move(next);
  }
  return values.front();
}
static float reference(const std::vector<float> &a, const std::vector<float> &b, std::int64_t rows,
                       std::int64_t inner, std::int64_t columns) {
  std::vector<float> output;
  for (std::int64_t row = 0; row < rows; ++row)
    for (std::int64_t col = 0; col < columns; ++col) {
      std::vector<float> products;
      for (std::int64_t k = 0; k < inner; ++k) {
        volatile float rounded = a[row * inner + k] * b[k * columns + col];
        products.push_back(static_cast<float>(rounded));
      }
      output.push_back(balanced(std::move(products)));
    }
  return balanced(std::move(output));
}
static int run(int argc, char **argv) {
  std::int64_t rows = 2, inner = 3, columns = 2, rightRows = -1;
  std::uint64_t repeats = 1;
  const char *inputPath = nullptr;
  bool negative = false, oversize = false, verify = false, quiet = false, finite = false,
       isolation = false;
  std::uint64_t isolationFail = 0;
  std::uint32_t seed = 0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--isolation"))
      isolation = true;
    else if (!std::strcmp(argv[i], "--verify"))
      verify = true;
    else if (!std::strcmp(argv[i], "--finite"))
      finite = true;
    else if (!std::strcmp(argv[i], "--quiet"))
      quiet = true;
    else if (!std::strcmp(argv[i], "--negative"))
      negative = true;
    else if (!std::strcmp(argv[i], "--oversize"))
      oversize = true;
    else if (!std::strcmp(argv[i], "--input") && i + 1 < argc)
      inputPath = argv[++i];
    else if (i + 1 < argc) {
      const char *option = argv[i];
      const char *text = argv[++i];
      char *end = nullptr;
      errno = 0;
      auto value = std::strtoull(text, &end, 10);
      if (errno || end == text || *end ||
          value > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        return 2;
      if (!std::strcmp(option, "--isolation-fail"))
        isolationFail = value;
      else if (!std::strcmp(option, "--seed"))
        seed = static_cast<std::uint32_t>(value);
      else if (!std::strcmp(option, "--rows"))
        rows = value;
      else if (!std::strcmp(option, "--inner"))
        inner = value;
      else if (!std::strcmp(option, "--columns"))
        columns = value;
      else if (!std::strcmp(option, "--right-rows"))
        rightRows = value;
      else if (!std::strcmp(option, "--repeat"))
        repeats = value;
      else
        return 2;
    } else
      return 2;
  }
  if (rightRows < 0)
    rightRows = inner;
  const auto count = [](std::int64_t a, std::int64_t b) -> std::size_t {
    if (a == 0 || b == 0)
      return 0;
    if (a > 1000000 / b)
      std::exit(2);
    return static_cast<std::size_t>(a * b);
  };
  if (repeats == 0 || repeats > 1000000)
    return 2;
  std::vector<float> a(count(rows, inner)), b(count(rightRows, columns));
  for (std::size_t i = 0; i < a.size(); ++i)
    a[i] = static_cast<float>(i + 1);
  for (std::size_t i = 0; i < b.size(); ++i)
    b[i] = static_cast<float>(i + 1);
  if (seed) {
    const auto sample = [&]() {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      // Mix cancellation/rounding-sensitive finite values with IEEE edge cases.
      static constexpr std::uint32_t edges[] = {0,          0x80000000, 1,          0x80000001,
                                                0x007fffff, 0x00800000, 0x7f7fffff, 0xff7fffff,
                                                0x7f800000, 0xff800000, 0x7fc00001};
      const auto bits = finite          ? (seed & 0x807fffff) | (((seed >> 23) % 46 + 100) << 23)
                        : seed % 5 == 0 ? edges[(seed / 5) % 11]
                                        : (seed & 0x807fffff) | (((seed >> 23) % 250 + 1) << 23);
      return std::bit_cast<float>(bits);
    };
    for (auto &value : a)
      value = sample();
    for (auto &value : b)
      value = sample();
  }
  if (inputPath) {
    auto *input = std::fopen(inputPath, "rb");
    if (!input)
      return 2;
    bool valid = std::fread(a.data(), sizeof(float), a.size(), input) == a.size() &&
                 std::fread(b.data(), sizeof(float), b.size(), input) == b.size() &&
                 std::fgetc(input) == EOF && !std::ferror(input);
    std::fclose(input);
    if (!valid)
      return 2;
  }
  const auto originalA = a, originalB = b;
  const float expected = verify ? reference(a, b, rows, inner, columns) : 0.0f;
  Matrix left{a.data(), a.data(), 0, {rows, inner}, {inner, 1}};
  Matrix right{b.data(), b.data(), 0, {rightRows, columns}, {columns, 1}};
  if (negative)
    left.sizes[0] = -1;
  if (oversize)
    left.sizes[0] = std::numeric_limits<std::int64_t>::max();
  if (isolation) {
    using kalandra::runtime::Invocation;
    Invocation parent, child;
    auto *leftStorage = static_cast<float *>(parent.allocate(a.size() * sizeof(float)));
    auto *rightStorage = static_cast<float *>(parent.allocate(b.size() * sizeof(float)));
    if (!leftStorage || !rightStorage)
      return 5;
    if (!a.empty())
      std::memcpy(leftStorage, a.data(), a.size() * sizeof(float));
    if (!b.empty())
      std::memcpy(rightStorage, b.data(), b.size() * sizeof(float));
    if (!child.retainFrom(parent, leftStorage) || !child.retainFrom(parent, rightStorage))
      return 5;
    left.allocated = left.aligned = leftStorage;
    right.allocated = right.aligned = rightStorage;
    // Two retained packet fields are already initialized; inject into generated code.
    child.failAllocationAt(isolationFail ? isolationFail + 2 : 0);
    (void)_mlir_ciface_kalandra_test_entry(&left, &right, &child);
    if (child.status() != (isolationFail ? 2 : 1))
      return 5;
    child.settle();
    if (child.live() || child.allocations() != child.releases() || parent.live() != 2 ||
        parent.status() ||
        (!a.empty() && std::memcmp(leftStorage, a.data(), a.size() * sizeof(float))) ||
        (!b.empty() && std::memcmp(rightStorage, b.data(), b.size() * sizeof(float))))
      return 5;
    // A different invocation can still execute in this same process after failure.
    float recoveryLeft[] = {1, 2, 3, 4, 5, 6};
    float recoveryRight[] = {1, 2, 3, 4, 5, 6};
    Matrix l{recoveryLeft, recoveryLeft, 0, {2, 3}, {3, 1}};
    Matrix r{recoveryRight, recoveryRight, 0, {3, 2}, {2, 1}};
    Invocation recovery;
    const auto result = _mlir_ciface_kalandra_test_entry(&l, &r, &recovery);
    if (recovery.status() || result != 163.0f || recovery.live())
      return 5;
    parent.settle();
    if (parent.allocations() != parent.releases())
      return 5;
    std::puts("Isolated fault settled; parent inputs preserved; next invocation returned 163");
    return 0;
  }
  kalandra::runtime::Invocation context;
  float value = 0;
  std::uint32_t firstBits = 0;
  for (std::uint64_t i = 0; i < repeats; ++i) {
    value = _mlir_ciface_kalandra_test_entry(&left, &right, &context);
    if (context.status() || context.hasTaskError())
      context.reportFailure();
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if (verify && !(std::isnan(value) && std::isnan(expected)) &&
        bits != std::bit_cast<std::uint32_t>(expected)) {
      std::fprintf(stderr, "Reference mismatch %lld x %lld x %lld: %08x != %08x\n",
                   static_cast<long long>(rows), static_cast<long long>(inner),
                   static_cast<long long>(columns), bits, std::bit_cast<std::uint32_t>(expected));
      return 4;
    }
    if (i == 0)
      firstBits = bits;
    if ((bits != firstBits && !std::isnan(value)) || context.live() ||
        context.allocations() != context.releases() ||
        (!a.empty() && std::memcmp(a.data(), originalA.data(), a.size() * sizeof(float))) ||
        (!b.empty() && std::memcmp(b.data(), originalB.data(), b.size() * sizeof(float))))
      return 3;
  }
  if (!quiet)
    std::printf("{\"bits\":%u,\"allocations\":%llu,\"releases\":%llu,\"live\":%llu}\n",
                std::bit_cast<std::uint32_t>(value),
                static_cast<unsigned long long>(context.allocations()),
                static_cast<unsigned long long>(context.releases()),
                static_cast<unsigned long long>(context.live()));
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2 || std::strcmp(argv[1], "--sweep"))
    return run(argc, argv);
  unsigned cases = 0;
  for (auto rows : {0, 1, 2, 3, 5, 8, 17})
    for (auto inner : {0, 1, 2, 3, 7, 16, 33})
      for (auto columns : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 17})
        for (unsigned mode = 0; mode < 3; ++mode) {
          char r[20], k[20], c[20], seed[20];
          std::snprintf(r, sizeof(r), "%d", rows);
          std::snprintf(k, sizeof(k), "%d", inner);
          std::snprintf(c, sizeof(c), "%d", columns);
          std::snprintf(seed, sizeof(seed), "%u", mode == 0 ? 0 : 1234567 + cases * 7919);
          const char *args[] = {argv[0],   "--rows",
                                r,         "--inner",
                                k,         "--columns",
                                c,         "--seed",
                                seed,      "--verify",
                                "--quiet", "--repeat",
                                "2",       mode == 1 ? "--finite" : "--quiet"};
          if (auto status = run(14, const_cast<char **>(args)))
            return status;
          ++cases;
        }
  std::printf("Verified %u runtime-shape/content cases; live=%llu\n", cases, 0ULL);
}
