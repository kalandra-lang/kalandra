// Link Runtime.cpp with only its allocation entry renamed. Generated LLVM then
// calls this forwarding shim, recording actual requested payloads without
// modifying the production runtime or inserting probes into timing binaries.
#include "../runtime/Runtime.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
using kalandra::runtime::Invocation;
struct Matrix {
  float *allocated, *aligned;
  std::int64_t offset, sizes[2], strides[2];
};
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Matrix *, Invocation *);
extern "C" void *kalandra_probe_alloc_v1(Invocation *, std::uint64_t) noexcept;
static std::array<std::uint64_t, 2> requests;
static unsigned calls;
static void check(bool ok) {
  if (!ok) {
    std::fputs("Matrix allocation payload probe failed\n", stderr);
    std::exit(1);
  }
}
extern "C" void *kalandra_alloc_v1(Invocation *context, std::uint64_t bytes) noexcept {
  check(calls < requests.size());
  requests[calls++] = bytes;
  return kalandra_probe_alloc_v1(context, bytes);
}
int main() {
  unsigned cases = 0;
  const auto verify = [&](std::int64_t m, std::int64_t k, std::int64_t n) {
    std::vector<float> a(m * k + 2, 1), b(k * n + 2, 1);
    Matrix left{a.data(), a.data() + 1, 0, {m, k}, {k, 1}};
    Matrix right{b.data(), b.data() + 1, 0, {k, n}, {n, 1}}, result{};
    Invocation context;
    calls = 0;
    _mlir_ciface_kalandra_test_entry(&result, &left, &right, &context);
    if (context.status() || calls != 2)
      std::fprintf(stderr, "Shape %lld,%lld,%lld status %lld calls %u\n", (long long)m,
                   (long long)k, (long long)n, (long long)context.status(), calls);
    check(!context.status() && calls == 2);
    std::uint64_t scratch = 0;
    if (m && n) {
      const bool wide = m >= 4 && n >= 16;
      auto width = wide ? 16 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
      // Two rows by eight columns; thirty-two register-reduced leaves plus
      // stored subtree roots. Empty contractions still reserve the tile area.
      auto count = wide ? 33 + k / 32 : k;
      scratch = std::uint64_t(count) * width * sizeof(float);
    }
    // Bufferization requests 64 extra bytes to align tensor output; product
    // scratch remains a plain float memref without that output padding.
    auto expected = std::array{scratch, std::uint64_t(m * n) * sizeof(float) + 64};
    std::sort(expected.begin(), expected.end());
    std::sort(requests.begin(), requests.end());
    if (requests != expected)
      std::fprintf(stderr, "Shape %lld,%lld,%lld actual %llu,%llu expected %llu,%llu\n",
                   (long long)m, (long long)k, (long long)n, (unsigned long long)requests[0],
                   (unsigned long long)requests[1], (unsigned long long)expected[0],
                   (unsigned long long)expected[1]);
    check(requests == expected);
    for (std::int64_t r = 0; r < m && n; ++r)
      for (std::int64_t c = 0; c < n; ++c)
        check(result.aligned[result.offset + r * result.strides[0] + c * result.strides[1]] == k);
    context.settle();
    check(!context.live() && context.allocations() == context.releases());
    ++cases;
  };
  for (auto m : {0, 3, 4, 5, 7, 8})
    for (auto k : {0, 1, 256, 257, 513, 8193})
      for (auto n : {0, 1, 2, 3, 4, 15, 16, 17, 18, 19, 23, 31, 32, 33})
        verify(m, k, n);
  verify(0, std::numeric_limits<std::int64_t>::max(), 0);
  verify(std::numeric_limits<std::int64_t>::max(), 0, 0);
  std::printf("Verified %u actual matrix allocation payloads\n", cases);
}
