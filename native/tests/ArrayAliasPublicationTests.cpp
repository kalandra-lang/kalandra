#include "../runtime/Tensor.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
using Array = CpuTensor<NumericType::Int64, 1>;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Array *, std::int64_t, Invocation *);
int main() {
  try {
    Invocation context;
    auto *storage = static_cast<std::int64_t *>(context.allocate(8));
    if (!storage) throw std::runtime_error("initial allocation failed");
    *storage = 0;
    Array input{storage, storage, 0, {1}, {1}};
    context.failAllocationAt(8);
    auto result = _mlir_ciface_kalandra_test_entry(&input, 10000, &context);
    if (context.status() || result != 10000 || *storage != 10000 || context.allocations() >= 8)
      throw std::runtime_error("repeated source alias publication grew ownership or lost writes");
    context.release(storage);
    context.releaseRecords();
    context.settle();
    if (context.live() || context.allocations() != context.releases())
      throw std::runtime_error("source alias publication leaked ownership");
    std::cout << "10000 source alias publications: constant ownership, visible writes, clean settlement passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
