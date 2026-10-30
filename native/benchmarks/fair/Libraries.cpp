#ifdef ACCELERATE
#include <Accelerate/Accelerate.h>
#else
#include <cblas.h>
static_assert(sizeof(blasint) == 4);
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef FLOAT32
using Float = float;
#else
using Float = double;
#endif
static Float input(std::size_t i, bool right, int pattern) {
  return pattern ? Float(int((i * (right ? 43 : 37)) % 101) - 50) / Float(right ? 29 : 37)
                 : Float(int(i % (right ? 13 : 17)) - (right ? 6 : 8)) / Float(8);
}
static Float dot(int n, const Float *a, const Float *b) {
#ifdef FLOAT32
  return cblas_sdot(n, a, 1, b, 1);
#else
  return cblas_ddot(n, a, 1, b, 1);
#endif
}
static Float kernel(int kind, int n, const std::vector<Float> &a, const std::vector<Float> &b,
                    const std::vector<Float> &ones, std::unique_ptr<Float[]> &out) {
  if (kind == 0)
    return dot(n, a.data(), b.data());
  out = std::make_unique_for_overwrite<Float[]>(a.size());
#ifdef FLOAT32
  if (kind == 1) {
    cblas_scopy(n, b.data(), 1, out.get(), 1);
    cblas_saxpy(n, 3, a.data(), 1, out.get(), 1);
  } else
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, n, n, n, 1, a.data(), n, b.data(), n, 0,
                out.get(), n);
#else
  if (kind == 1) {
    cblas_dcopy(n, b.data(), 1, out.get(), 1);
    cblas_daxpy(n, 3, a.data(), 1, out.get(), 1);
  } else
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, n, n, n, 1, a.data(), n, b.data(), n, 0,
                out.get(), n);
#endif
  return dot(int(a.size()), out.get(), ones.data());
}
int main(int argc, char **argv) {
  if (argc != 8)
    return 2;
  const int kind = std::stoi(argv[1]), n = std::stoi(argv[2]), rounds = std::stoi(argv[4]),
            threads = std::stoi(argv[5]), pattern = std::stoi(argv[6]), dump = std::stoi(argv[7]);
  const auto iterations = std::stoull(argv[3]);
  if (kind < 0 || kind > 2 || n < 1 || n > 4194304 || (!dump && (!iterations || rounds < 1)))
    return 2;
#ifdef ACCELERATE
  const auto mode = threads == 1 ? BLAS_THREADING_SINGLE_THREADED : BLAS_THREADING_MULTI_THREADED;
  if (BLASSetThreading(mode) || BLASGetThreading() != mode)
    return 3;
  const int actual = threads == 1 ? 1 : 0;
#else
  openblas_set_num_threads(threads);
  const int actual = openblas_get_num_threads();
  if (actual != threads)
    return 3;
#endif
  const auto count = kind == 2 ? std::size_t(n) * n : std::size_t(n);
  std::vector<Float> a(count), b(count), ones(count, 1);
  for (std::size_t i = 0; i < count; ++i) {
    a[i] = input(i, false, pattern);
    b[i] = input(i, true, pattern);
  }
  if (dump) {
    std::unique_ptr<Float[]> out;
    auto value = kernel(kind, n, a, b, ones, out);
    std::printf("{\"result\":%.17g,\"threads\":%d,\"cells\":[", double(value), actual);
    for (std::size_t i = 0; i < (out ? a.size() : 0); ++i)
      std::printf("%s%.17g", i ? "," : "", double(out[i]));
    std::puts("]}");
    return 0;
  }
  for (int round = -2; round < rounds; ++round) {
    double checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
      std::unique_ptr<Float[]> out;
      checksum += double(kernel(kind, n, a, b, ones, out));
    }
    const auto ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g,\"threads\":%d}\n", round, ns,
                checksum, actual);
  }
  for (std::size_t i = 0; i < count; ++i)
    if (a[i] != input(i, false, pattern) || b[i] != input(i, true, pattern))
      return 4;
}
