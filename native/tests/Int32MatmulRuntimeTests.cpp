#include "../runtime/Tensor.h"
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>
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
constexpr Cell squareLimit = INTEGER_BITS == 8    ? Cell(15)
                             : INTEGER_BITS == 16 ? Cell(255)
                             : INTEGER_BITS == 64 ? Cell(4294967295ULL)
                                                  : Cell(65535);
constexpr auto minimum = std::numeric_limits<Cell>::min();
constexpr auto maximum = std::numeric_limits<Cell>::max();
using Tensor = CpuTensor<element, 2>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Tensor *, Invocation *);
static unsigned cases = 0, faults = 0;
struct View {
  std::vector<Cell> storage;
  Tensor tensor{};
  View(std::int64_t rows, std::int64_t columns, const std::vector<Cell> &values, bool transposed)
      : storage(2 * rows * columns + 3 * (rows + columns) + 8, Cell(42)) {
    tensor.allocated = storage.data();
    tensor.aligned = storage.data() + 1;
    tensor.offset = 2;
    tensor.sizes = {rows, columns};
    tensor.strides = transposed ? std::array<std::int64_t, 2>{2, 2 * rows + 3}
                                : std::array<std::int64_t, 2>{2 * columns + 3, 2};
    for (std::int64_t r = 0; r < rows; ++r)
      for (std::int64_t c = 0; c < columns; ++c)
        tensor.aligned[tensor.offset + r * tensor.strides[0] + c * tensor.strides[1]] =
            values[r * columns + c];
  }
};
static bool verify(std::int64_t rows, std::int64_t inner, std::int64_t columns, std::vector<Cell> a,
                   std::vector<Cell> b, unsigned fail = 0, bool alias = false) {
  a.resize(rows * inner);
  b.resize(inner * columns);
  if (alias)
    b = a;
  View left(rows, inner, a, false), right(inner, columns, b, true);
  auto beforeLeft = left.storage, beforeRight = right.storage;
  bool overflow = false;
  std::vector<Cell> expected;
  // Independent wider reference checks each product and every adjacent-tree sum.
  for (std::int64_t r = 0; r < rows && !overflow; ++r) {
    for (std::int64_t c = 0; c < columns && !overflow; ++c) {
      std::vector<Wide> tree;
      for (std::int64_t k = 0; k < inner; ++k) {
        auto product = Wide(a[r * inner + k]) * b[k * columns + c];
        if (product < minimum || product > maximum) {
          overflow = true;
          break;
        }
        tree.push_back(product);
      }
      while (tree.size() > 1 && !overflow) {
        std::vector<Wide> next;
        for (std::size_t i = 0; i < tree.size(); i += 2) {
          auto sum = tree[i] + (i + 1 < tree.size() ? tree[i + 1] : 0);
          if (sum < minimum || sum > maximum) {
            overflow = true;
            break;
          }
          next.push_back(sum);
        }
        tree = std::move(next);
      }
      if (!overflow)
        expected.push_back(tree.empty() ? 0 : Cell(tree[0]));
    }
  }
  Invocation context;
  context.failAllocationAt(fail);
  Tensor result{};
  _mlir_ciface_kalandra_test_entry(&result, &left.tensor, alias ? &left.tensor : &right.tensor,
                                   &context);
  auto code = fail ? 2 : overflow ? 4 : 0;
  ++cases;
  faults += code != 0;
  auto reject = [&](const char *message) {
    std::cerr << message << " case " << cases << " shape " << rows << ',' << inner << ',' << columns
              << " status " << context.status() << " expected " << code << '\n';
    return false;
  };
  if (context.status() != code)
    return reject("fault mismatch");
  auto cellsCorrect = [&] {
    if (result.sizes != std::array<std::int64_t, 2>{rows, columns})
      return false;
    for (std::int64_t r = 0; r < rows; ++r)
      for (std::int64_t c = 0; c < columns; ++c)
        if (!result.aligned ||
            result.aligned[result.offset + r * result.strides[0] + c * result.strides[1]] !=
                expected[r * columns + c])
          return false;
    return true;
  };
  std::optional<AllocationLease> retained;
  if (code) {
    if (result.allocated)
      return reject("partial tensor published");
    if (code == 4 && (context.line() != 2 || context.column() != 3))
      return reject("fault source lost");
  } else {
    if (!cellsCorrect())
      return reject("wrong cells");
    retained = context.lease(result.allocated);
    if (!retained)
      return reject("missing result owner");
  }
  if (left.storage != beforeLeft || right.storage != beforeRight)
    return reject("input mutated");
  context.settle();
  if (context.live() || context.allocations() != context.releases())
    return reject("owners not settled");
  if (!code && !cellsCorrect())
    return reject("retained result lost after settlement");
  return true;
}
static bool metadata(std::array<std::int64_t, 2> a, std::array<std::int64_t, 2> b, int code,
                     unsigned fail = 0) {
  Cell value = 1;
  Tensor x{}, y{}, out{};
  x.allocated = x.aligned = y.allocated = y.aligned = &value;
  x.sizes = a;
  y.sizes = b;
  x.strides = y.strides = {1, 1};
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&out, &x, &y, &context);
  ++cases;
  faults += code != 0;
  if (context.status() != code || (code && out.allocated) ||
      (!code && out.sizes != std::array<std::int64_t, 2>{a[0], b[1]})) {
    std::cerr << "Metadata: a=" << a[0] << "," << a[1] << " b=" << b[0] << "," << b[1]
              << " status=" << context.status() << " expected=" << code << " out=" << out.allocated
              << "\n";
    return false;
  }
  context.settle();
  return !context.live() && context.allocations() == context.releases();
}
static bool cancelled() {
  // Poll-triggered cancellation exercises generated loops, without fixed sleeps.
  std::vector<Cell> a(257 * 8193, 1), b(8193 * 3, 0);
  View left(257, 8193, a, false), right(8193, 3, b, true);
  Invocation context;
  std::atomic<bool> token{false}, done{false}, ready{false};
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
  Tensor out{};
  while (!ready.load())
    std::this_thread::yield();
  _mlir_ciface_kalandra_test_entry(&out, &left.tensor, &right.tensor, &context);
  done.store(true);
  signal.join();
  bool ok = token.load() && context.poll() == -1 && !context.status() && !out.allocated;
  if (!ok)
    std::cerr << "Cancel: token=" << token.load() << " polls=" << polls.load()
              << " code=" << context.status() << " output=" << out.allocated << "\n";
  context.settle();
  return ok && !context.live() && context.allocations() == context.releases();
}
int main() {
  for (Cell value :
       {minimum, maximum, precision, (UNSIGNED_INPUT ? upperHalf : Cell(-1)), Cell(0), Cell(1)}) {
    if (!verify(1, 1, 1, {value}, {1}) ||
        !verify(1, 1, 1, {value}, {UNSIGNED_INPUT ? maximum : Cell(-1)}))
      return 1;
  }
  std::vector<std::vector<Cell>> boundaries;
  if constexpr (UNSIGNED_INPUT)
    boundaries = {{maximum, 1, 0},        {maximum, 0, 1, 0}, {maximum - 1, 1},
                  {upperHalf, upperHalf}, {maximum, maximum}, {precision, 1, 2, 3, 4}};
  else
    boundaries = {{maximum, 1, Cell(-1)},    {minimum, Cell(-1), 1}, {maximum, 0, 1, 0},
                  {minimum, 0, Cell(-1), 0}, {maximum, Cell(-1), 1}, {upperHalf, Cell(-upperHalf)},
                  {precision, 1, 2, 3, 4}};
  for (auto a : boundaries) {
    if (!verify(1, a.size(), 1, a, std::vector<Cell>(a.size(), a.size() == 2 ? 2 : 1)))
      return 1;
  }
  if constexpr (UNSIGNED_INPUT) {
    if (!verify(1, 2, 1, {upperHalf, Cell(upperHalf - 1)}, {1, 1}) ||
        !verify(1, 1, 1, {squareLimit}, {squareLimit}) ||
        !verify(1, 1, 1, {Cell(squareLimit + 1)}, {Cell(squareLimit + 1)}) ||
        !verify(1, 1, 1, {maximum}, {maximum}) || !verify(1, 1, 1, {maximum}, {0}))
      return 1;
  }
  // First cell succeeds; a later product/tree fault must not publish a tensor.
  if (!verify(1, 1, 2, {maximum}, {1, 2}) || !verify(1, 2, 2, {maximum, 1}, {0, 1, 0, 1}))
    return 1;
  for (auto shape :
       {std::array{0, 7, 3}, std::array{3, 7, 0}, std::array{2, 0, 3}, std::array{0, 0, 0},
        std::array{2, 1, 3}, std::array{3, 2, 5}, std::array{1, 255, 2}, std::array{1, 256, 2},
        std::array{1, 257, 2}, std::array{1, 1025, 1}}) {
    auto [rows, inner, cols] = shape;
    if (!verify(rows, inner, cols, std::vector<Cell>(rows * inner, 1),
                std::vector<Cell>(inner * cols, 1)))
      return 1;
  }
  if (!verify(2, 2, 2, {1, 2, 3, 4}, {}, 0, true))
    return 1;
  for (auto fail : {1u, 2u})
    if (!verify(2, 3, 2, {1, 2, 3, 4, 5, 6}, {1, 2, 3, 4, 5, 6}, fail))
      return 1;
  State state = 0x12345678;
  auto next = [&] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  for (unsigned trial = 0; trial < 1000; ++trial) {
    auto rows = next() % 5, inner = next() % 10, cols = next() % 6;
    std::vector<Cell> a(rows * inner), b(inner * cols);
    for (auto &v : a)
      if constexpr (UNSIGNED_INPUT)
        v = trial % 2 ? next() : next() % (INTEGER_BITS < 32 ? 4 : 201);
      else
        v = trial % 2 ? std::bit_cast<Cell>(static_cast<std::make_unsigned_t<Cell>>(next()))
            : INTEGER_BITS < 32 ? std::int64_t(next() % 7) - 3
                                : std::int64_t(next() % 201) - 100;
    for (auto &v : b)
      if constexpr (UNSIGNED_INPUT)
        v = trial % 2 ? next() : next() % (INTEGER_BITS < 32 ? 4 : 201);
      else
        v = trial % 2 ? std::bit_cast<Cell>(static_cast<std::make_unsigned_t<Cell>>(next()))
            : INTEGER_BITS < 32 ? std::int64_t(next() % 7) - 3
                                : std::int64_t(next() % 201) - 100;
    if (!verify(rows, inner, cols, a, b))
      return 1;
  }
  constexpr std::int64_t cellLimit = INT64_MAX / sizeof(Cell);
  // For byte elements INT64_MAX cells remain representable. Reject the actual
  // huge allocation deterministically; do not manufacture a wrapped extent.
  constexpr auto beyond = cellLimit + (sizeof(Cell) == 1 ? 0 : 1);
  if (!metadata({-1, 1}, {1, 1}, 1) || !metadata({1, -1}, {-1, 1}, 1) ||
      !metadata({0, 1}, {2, 0}, 1) || !metadata({1, 1}, {1, -1}, 1) ||
      !metadata({INT64_MAX, 0}, {0, 2}, 2) ||
      !metadata({1, INT64_MAX}, {INT64_MAX, 1}, 2, sizeof(Cell) == 1 ? 1 : 0) ||
      !metadata({0, INT64_MAX}, {INT64_MAX, 3}, 2) ||
      !metadata({3, INT64_MAX}, {INT64_MAX, 0}, 2) ||
      !metadata({beyond, 0}, {0, 1}, 2, sizeof(Cell) == 1 ? 1 : 0) ||
      !metadata({1, beyond}, {beyond, 0}, sizeof(Cell) == 1 ? 0 : 2) ||
      !metadata({0, beyond}, {beyond, 0}, 0) || !metadata({0, INT64_MAX}, {INT64_MAX, 0}, 0) ||
      !metadata({0, cellLimit}, {cellLimit, 1}, 0) ||
      !metadata({1, cellLimit}, {cellLimit, 0}, 0) || !cancelled())
    return 1;
  std::cout << describeNumeric(element)->name << " matmul: " << cases << " runtime inputs, "
            << faults
            << " checked faults; exact products/tree, strided views, retained results and settled "
               "owners passed\n";
}
