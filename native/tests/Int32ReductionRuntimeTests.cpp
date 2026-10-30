#include "../runtime/Tensor.h"
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>
#ifndef TENSOR_RANK
#define TENSOR_RANK 1
#endif
using namespace kalandra;
using namespace kalandra::runtime;
#ifndef INTEGER_BITS
#define INTEGER_BITS 32
#endif
#ifndef UNSIGNED_INPUT
#define UNSIGNED_INPUT 0
#endif
static_assert(INTEGER_BITS == 8 || INTEGER_BITS == 16 || INTEGER_BITS == 32 || INTEGER_BITS == 64);
constexpr auto element = UNSIGNED_INPUT ? (INTEGER_BITS == 8    ? NumericType::UInt8
                                           : INTEGER_BITS == 16 ? NumericType::UInt16
                                           : INTEGER_BITS == 64 ? NumericType::UInt64
                                                                : NumericType::UInt32)
                                        : (INTEGER_BITS == 8    ? NumericType::Int8
                                           : INTEGER_BITS == 16 ? NumericType::Int16
                                           : INTEGER_BITS == 64 ? NumericType::Int64
                                                                : NumericType::Int32);
using Cell = NumericValue<element>;
using Wide = std::conditional_t<UNSIGNED_INPUT,
                                std::conditional_t<INTEGER_BITS == 64, __uint128_t, std::uint64_t>,
                                std::conditional_t<INTEGER_BITS == 64, __int128_t, std::int64_t>>;
using State = std::conditional_t<INTEGER_BITS == 64, std::uint64_t, std::uint32_t>;
constexpr Cell precision = INTEGER_BITS < 32    ? Cell(42)
                           : INTEGER_BITS == 64 ? Cell(9007199254740993LL)
                                                : Cell(16777217);
constexpr Cell upperHalf = std::numeric_limits<Cell>::max() / 2 + 1;
constexpr auto minimum = std::numeric_limits<Cell>::min();
constexpr auto maximum = std::numeric_limits<Cell>::max();
using Tensor = CpuTensor<element, TENSOR_RANK>;
using Raw = std::make_unsigned_t<Cell>;
extern "C" Raw _mlir_ciface_kalandra_test_entry(Tensor *, Invocation *);
static Cell invoke(Tensor *input, Invocation *context) {
  // The private MLIR interface returns only the declared low bits, without
  // platform C sign/zero-extension attributes. Materialize those bits before
  // interpreting their signedness; do not rely on caller promotion assumptions.
  volatile Raw bits = _mlir_ciface_kalandra_test_entry(input, context);
  return std::bit_cast<Cell>(Raw(bits));
}
static unsigned cases = 0, faults = 0;
template <std::size_t Rank>
static bool verify(std::vector<Cell> values, std::array<std::int64_t, Rank> shape,
                   unsigned fail = 0, std::int64_t metadataFault = 0) {
  // Independent wider reference: adjacent pairs, with an unchanged odd tail.
  auto tree = values;
  bool overflow = false;
  while (tree.size() > 1 && !overflow) {
    std::vector<Cell> next;
    for (std::size_t i = 0; i < tree.size(); i += 2) {
      const Wide value = Wide(tree[i]) + (i + 1 < tree.size() ? tree[i + 1] : 0);
      if (value < minimum || value > maximum) {
        overflow = true;
        break;
      }
      next.push_back(Cell(value));
    }
    tree = std::move(next);
  }
  const auto expected = tree.empty() ? 0 : tree[0];
  // Offset view with padded strides: do not assume contiguous foreign input.
  std::vector<Cell> storage(values.size() * 2 + 8, Cell(42));
  for (std::size_t i = 0; i < values.size(); ++i)
    storage[3 + 2 * i] = values[i];
  auto before = storage;
  CpuTensor<element, Rank> input{};
  input.allocated = storage.data();
  input.aligned = storage.data() + 1;
  input.offset = 2;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    std::int64_t stride = 2;
    for (std::size_t i = Rank; i > 0; --i) {
      input.strides[i - 1] = stride;
      // Invalid dimensions are checked before any reads; avoid host-side overflow.
      if (!metadataFault && !std::ranges::contains(shape, 0))
        stride *= shape[i - 1];
    }
  }
  Invocation context;
  context.failAllocationAt(fail);
  auto result = invoke(&input, &context);
  const auto code = metadataFault ? metadataFault : fail ? 2 : overflow ? 4 : 0;
  ++cases;
  faults += code != 0;
  if (context.status() != code || (!code && result != expected) || (code && result != 0) ||
      (code == 4 && (context.line() != 2 || context.column() != 3)) || storage != before) {
    std::cerr << "rank " << TENSOR_RANK << " case " << cases << " status " << context.status()
              << " expected " << code << " result " << result << " reference " << expected << '\n';
    return false;
  }
  context.settle();
  return context.live() == 0 && context.allocations() == context.releases();
}
template <std::size_t Rank> static std::optional<bool> cancellationAttempt(std::size_t count) {
  if constexpr (Rank == 0)
    count = 1;
  std::vector<Cell> storage(count, 0);
  storage[0] = 1;
  CpuTensor<element, Rank> input{};
  input.allocated = input.aligned = storage.data();
  if constexpr (Rank > 0) {
    input.sizes.fill(1);
    input.sizes.back() = count;
    input.strides.fill(count);
    input.strides.back() = 1;
  }
  Invocation context;
  std::atomic<bool> token{Rank == 0}, done{false}, ready{false};
  std::atomic<std::uint64_t> polls{0};
  context.bindCancellation(&token);
  context.watchPolls(&polls);
  std::thread signal([&] {
    ready.store(true);
    while (!done.load() && polls.load() < 4)
      std::this_thread::yield();
    if (!done.load())
      token.store(true);
  });
  while (!ready.load())
    std::this_thread::yield();
  auto value = invoke(&input, &context);
  done.store(true);
  signal.join();
  const bool completed = value == 1 && !context.status();
  const bool ok = token.load() && value == 0 && context.poll() == -1 && !context.status();
  context.settle();
  if (context.live() || context.allocations() != context.releases())
    return false;
  if (completed)
    return {};
  return ok;
}
template <std::size_t Rank> static bool cancelled() {
  for (const auto count : {1048576, 4194304, 16777216})
    if (const auto result = cancellationAttempt<Rank>(count))
      return *result;
  std::cerr << "cancellation signal was never observed\n";
  return false;
}
int main() {
  std::array<std::int64_t, TENSOR_RANK> shape{};
  shape.fill(1);
  for (Cell value :
       {minimum, maximum, precision, (UNSIGNED_INPUT ? upperHalf : Cell(-1)), Cell(0), Cell(1)})
    if (!verify({value}, shape) || !verify({value}, shape, 1))
      return 1;
  if (!cancelled<TENSOR_RANK>())
    return 1;
  if constexpr (TENSOR_RANK > 0) {
    for (auto size : {255, 256, 257, 1023, 1024, 1025, 32769}) {
      shape.fill(1);
      shape.back() = size;
      std::vector<Cell> values(size, 1);
      if (!verify(values, shape))
        return 1;
      values.back() = maximum;
      if (!verify(values, shape))
        return 1;
    }
    std::vector<std::vector<Cell>> boundaries;
    if constexpr (UNSIGNED_INPUT)
      boundaries = {{},
                    {maximum, 1, 0},
                    {maximum, 0, 1, 0},
                    {maximum, 0},
                    {0, maximum},
                    {maximum - 1, 1},
                    {upperHalf, Cell(upperHalf - 1)},
                    {upperHalf, upperHalf},
                    {0, 0, maximum},
                    {1, 0, maximum},
                    {precision, 1, 2, 3, 4},
                    {1, 2, 3, 4, 5, 6, 7}};
    else
      boundaries = {{},
                    {maximum, 1, Cell(-1)},
                    {minimum, Cell(-1), 1},
                    {maximum, 1, minimum, 0},
                    {maximum, 0, 1, 0},
                    {minimum, 0, Cell(-1), 0},
                    {maximum, minimum, 1},
                    {maximum, Cell(-1), 1},
                    {precision, 1, 2, 3, 4},
                    {1, 2, 3, 4, 5, 6, 7}};
    for (auto values : boundaries) {
      shape.fill(1);
      shape.back() = values.size();
      if (!verify(values, shape))
        return 1;
    }
    for (std::size_t axis = 0; axis < TENSOR_RANK; ++axis) {
      shape.fill(3);
      shape[axis] = 0;
      if (!verify({}, shape))
        return 1;
    }
    if constexpr (TENSOR_RANK > 1) {
      shape.fill(1);
      shape.front() = 0;
      shape.back() = INT64_MAX;
      if (!verify({}, shape))
        return 1;
    }
    State state = 0x9e3779b9;
    auto next = [&] {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      return state;
    };
    for (unsigned trial = 0; trial < 1200; ++trial) {
      std::size_t count = 1;
      for (auto &d : shape) {
        d = next() % 7;
        count *= d;
      }
      std::vector<Cell> values(count);
      for (auto &v : values)
        if constexpr (UNSIGNED_INPUT)
          v = trial % 2 ? next() : next() % 20001;
        else
          v = trial % 2 ? std::bit_cast<Cell>(static_cast<std::make_unsigned_t<Cell>>(next()))
                        : std::int64_t(next() % 20001) - 10000;
      if (!verify(values, shape))
        return 1;
    }
    shape.fill(1);
    shape[0] = -1;
    if (!verify({1}, shape, 0, 1))
      return 1;
    shape[0] = INT64_MAX / sizeof(Cell);
    if (sizeof(Cell) > 1)
      ++shape[0];
    if (!verify({1}, shape, sizeof(Cell) == 1 ? 1 : 0, 2))
      return 1;
    shape[0] = INT64_MAX;
    if (!verify({1}, shape, sizeof(Cell) == 1 ? 1 : 0, 2))
      return 1;
  }
  std::cout << describeNumeric(element)->name << " reduction rank " << TENSOR_RANK << ": " << cases
            << " runtime inputs, " << faults
            << " checked faults; exact adjacent tree, offsets/strides and settled owners passed\n";
}
