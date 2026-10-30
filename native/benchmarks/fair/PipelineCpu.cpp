#include "../../runtime/Runtime.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
using namespace kalandra::runtime;
struct Matrix {
  float *allocated, *aligned;
  std::int64_t offset, sizes[2], strides[2];
};
extern "C" float _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Matrix *, float,
                                                  Invocation *);
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  const auto n = std::stoll(argv[1]), count = std::stoll(argv[2]);
  const auto rounds = std::stoi(argv[3]);
  if (n < 1 || n > 2048 || count < 1 || rounds < 1)
    return 2;
  std::vector<float> a(n * n), b(n * n), bias(n * n, 0.125f);
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i] = float(int(i % 17) - 8) / 8;
    b[i] = float(int(i % 13) - 6) / 8;
  }
  Matrix x{a.data(), a.data(), 0, {n, n}, {n, 1}}, y{b.data(), b.data(), 0, {n, n}, {n, 1}},
      z{bias.data(), bias.data(), 0, {n, n}, {n, 1}};
  for (int round = -2; round < rounds; ++round) {
    double checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < count; ++i) {
      Invocation ctx;
      checksum += _mlir_ciface_kalandra_test_entry(&x, &y, &z, 0.25f, &ctx);
      if (ctx.status() || ctx.callDepth())
        throw std::runtime_error("CPU pipeline failed");
      ctx.settle();
      if (ctx.live() || ctx.allocations() != ctx.releases())
        throw std::runtime_error("CPU pipeline leaked");
    }
    auto ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g}\n", round, ns, checksum);
  }
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i] != float(int(i % 17) - 8) / 8 || b[i] != float(int(i % 13) - 6) / 8 ||
        bias[i] != 0.125f)
      return 3;
}
