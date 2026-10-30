#include "../runtime/Tensor.h"
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>
#ifndef TENSOR_RANK
#define TENSOR_RANK 2
#endif
using namespace kalandra;
using namespace kalandra::runtime;
#ifndef TENSOR_UNSIGNED
#define TENSOR_UNSIGNED 0
#endif
static constexpr bool unsignedCells = TENSOR_UNSIGNED;
#ifndef TENSOR_BITS
#define TENSOR_BITS 64
#endif
static_assert(TENSOR_BITS == 8 || TENSOR_BITS == 16 || TENSOR_BITS == 64);
static constexpr NumericType element = unsignedCells ? (TENSOR_BITS == 8    ? NumericType::UInt8
                                                        : TENSOR_BITS == 16 ? NumericType::UInt16
                                                                            : NumericType::UInt64)
                                                     : (TENSOR_BITS == 8    ? NumericType::Int8
                                                        : TENSOR_BITS == 16 ? NumericType::Int16
                                                                            : NumericType::Int64);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<TENSOR_BITS == 8, std::uint8_t,
                               std::conditional_t<TENSOR_BITS == 16, std::uint16_t, std::uint64_t>>;
using Wide = std::conditional_t<unsignedCells, __uint128_t, __int128_t>;
static constexpr Cell maximum = std::numeric_limits<Cell>::max();
static constexpr Cell minimum = std::numeric_limits<Cell>::min();
static constexpr Cell highLiteral =
    unsignedCells ? Cell(UINT64_C(1) << (TENSOR_BITS - 1)) : minimum;
static constexpr Cell precisionLiteral = TENSOR_BITS == 64 ? Cell(9007199254740993) : Cell(42);
using Tensor = CpuTensor<element, TENSOR_RANK>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Tensor *, Cell, Invocation *);
static std::string_view operation;
static int mode;
static unsigned cases = 0, faults = 0;
template <std::size_t Rank>
static bool verify(std::array<std::int64_t, Rank> shape, std::vector<Cell> a, std::vector<Cell> b,
                   Cell scalar = 1, bool mismatch = false, unsigned fail = 0, bool alias = false) {
  std::size_t count = 1;
  for (auto extent : shape)
    count *= extent;
  a.resize(count);
  b.resize(count);
  if (alias)
    b = a;
  // Padded noncontiguous borrowed views, with both aligned-pointer and offset displacement.
  std::vector<Cell> left(2 * count + 8, Cell(1234567)), right(2 * count + 8, Cell(7654321));
  for (std::size_t i = 0; i < count; ++i) {
    left[3 + 2 * i] = a[i];
    right[3 + 2 * i] = b[i];
  }
  const auto beforeLeft = left, beforeRight = right;
  CpuTensor<element, Rank> x{}, y{}, result{};
  x.allocated = left.data();
  x.aligned = left.data() + 1;
  x.offset = 2;
  y.allocated = right.data();
  y.aligned = right.data() + 1;
  y.offset = 2;
  if constexpr (Rank > 0) {
    x.sizes = y.sizes = shape;
    std::int64_t stride = 2;
    for (std::size_t i = Rank; i > 0; --i) {
      x.strides[i - 1] = y.strides[i - 1] = stride;
      stride *= shape[i - 1];
    }
    if (mismatch)
      ++y.sizes[0];
  }
  bool arithmetic = false;
  std::vector<Wide> expected;
  if (mode == 4) {
    if constexpr (Rank == 0)
      expected = {scalar};
    else
      expected = {scalar, maximum, highLiteral, precisionLiteral};
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      const Wide p = mode == 2 ? scalar : a[i];
      const Wide q = mode == 5 ? 1 : mode == 1 ? scalar : b[i];
      Wide value = 0;
      if (operation == "add")
        value = p + q;
      else if (operation == "sub") {
        if (unsignedCells && p < q)
          arithmetic = true;
        else
          value = p - q;
      } else if (operation == "mul")
        value = p * q;
      else if (operation == "div" || operation == "rem") {
        if (q == 0 || (!unsignedCells && operation == "div" && p == minimum && q == Wide(-1)))
          arithmetic = true;
        else
          value = operation == "div" ? p / q : p % q;
      } else if (operation == "neg") {
        if constexpr (unsignedCells)
          arithmetic |= p != 0;
        else
          value = -p;
      } else if (operation == "pos")
        value = p;
      else
        return false;
      arithmetic |= value > maximum || value < minimum;
      expected.push_back(value);
    }
  }
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&result, &x, alias ? &x : &y, scalar, &context);
  const bool refinement =
      mode == 5 && std::any_of(shape.begin(), shape.end(), [](auto d) { return d != 2; });
  const auto code = (mismatch || refinement) ? 1 : fail ? 2 : arithmetic ? 4 : 0;
  ++cases;
  faults += code != 0;
  auto reject = [&](const char *why) {
    std::cerr << why << ": " << operation << " mode " << mode << " rank " << Rank << " count "
              << count << " status " << context.status() << " expected " << code << '\n';
    return false;
  };
  auto correct = [&] {
    if constexpr (Rank > 0) {
      auto wanted = shape;
      if (mode == 4) {
        wanted.fill(1);
        if constexpr (Rank == 1)
          wanted[0] = 4;
        else {
          wanted[0] = 2;
          wanted[1] = 2;
        }
      }
      if (result.sizes != wanted)
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
      auto flat = i;
      auto offset = result.offset;
      if constexpr (Rank > 0)
        for (std::size_t d = Rank; d > 0; --d) {
          offset += std::int64_t(flat % result.sizes[d - 1]) * result.strides[d - 1];
          flat /= result.sizes[d - 1];
        }
      if (!result.aligned || result.aligned[offset] != expected[i])
        return false;
    }
    return true;
  };
  if (context.status() != code)
    return reject("fault mismatch");
  std::optional<AllocationLease> lease;
  if (code) {
    if (result.allocated)
      return reject("partial result published");
    if (code == 4 && (context.line() != 2 || context.column() != 3))
      return reject("source location lost");
  } else {
    if (!correct())
      return reject("wrong integer cells");
    lease = context.lease(result.allocated);
    if (!lease)
      return reject("missing result owner");
  }
  if (left != beforeLeft || right != beforeRight)
    return reject("input mutated");
  context.settle();
  if (context.live() || context.allocations() != context.releases())
    return reject("owners not settled");
  return code || correct();
}
template <std::size_t Rank>
static bool metadata(std::array<std::int64_t, Rank> shape, int code, bool failAllocation = false) {
  Cell cell = 1;
  CpuTensor<element, Rank> input{}, result{};
  input.allocated = input.aligned = &cell;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    input.strides.fill(1);
  }
  Invocation context;
  if (failAllocation)
    context.failAllocationAt(1);
  _mlir_ciface_kalandra_test_entry(&result, &input, &input, 1, &context);
  ++cases;
  ++faults;
  if (context.status() != code || result.allocated)
    return false;
  context.settle();
  return !context.live() && context.allocations() == context.releases();
}
template <std::size_t Rank> static std::optional<bool> cancellationAttempt(std::size_t count) {
  // Only addition uses this probe; zero data avoids incidental arithmetic faults.
  std::vector<Cell> cells(count, 0);
  CpuTensor<element, Rank> input{}, result{};
  input.allocated = input.aligned = cells.data();
  if constexpr (Rank > 0) {
    input.sizes.fill(1);
    input.sizes.back() = cells.size();
    input.strides.fill(cells.size());
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
  _mlir_ciface_kalandra_test_entry(&result, &input, &input, 0, &context);
  done.store(true);
  signal.join();
  const bool completed = result.allocated && !context.status();
  const bool ok = token.load() && context.poll() == -1 && !context.status() && !result.allocated;
  context.settle();
  if (context.live() || context.allocations() != context.releases())
    return false;
  if (completed)
    return {}; // The kernel finished before the signal was observed: retry a larger workload.
  return ok;
}
template <std::size_t Rank> static bool cancelled() {
  for (const auto count : {1048576, 4194304, 16777216})
    if (const auto result = cancellationAttempt<Rank>(count))
      return *result;
  std::cerr << "cancellation signal was never observed\n";
  return false;
}
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  operation = argv[1];
  mode = argv[2][0] - '0';
  std::array<std::int64_t, TENSOR_RANK> shape{};
  shape.fill(1);
  if (mode == 4) {
    if (!verify(shape, {1}, {1}, maximum - 1) || !verify(shape, {1}, {1}, maximum - 1, false, 1))
      return 1;
  } else if (mode == 5) {
    shape.fill(2);
    if (!verify(shape, {1, 2, 3, 4}, {}, 1) || !verify(shape, {1, 2, 3, maximum}, {}, 1) ||
        !verify(shape, {1, 2, 3, 4}, {}, 1, false, 1))
      return 1;
    shape.fill(1);
    if (!verify(shape, {1}, {}, 1))
      return 1;
  } else {
    std::vector<Cell> edges;
    if constexpr (TENSOR_BITS == 64) {
      edges = {0,
               1,
               2,
               3,
               Cell(255),
               Cell(65536),
               Cell(16777217),
               Cell(4294967295),
               Cell(9007199254740991),
               Cell(9007199254740992),
               Cell(9007199254740993),
               Cell(3037000499),
               Cell(3037000500),
               maximum / 2,
               maximum - 1,
               maximum};
    } else {
      edges = {0, 1, 2, 3, Cell(maximum / 2), Cell(maximum - 1), maximum};
    }
    if constexpr (unsignedCells) {
      edges.push_back(highLiteral);
      edges.push_back(highLiteral + 1);
    } else {
      edges.insert(edges.end(), {minimum, minimum + 1, minimum / 2, Cell(-1), Cell(-2),
                                 TENSOR_BITS == 64 ? Cell(-3037000500) : Cell(-42)});
    }
    for (auto a : edges)
      for (auto b : edges)
        if (!verify(shape, {a}, {b}, mode == 2 ? a : b))
          return 1;
    std::uint64_t state = 0x123456789abcdef0;
    auto next = [&] {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      return std::bit_cast<Cell>(Raw(state));
    };
    for (unsigned trial = 0; trial < 1000; ++trial) {
      auto a = next(), b = next();
      if (!verify(shape, {a}, {b}, mode == 2 ? a : b))
        return 1;
    }
    if (!verify(shape, {6}, {2}, 2, false, 1))
      return 1;
    if constexpr (TENSOR_RANK > 0) {
      for (auto length : {0, 1, 3, 17}) {
        shape.fill(1);
        shape[0] = length;
        std::vector<Cell> a(length, 6), b(length, 2);
        if (!verify(shape, a, b, 2) || !verify(shape, a, b, 2, false, 0, true))
          return 1;
        if (mode == 0 && !verify(shape, a, b, 2, true))
          return 1;
      }
      shape.fill(2);
      std::size_t count = std::size_t(1) << TENSOR_RANK;
      std::vector<Cell> a(count), b(count, 2);
      for (std::size_t i = 0; i < count; ++i)
        a[i] = i;
      if (!verify(shape, a, b, 2))
        return 1;
      for (std::size_t axis = 0; axis < TENSOR_RANK; ++axis) {
        auto empty = shape;
        empty[axis] = 0;
        if (!verify(empty, {}, {}, 0))
          return 1;
      }
      auto invalid = shape;
      invalid[0] = -1;
      if (!metadata(invalid, 1))
        return 1;
      invalid.fill(1);
      if constexpr (sizeof(Cell) > 1) {
        invalid[0] = INT64_MAX / sizeof(Cell) + 1;
        if (!metadata(invalid, 2))
          return 1;
      } else if constexpr (TENSOR_RANK > 1) {
        invalid[0] = INT64_MAX;
        invalid[1] = 2;
        if (!metadata(invalid, 2))
          return 1;
      }
      // A one-byte rank-one extent can fit the byte budget while being too
      // large to materialize. Inject allocator failure before any input read.
      invalid.fill(1);
      invalid[0] = INT64_MAX;
      if (!metadata(invalid, 2, sizeof(Cell) == 1))
        return 1;
      if (operation != "pos") {
        shape.fill(1);
        shape[0] = 17;
        std::vector<Cell> left(17, mode == 2 ? 1 : 6), right(17, 2);
        Cell scalar = mode == 2 ? 6 : 2;
        if (operation == "add") {
          left.back() = maximum;
          right.back() = maximum;
          scalar = 1;
        }
        if (operation == "sub") {
          if (mode == 2) {
            right.back() = unsignedCells ? maximum : minimum;
            scalar = 6;
          } else {
            left.back() = minimum;
            right.back() = 1;
            scalar = 1;
          }
        }
        if (operation == "mul") {
          left.back() = maximum;
          right.back() = maximum;
          scalar = 2;
        }
        if (operation == "div" || operation == "rem") {
          if (mode == 2)
            right.back() = 0;
          else if (mode == 1)
            scalar = 0;
          else
            right.back() = 0;
        }
        if (operation == "neg") {
          std::fill(left.begin(), left.end(), 0);
          left.back() = unsignedCells ? 1 : minimum;
        }
        if (!verify(shape, left, right, scalar))
          return 1;
      }
    }
    if (operation == "add" && mode == 0 && !cancelled<TENSOR_RANK>())
      return 1;
  }
  std::cout << describeNumeric(element)->name << " tensor " << operation << " mode " << mode
            << " rank " << TENSOR_RANK << ": " << cases << " cases, " << faults
            << " faults; exact integer cells, immutable inputs and settled owners passed\n";
}
