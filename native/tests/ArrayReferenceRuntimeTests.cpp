#include "../runtime/Tensor.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Array = CpuTensor<element, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Array *, Array *, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (const auto count : {0, 1, 17}) {
      Invocation context;
      auto *storage = static_cast<Cell *>(context.allocate(count * sizeof(Cell)));
      Array input{storage, storage, 0, {count}, {count ? 1 : 0}}, output{};
      for (int i = 0; i < count; ++i)
        storage[i] = static_cast<Cell>(i + 1);
      _mlir_ciface_kalandra_test_entry(&output, &input, &context);
      require(context.status() == 0, "reference roundtrip faulted");
      require((!count || (output.allocated == storage && output.aligned == storage)) &&
                  output.offset == 0 && output.sizes[0] == count,
              "array identity was copied at a function/aggregate boundary");
      for (int i = 0; i < count; ++i)
        require(output.aligned[i] == static_cast<Cell>(i + 1), "reference changed data");
      if (count) {
        storage[0] = static_cast<Cell>(42);
        require(output.aligned[0] == static_cast<Cell>(42), "returned reference lost its alias");
        auto retained = context.lease(output.allocated);
        require(bool(retained), "returned alias has no managed owner");
        context.settle();
        require(output.aligned[0] == static_cast<Cell>(42),
                "retained reference died at settlement");
      } else
        context.settle();
      require(context.live() == 0, "reference owners leaked after settlement");
    }
    for (const auto mode : {0, 1, 2}) {
      std::array<Cell, 3> raw{Cell(1), Cell(2), Cell(3)};
      Invocation context;
      std::atomic<bool> cancelled{mode == 2};
      if (mode == 1)
        context.failAllocationAt(1);
      if (mode == 2)
        context.bindCancellation(&cancelled);
      Array input{raw.data(), raw.data(), 0, {3}, {1}}, output{};
      _mlir_ciface_kalandra_test_entry(&output, &input, &context);
      std::optional<AllocationLease> retained;
      if (!mode) {
        require(!context.status() && output.allocated != raw.data() && output.sizes[0] == 3,
                "escaping unmanaged input was not imported");
        retained = context.lease(output.allocated);
        require(bool(retained), "imported reference has no managed owner");
        raw[1] = Cell(99);
        require(output.aligned[1] == Cell(2), "imported reference aliases unmanaged host storage");
      } else
        require(!output.allocated && !output.aligned &&
                    (mode == 1 ? context.status() == 2 : context.poll() < 0),
                "failed/stopped import published an array reference");
      context.settle();
      require(!context.live(), "unmanaged import leaked owners");
      if (!mode)
        require(output.aligned[2] == Cell(3), "imported reference died at settlement");
    }
    std::cout
        << "Array reference identity, visible aliases, retained lifetime and settlement passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
