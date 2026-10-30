#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Input = CpuTensor<element, INPUT_RANK>;
using Output = CpuTensor<NumericType::Int64, 1>;
using Shape = std::array<std::int64_t, INPUT_RANK>;
#ifdef QUERY_LENGTH
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Input *, Invocation *);
#else
extern "C" void _mlir_ciface_kalandra_test_entry(Output *, Input *, Invocation *);
#endif
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::uint64_t cases = 0, faults = 0;
template <std::size_t Rank>
static void verify(std::array<std::int64_t, Rank> shape, int expected = 0) {
  CpuTensor<element, Rank> input{};
  // Deliberately no payload: a metadata query must never dereference it.
  input.offset = INT64_MAX;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    input.strides.fill(INT64_MIN);
  }
  std::uint64_t allocations = 0;
  for (unsigned fail = 0; fail <= allocations + 1; ++fail) {
    Invocation context;
    context.failAllocationAt(fail);
#ifdef QUERY_LENGTH
    const auto result = _mlir_ciface_kalandra_test_entry(&input, &context);
    const bool failed = expected != 0;
    require(context.status() == expected && result == (failed ? 0 : shape[0]),
            "length result/status changed");
    require(!context.allocations(), "length query allocated");
#else
    Output result{};
    _mlir_ciface_kalandra_test_entry(&result, &input, &context);
    const bool failed = expected || (fail && fail <= allocations);
    require(context.status() == (expected ? expected
                                 : failed ? 2
                                          : 0),
            "shape result/status changed");
    auto check = [&] {
      require(result.sizes[0] == std::int64_t(Rank), "shape query lost rank");
      for (std::size_t i = 0; i < Rank; ++i)
        require(result.aligned[result.offset + std::int64_t(i) * result.strides[0]] == shape[i],
                "shape query changed dimension/order");
    };
    std::optional<AllocationLease> lease;
    if (!failed) {
      check();
      if constexpr (Rank > 0) {
        lease = context.lease(result.allocated);
        require(bool(lease), "shape backing not retained");
      }
    } else
      require(!result.allocated && !result.aligned, "failed query exposed partial array");
#endif
    if (!fail) {
      allocations = context.allocations();
      require(allocations < 16, "unbounded query allocations");
    }
    if (failed) {
      require(context.line() == (expected ? 1 : 0) && context.column() == (expected ? 4 : 0),
              "query fault lost boundary/operation location");
      ++faults;
    }
    if (expected)
      require(!context.allocations(), "metadata validation happened after allocation");
    context.settle();
    require(!context.live() && context.allocations() == context.releases(), "query leaked owners");
#ifndef QUERY_LENGTH
    if (!failed)
      check();
#endif
    ++cases;
  }
}
template <std::size_t Rank> static void cancelled() {
  CpuTensor<element, Rank> input{};
  Invocation context;
  std::atomic<bool> token{true};
  context.bindCancellation(&token);
#ifdef QUERY_LENGTH
  auto output = _mlir_ciface_kalandra_test_entry(&input, &context);
  require(!output, "cancelled length published value");
#else
  Output output{};
  _mlir_ciface_kalandra_test_entry(&output, &input, &context);
  require(!output.allocated && !output.aligned, "cancelled shape published array");
#endif
  require(context.poll() == -1 && !context.status() && !context.allocations(),
          "query lost cancellation");
  context.settle();
}
int main() {
  try {
    FpState state;
    for (unsigned environment = 0; environment < 8; ++environment) {
      state.set(environment);
#if INPUT_RANK == 0
      verify<0>({});
#elif INPUT_RANK == 1
      for (auto n : {std::int64_t(0), std::int64_t(1), std::int64_t(17), std::int64_t(1048576),
                     std::int64_t(1) << 40})
        verify<1>({n});
      verify<1>({-1}, 1);
      verify<1>({INT64_MAX}, 2);
#elif INPUT_RANK == 2
      for (auto shape :
           {Shape{2, 3}, {0, 3}, {2, 0}, {0, INT64_MAX}, {INT64_MAX, 0}, {1, 1}, {17, 13}})
        verify<2>(shape);
      verify<2>({-1, 0}, 1);
      verify<2>({0, -1}, 1);
      verify<2>({INT64_MAX, 2}, 2);
#elif INPUT_RANK == 3
      for (auto shape : {Shape{2, 3, 4}, {0, 3, 4}, {2, 0, 4}, {2, 3, 0}, {1, 1, 1}})
        verify<3>(shape);
      verify<3>({2, -1, 4}, 1);
      verify<3>({INT64_MAX, 2, 2}, 2);
#else
      for (auto shape :
           {Shape{1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 3}, {2, 2, 2, 0, 2, 2, 2, 3}})
        verify<8>(shape);
      verify<8>({1, 1, 1, 1, 1, 1, 1, -1}, 1);
      verify<8>({INT64_MAX, 2, 2, 2, 2, 2, 2, 2}, 2);
#endif
      state.checkFlags();
    }
    cancelled<INPUT_RANK>();
    std::cout << "collection query " << describeNumeric(element)->name << " rank " << INPUT_RANK
#ifdef QUERY_LENGTH
              << " length"
#else
              << " shape"
#endif
              << ": " << cases << " cases, " << faults
              << " checked faults; exact metadata, no payload reads, eight FP states, allocation "
                 "failure, cancellation and retained owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
