#include "../runtime/Tensor.h"
#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Array = CpuTensor<element, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Array *, std::int64_t, std::int64_t, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    std::size_t cases = 0;
    for (std::int64_t count : {0, 1, 17, 4097}) {
      std::uint64_t allocations = 0;
      for (std::uint64_t fail = 0; fail <= allocations + 1; ++fail) {
        Invocation context;
        context.failAllocationAt(fail);
        Array result{};
        _mlir_ciface_kalandra_test_entry(&result, count, 3, &context);
        const bool failed = fail && fail <= allocations;
        require(context.status() == (failed ? 2 : 0), "generator allocation status mismatch");
        auto verify = [&] {
          require(result.sizes[0] == count, "generator length mismatch");
          for (std::int64_t i = 0; i < count; ++i)
            require(result.aligned[result.offset + i * result.strides[0]] == Cell(i % 17 + 3),
                    "generator index/order/capture mismatch");
        };
        std::optional<AllocationLease> retained;
        if (failed)
          require(!result.allocated && !result.aligned, "generator published partial result");
        else {
          verify();
          if (count) {
            retained = context.lease(result.allocated);
            require(bool(retained), "generator result has no owner");
          }
        }
        if (!fail) {
          allocations = context.allocations();
          require(allocations < 16, "generator allocated per element");
        }
        context.settle();
        require(!context.live() && context.allocations() == context.releases(), "generator leaked");
        if (!failed)
          verify();
        ++cases;
      }
    }
    for (std::int64_t count : {std::int64_t(-1), INT64_MAX}) {
      Invocation context;
      // INT64_MAX bytes is representable for int8/uint8. Exercise allocator
      // rejection deterministically instead of requesting exabytes from ASan.
      if (count == INT64_MAX && sizeof(Cell) == 1)
        context.failAllocationAt(1);
      Array result{};
      _mlir_ciface_kalandra_test_entry(&result, count, 3, &context);
      require(context.status() == (count < 0 ? 5 : 2), "generator count guard missing");
      require(!context.allocations() && !result.allocated, "invalid count reached allocation");
      context.settle();
      require(!context.live(), "invalid count leaked");
    }
    std::atomic<bool> cancelled{true};
    Invocation context;
    context.bindCancellation(&cancelled);
    Array result{};
    _mlir_ciface_kalandra_test_entry(&result, 4097, 3, &context);
    require(context.poll() < 0 && !result.allocated, "cancelled generator published output");
    context.settle();
    require(!context.live(), "cancelled generator leaked");
    // A prior fault must win over an already-requested cancellation.
    Invocation faulted;
    faulted.fault(1, 4, 2);
    faulted.bindCancellation(&cancelled);
    _mlir_ciface_kalandra_test_entry(&result, 17, 3, &faulted);
    require(faulted.status() == 1 && !result.allocated, "generator changed fault precedence");
    std::cout << "Array generation " << describeNumeric(element)->name << ": " << cases
              << " cases, exact values, allocation faults, retained owners, count guards and "
                 "cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
