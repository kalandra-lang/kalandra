#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include "llvm/ADT/APSInt.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto source = static_cast<NumericType>(SOURCE_TYPE),
               destination = static_cast<NumericType>(DESTINATION_TYPE);
using Input = NumericValue<source>;
using Output = NumericValue<destination>;
#include "ConversionOracle.h"
using InTensor = CpuTensor<source, TENSOR_RANK>;
using OutTensor = CpuTensor<destination, TENSOR_RANK>;
using Shape = std::array<std::int64_t, TENSOR_RANK>;
extern "C" void _mlir_ciface_kalandra_test_entry(OutTensor *, InTensor *, Invocation *);
static std::uint64_t cases = 0, faults = 0, comparisons = 0;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class Tensor> static std::int64_t offset(const Tensor &tensor, std::size_t flat) {
  auto at = tensor.offset;
  if constexpr (TENSOR_RANK > 0)
    for (std::size_t axis = TENSOR_RANK; axis > 0; --axis) {
      at += std::int64_t(flat % tensor.sizes[axis - 1]) * tensor.strides[axis - 1];
      flat /= tensor.sizes[axis - 1];
    }
  return at;
}
template <std::size_t Rank = TENSOR_RANK>
static void verify(const std::vector<std::uint64_t> &bits, std::array<std::int64_t, Rank> shape,
                   const std::vector<Expected> &expected, bool transpose = false,
                   unsigned fail = 0) {
  std::vector<Input> backing(bits.size() * 2 + 8, Input{17});
  CpuTensor<source, Rank> input{};
  input.allocated = backing.data();
  input.aligned = backing.data() + 1;
  input.offset = 2;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    std::int64_t stride = 2;
    for (std::size_t axis = Rank; axis > 0; --axis) {
      input.strides[axis - 1] = stride;
      stride *= shape[axis - 1];
    }
    if constexpr (Rank == 2)
      if (transpose) {
        input.strides[0] = 2;
        input.strides[1] = 2 * shape[0];
      }
  }
  for (std::size_t i = 0; i < bits.size(); ++i)
    input.aligned[offset(input, i)] = std::bit_cast<Input>(Raw<Input>(bits[i]));
  const auto before = backing;
  Invocation context;
  context.failAllocationAt(fail);
  CpuTensor<destination, Rank> result{};
  _mlir_ciface_kalandra_test_entry(&result, &input, &context);
  const bool valid = std::all_of(expected.begin(), expected.end(), [](auto e) { return e.valid; });
  const auto code = fail ? 2 : valid ? 0 : 4;
  require(context.status() == code, "tensor conversion status mismatch");
  if (code == 4)
    require(context.line() == 2 && context.column() == 3, "conversion lost fault location");
  ++cases;
  faults += code != 0;
  auto check = [&] {
    require(bool(result.aligned), "conversion result has no backing");
    if constexpr (Rank > 0)
      require(result.sizes == shape, "conversion changed shape");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto value = result.aligned[offset(result, i)];
      bool same = std::bit_cast<Raw<Output>>(value) == expected[i].bits;
#ifdef TOTAL_HARDWARE_CAST
      if constexpr (source == NumericType::Float32 && destination == NumericType::Float64)
        same = same || (std::isnan(std::bit_cast<Input>(Raw<Input>(bits[i]))) && std::isnan(value));
#endif
      require(same, "tensor conversion bits mismatch");
      ++comparisons;
    }
  };
  std::optional<AllocationLease> retained;
  if (code)
    require(!result.allocated && !result.aligned, "partial conversion result published");
  else {
    check();
    retained = context.lease(result.allocated);
    require(bool(retained), "conversion output not owned");
  }
  require(!std::memcmp(before.data(), backing.data(), backing.size() * sizeof(Input)),
          "conversion mutated borrowed input");
  context.settle();
  require(!context.live() && context.allocations() == context.releases(),
          "conversion owners leaked");
  if (!code)
    check(); // Retained output survives input invocation settlement.
}
template <std::size_t Rank = TENSOR_RANK>
static void metadata(std::int64_t extent, int code, unsigned fail = 0) {
  CpuTensor<source, Rank> input{};
  CpuTensor<destination, Rank> result{};
  Input cell{};
  input.allocated = input.aligned = &cell;
  if constexpr (Rank > 0) {
    input.sizes.fill(1);
    input.sizes.back() = extent;
    input.strides.fill(1);
  }
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&result, &input, &context);
  require(context.status() == code && !result.allocated, "invalid conversion metadata accepted");
  context.settle();
  require(!context.live() && context.allocations() == context.releases(), "metadata owners leaked");
  ++cases;
  ++faults;
}
template <std::size_t Rank = TENSOR_RANK> static bool tryCancellation(bool active, unsigned attempt) {
  const std::size_t size = Rank == 0 ? 1 : active ? (1048576u << std::min(attempt, 3u)) : 1;
  std::vector<Input> cells(size);
  CpuTensor<source, Rank> input{};
  CpuTensor<destination, Rank> result{};
  input.allocated = input.aligned = cells.data();
  if constexpr (Rank > 0) {
    input.sizes.fill(1);
    input.sizes.back() = size;
    input.strides.fill(size);
    input.strides.back() = 1;
  }
  Invocation context;
  std::atomic<bool> token{!active}, done{false}, ready{false};
  std::atomic<std::uint64_t> polls{0};
  context.bindCancellation(&token);
  context.watchPolls(&polls);
  std::thread sender;
  if (active)
    sender = std::thread([&] {
      ready.store(true);
      // Yielding here can deschedule the sender longer than an optimized
      // copy takes. Stay ready until the invocation has entered its poll path.
      while (!done.load() && polls.load() < 4)
        std::atomic_signal_fence(std::memory_order_seq_cst);
      if (!done.load())
        token.store(true);
    });
  // Ensure this is an active-cancellation test rather than a thread-start race.
  while (active && !ready.load())
    std::this_thread::yield();
  _mlir_ciface_kalandra_test_entry(&result, &input, &context);
  done.store(true);
  if (active)
    sender.join();
  if (active && !token.load()) {
    // A descheduled sender can miss the entire kernel. No cancellation was
    // requested in that attempt, so verify ordinary completion/cleanup and
    // retry a larger workload. Never excuse publication after a real request.
    require(!context.status() && result.allocated, "unsignalled conversion failed");
    context.settle();
    require(!context.live() && context.allocations() == context.releases(),
            "unsignalled conversion owners leaked");
    std::cout << "Cancellation sender missed completed kernel; retrying active control\n";
    return false;
  }
  if (!(token.load() && context.poll() == -1 && !context.status() && !result.allocated))
    std::cerr << "cancellation active=" << active << " requested=" << token.load()
              << " polls=" << polls.load() << " status=" << context.status()
              << " published=" << bool(result.allocated) << '\n';
  require(token.load() && context.poll() == -1 && !context.status() && !result.allocated,
          "conversion did not cancel without publishing output");
  context.settle();
  require(!context.live() && context.allocations() == context.releases(),
          "cancelled owners leaked");
  return true;
}
template <std::size_t Rank = TENSOR_RANK> static void cancellation(bool active) {
  for (unsigned attempt = 0; attempt != 8; ++attempt)
    if (tryCancellation<Rank>(active, attempt))
      return;
  require(false, "sender never requested active cancellation");
}
int main() {
  try {
    const auto inputs = conversionInputs();
    std::vector<std::uint64_t> good, bad;
    std::vector<Expected> wanted;
    for (auto bits : inputs) {
      auto expected = reference(bits);
      if (expected.valid) {
        good.push_back(bits);
        wanted.push_back(expected);
      } else
        bad.push_back(bits);
    }
    struct Batch {
      std::vector<std::uint64_t> bits;
      Shape shape;
      std::vector<Expected> expected;
    };
    std::vector<Batch> batches;
    // Rank-two runs the full scalar corpus; other ranks qualify topology and representative bits.
    const auto total = TENSOR_RANK == 2 ? good.size() : std::min<std::size_t>(good.size(), 1024);
    const std::size_t block = TENSOR_RANK == 0 ? 1 : 512;
    for (std::size_t start = 0; start < total; start += block) {
      auto end = std::min(total, start + block);
      Batch batch{{good.begin() + start, good.begin() + end},
                  {},
                  {wanted.begin() + start, wanted.begin() + end}};
      batch.shape.fill(1);
      if constexpr (TENSOR_RANK > 0)
        batch.shape.back() = end - start;
      if constexpr (TENSOR_RANK == 2)
        if (end - start == 512) {
          batch.shape[0] = 16;
          batch.shape[1] = 32;
        }
      if constexpr (TENSOR_RANK == 3)
        if (end - start == 512) {
          batch.shape[0] = 2;
          batch.shape[1] = 8;
          batch.shape[2] = 32;
        }
      batches.push_back(std::move(batch));
    }
    // Invalid values sampled across the entire corpus, including boundaries, in each lane position.
    for (std::size_t i = 0; i < std::min<std::size_t>(bad.size(), 64); ++i)
      for (unsigned position = 0; position < (TENSOR_RANK == 0 ? 1u : 3u); ++position) {
        Batch batch;
        batch.shape.fill(1);
        batch.bits.assign(TENSOR_RANK == 0 ? 1 : 3, 0);
        if constexpr (TENSOR_RANK > 0)
          batch.shape.back() = 3;
        batch.bits[position] = bad[i * bad.size() / std::min<std::size_t>(bad.size(), 64)];
        for (auto bits : batch.bits)
          batch.expected.push_back(reference(bits));
        batches.push_back(std::move(batch));
      }
    {
      FpState state;
      const unsigned environments =
#ifdef TOTAL_HARDWARE_CAST
          1;
#else
          8;
#endif
      for (unsigned environment = 0; environment < environments; ++environment) {
        state.set(environment);
        for (const auto &batch : batches)
          verify(batch.bits, batch.shape, batch.expected, true);
#ifndef TOTAL_HARDWARE_CAST
        state.checkFlags();
#endif
      }
    }
    Shape shape;
    shape.fill(1);
    verify({0}, shape, {reference(0)}, false, 1);
    if constexpr (TENSOR_RANK > 0) {
      for (std::size_t axis = 0; axis < TENSOR_RANK; ++axis) {
        shape.fill(2);
        shape[axis] = 0;
        verify({}, shape, {});
      }
      metadata(-1, 1);
      constexpr auto bytes = std::max(sizeof(Input), sizeof(Output));
      constexpr std::int64_t limit = INT64_MAX / bytes;
      constexpr auto beyond = limit + (bytes > 1 ? 1 : 0);
      if constexpr (bytes > 1)
        metadata(beyond, 2);
      // For byte cells INT64_MAX is representable: inject allocation failure.
      metadata(limit, 2, 1);
    }
    cancellation(false);
    if (TENSOR_RANK == 2 &&
#ifdef TOTAL_HARDWARE_CAST
        true)
#else
        ((source == NumericType::Float64 && destination == NumericType::Float32) ||
         isNarrowInteger(source) || isNarrowInteger(destination)))
#endif
      cancellation(true);
    std::cout
        << "tensor conversion " << describeNumeric(source)->name << " -> "
        << describeNumeric(destination)->name << " rank " << TENSOR_RANK << ": " << cases
        << " cases, " << faults << " faults, " << comparisons
        << " APFloat lane checks; immutable strided views, retained results, "
           "checked metadata and cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
