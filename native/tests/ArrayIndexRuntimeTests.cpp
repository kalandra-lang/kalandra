#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
constexpr auto indexType = static_cast<NumericType>(INDEX_TYPE);
using Cell = NumericValue<element>;
using Position = NumericValue<indexType>;
using Raw = std::conditional_t<sizeof(Cell) == 8, std::uint64_t, std::uint32_t>;
using Tensor = CpuTensor<element, 1>;
extern "C" Cell _mlir_ciface_kalandra_test_entry(Tensor *, Position, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    std::uint64_t cases = 0, faults = 0;
    FpState state;
    for (unsigned environment = 0; environment < 8; ++environment) {
      state.set(environment);
      for (std::size_t length : {0u, 1u, 2u, 17u, 257u}) {
        for (std::int64_t stride : {1, 2, -1}) {
          std::vector<Cell> cells(2 * length + 8);
          for (std::size_t i = 0; i < cells.size(); ++i) {
            Raw bits = Raw(0xf17deacb1297ff83ULL) * Raw(i + 1);
            // Includes signed zeros, infinities, subnormals and NaN payloads.
            const Raw edges[] = {Raw(0),
                                 Raw(1),
                                 Raw(-1),
                                 sizeof(Cell) == 4 ? Raw(0x80000000) : Raw(0x8000000000000000ULL),
                                 sizeof(Cell) == 4 ? Raw(0x7f800000) : Raw(0x7ff0000000000000ULL),
                                 sizeof(Cell) == 4 ? Raw(0x7f800001) : Raw(0x7ff0000000000001ULL),
                                 sizeof(Cell) == 4 ? Raw(0x7fc12345) : Raw(0x7ff812345678abcdULL)};
            if (i < 7)
              bits = edges[i];
            cells[i] = std::bit_cast<Cell>(bits);
          }
          const auto original = cells;
          Tensor input{};
          input.allocated = cells.data();
          input.aligned = cells.data() + 1;
          input.offset = stride < 0 ? std::int64_t(length) + 1 : 1;
          input.sizes = {std::int64_t(length)};
          input.strides = {stride};
          std::vector<Position> indices;
          for (std::size_t i = 0; i < length + 2; ++i)
            indices.push_back(Position(i));
          indices.push_back(std::numeric_limits<Position>::max());
          indices.push_back(std::numeric_limits<Position>::min());
          indices.push_back(Position(-1));
          for (auto position : indices) {
            Invocation context;
            auto output = _mlir_ciface_kalandra_test_entry(&input, position, &context);
            const bool valid = std::uint64_t(position) < length;
            require(context.status() == (valid ? 0 : 5), "index did not report Bounds fault");
            if (valid) {
              const auto physical = 1 + input.offset + std::int64_t(position) * stride;
              require(std::bit_cast<Raw>(output) == std::bit_cast<Raw>(original[physical]),
                      "element bits or descriptor addressing changed");
            } else {
              ++faults;
              require(context.line() == 2 && context.column() == 3 &&
                          std::bit_cast<Raw>(output) == 0,
                      "bounds fault location or result changed");
            }
            require(!context.allocations() &&
                        !std::memcmp(cells.data(), original.data(), cells.size() * sizeof(Cell)),
                    "index allocated or mutated input");
            context.settle();
            require(!context.live(), "index leaked owners");
            ++cases;
          }
        }
      }
      state.checkFlags();
    }
    Tensor invalid{};
    for (auto extent : {std::int64_t(-1), INT64_MAX}) {
      invalid.sizes = {extent};
      invalid.strides = {1};
      Invocation context;
      auto output = _mlir_ciface_kalandra_test_entry(&invalid, Position{}, &context);
      require(context.status() == (extent < 0 ? 1 : 2) && std::bit_cast<Raw>(output) == 0 &&
                  !context.allocations(),
              "invalid metadata reached element read");
      context.settle();
    }
    Invocation context;
    std::atomic<bool> cancel{true};
    context.bindCancellation(&cancel);
    auto output = _mlir_ciface_kalandra_test_entry(&invalid, Position{}, &context);
    require(context.poll() == -1 && !context.status() && std::bit_cast<Raw>(output) == 0,
            "cancelled index performed read");
    context.settle();
    std::cout << "array index " << describeNumeric(element)->name << "/"
              << describeNumeric(indexType)->name << ": " << cases << " cases, " << faults
              << " bounds faults; exact bits, strided input, eight FP states, metadata and "
                 "cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
