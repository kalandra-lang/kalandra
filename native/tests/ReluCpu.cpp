#include "../runtime/Runtime.h"
#include "ReluCases.h"
#include <atomic>
#include <cstdio>
using namespace kalandra;
using namespace kalandra::runtime;
struct Vector {
  void *allocated, *aligned;
  std::int64_t offset, size, stride;
};
extern "C" void _mlir_ciface_kalandra_test_entry(Vector *, Vector *, Invocation *);
int main() {
  constexpr auto type = NumericType::ELEMENT;
  const auto width = describeNumeric(type)->bytes();
  std::size_t checks = 0;
  for (std::int64_t n : {0, 1, 3, 31, 32, 33, 127, 128, 129, 255, 256, 257, 1025})
    for (std::int64_t stride : {-3, 1, 2}) {
      auto data = relu_tests::corpus(type, n * 3 + 8);
      const auto offset = 3 + (stride < 0 && n ? (n - 1) * -stride : 0);
      auto logical = relu_tests::corpus(type, n);
      for (std::int64_t i = 0; i < n; ++i)
        std::memcpy(data.data() + (offset + i * stride) * width, logical.data() + i * width, width);
      auto original = data;
      Vector input{data.data(), data.data(), offset, n, stride}, out{};
      Invocation ctx;
      _mlir_ciface_kalandra_test_entry(&out, &input, &ctx);
      relu_tests::check(!ctx.status() && out.size == n);
      for (std::int64_t i = 0; i < n; ++i) {
        auto actual = relu_tests::read(
            static_cast<std::byte *>(out.aligned) + (out.offset + i * out.stride) * width, width);
        auto expected = relu_tests::expected(
            type, relu_tests::read(data.data() + (offset + i * stride) * width, width));
        relu_tests::check(actual == expected);
        ++checks;
      }
      relu_tests::check(data == original);
      ctx.settle();
      relu_tests::check(!ctx.live() && ctx.allocations() == ctx.releases() && !ctx.callDepth());
      if (n) {
        Invocation failed;
        failed.failAllocationAt(1);
        Vector missing{};
        _mlir_ciface_kalandra_test_entry(&missing, &input, &failed);
        relu_tests::check(failed.status() && missing.aligned == nullptr);
        failed.settle();
        relu_tests::check(!failed.live());
        Invocation cancelled;
        std::atomic<bool> stop{true};
        cancelled.bindCancellation(&stop);
        _mlir_ciface_kalandra_test_entry(&missing, &input, &cancelled);
        relu_tests::check(cancelled.poll() == -1 && missing.aligned == nullptr);
        cancelled.settle();
        relu_tests::check(!cancelled.live());
      }
    }
  std::printf(
      "%.*s CPU ReLU: %zu exact cells; strides, failures, cancellation and cleanup passed\n",
      int(describeNumeric(type)->name.size()), describeNumeric(type)->name.data(), checks);
}
