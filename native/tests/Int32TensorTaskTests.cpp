#include "../runtime/Tasks.h"
#include "../runtime/Tensor.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>
#ifndef TENSOR_RANK
#define TENSOR_RANK 1
#endif
#ifndef INTEGER_BITS
#define INTEGER_BITS 32
#endif
#ifndef FLOAT_RESULT
#define FLOAT_RESULT 0
#endif
#ifndef UNSIGNED_RESULT
#define UNSIGNED_RESULT 0
#endif
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = FLOAT_RESULT == 64 ? NumericType::Float64
                         : FLOAT_RESULT     ? NumericType::Float32
                         : UNSIGNED_RESULT  ? (INTEGER_BITS == 8    ? NumericType::UInt8
                                               : INTEGER_BITS == 16 ? NumericType::UInt16
                                               : INTEGER_BITS == 64 ? NumericType::UInt64
                                                                    : NumericType::UInt32)
                                            : (INTEGER_BITS == 8    ? NumericType::Int8
                                               : INTEGER_BITS == 16 ? NumericType::Int16
                                               : INTEGER_BITS == 64 ? NumericType::Int64
                                                                    : NumericType::Int32);
using Tensor = CpuTensor<element, TENSOR_RANK>;
struct Result {
  Failure failure;
  Tensor value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
static void settled(const TaskStats &s) {
  check(!s.retainedBytes && !s.retainedOwners && !s.unfinished && !s.records && !s.groups &&
            !s.ready && !s.pending && !s.maintenanceTasks && !s.externalPending,
        "scheduler retained live state");
}
template <NumericType E = element> static std::vector<NumericValue<E>> expected(int scenario) {
#ifdef CONVERSION_RESULT
  if (scenario == 4)
    return {};
  if (scenario == 3)
    return {2, 4, 6, 8};
#endif
  if constexpr (isNarrowInteger(E)) {
    using Value = NumericValue<E>;
    if (scenario == 0)
      return {42};
    if (scenario == 1)
      return {std::numeric_limits<Value>::min(), std::numeric_limits<Value>::max(), Value(42),
              Value(0)};
    if (scenario == 2)
      return {44, UNSIGNED_RESULT ? Value(12) : Value(-8), 12, 22};
    if (scenario == 3)
      return {2, 4, 6, 8};
    if (scenario == 4)
      return {};
    if (scenario == 5)
      return {1, std::numeric_limits<Value>::max()};
    return {};
  } else if constexpr (E == NumericType::Float64) {
    if (scenario == 0)
      return {18014398509481988.0};
    if (scenario == 1)
      return {-0.0, 1.0000000000000002, 4.9406564584124654e-324, 1.7976931348623157e308};
    if (scenario == 2)
      return {9007199254740992.0, 12, 102, 202};
    if (scenario == 3)
      return {2, 4, 6, 8};
    if (scenario == 5)
      return {1.0000000000000002, -0.0};
    if (scenario == 10)
      return {0, 0, 0, 0};
    if (scenario == 9)
      return {std::bit_cast<double>(UINT64_C(0x7ff0000000000000)),
              std::bit_cast<double>(UINT64_C(0xfff0000000000000)),
              std::bit_cast<double>(UINT64_C(0x7ff8000000000000)), -0.0};
    return {};
  } else if constexpr (E == NumericType::Float32)
    return {2, 3, 4, 5};
  else if constexpr (E == NumericType::UInt64) {
    if (scenario == 0)
      return {18014398509481988ULL};
    if (scenario == 1)
      return {0, UINT64_MAX, 9223372036854775808ULL, 9007199254740993ULL};
    if (scenario == 2)
      return {9007199254740995ULL, 12, 102, 202};
    if (scenario == 3)
      return {2, 4, 6, 8};
    if (scenario == 5)
      return {1, UINT64_MAX};
    return {};
  } else if constexpr (E == NumericType::Int64) {
    if (scenario == 0)
      return {18014398509481988LL};
    if (scenario == 1)
      return {INT64_MIN, INT64_MAX, 9007199254740993LL, -5};
    if (scenario == 2)
      return {9007199254740995LL, -8, 102, 202};
    if (scenario == 3)
      return {2, 4, 6, 8};
    if (scenario == 5)
      return {1, INT64_MAX};
    return {};
  } else if constexpr (E == NumericType::UInt32) {
    if (scenario == 0)
      return {4294967294};
    if (scenario == 1)
      return {0, UINT32_MAX, 2147483648, 16777217};
    if (scenario == 2)
      return {2147483650, 12, 102, 202};
    if (scenario == 3)
      return {2, 4, 6, 8};
    if (scenario == 5)
      return {1, UINT32_MAX};
    return {};
  } else {
    if (scenario == 0)
      return {33554436};
    if (scenario == 1)
      return {INT32_MIN, INT32_MAX, 16777217, -5};
    if (scenario == 2)
      return {16777219, -8, 102, 202};
    if (scenario == 3)
      return {2, 4, 6, 8};
    if (scenario == 4)
      return {};
    if (scenario == 5)
      return {1, INT32_MAX};
    return {};
  }
}
static void cells(const RetainedCpuTensor &value, int scenario) {
  auto want = expected(scenario);
  check(value.element() == element && value.bytes() == want.size() * sizeof(NumericValue<element>),
        "wrong retained type/bytes");
  auto shape = value.shape();
  check(shape.size() == TENSOR_RANK, "wrong rank");
  if constexpr (TENSOR_RANK == 1)
    check(shape[0] == std::int64_t(want.size()), "wrong vector extent");
  if constexpr (TENSOR_RANK == 2)
    check(shape[0] == 2 && shape[1] == (scenario == 4 ? 0 : 2), "wrong matrix extents");
  if constexpr (TENSOR_RANK == 3)
    check(shape[0] == 2 && shape[1] == 1 && shape[2] == 2, "wrong rank-three extents");
  if (!want.empty())
    check(std::memcmp(value.data().data(), want.data(), value.bytes()) == 0,
          "wrong returned cell bits");
}
int main(int argc, char **argv) {
#ifdef CONVERSION_RESULT
  if (argc != 4 && argc != 5)
#else
  if (argc != 4)
#endif
    return 2;
  const auto workers = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
  const auto scenario = std::atoi(argv[2]);
  const auto mode = std::atoi(
      argv[3]); // 0 success/fault, 1 cancellation, 2 deadline, 3 allocation sweep, 4 admission.
  TaskLimits limits;
  limits.workers = workers;
  if (mode == 4)
    limits.bytes = 1;
  auto core = TaskCore::create(limits);
  check(bool(core), "core creation failed");
#ifdef CONVERSION_RESULT
  if (argc == 5)
    core->failAllocationAt(std::strtoull(argv[4], nullptr, 10));
#endif
  auto *entry = kalandra_task_test_entry;
  check(entry && entry->result && entry->packetBytes == 0 &&
            entry->result->bytes == sizeof(Result) && entry->result->alignment == alignof(Result),
        "wrong generated native task export ABI");
#ifdef ARRAY_RESULT
  check(entry->result->identity == 0x40000 + static_cast<unsigned>(element),
        "array task layout was confused with a tensor/scalar");
#else
  if constexpr (element != NumericType::Float32)
    check(entry->result->identity == 0x30000 + 16 * static_cast<unsigned>(element) + TENSOR_RANK,
          "wrong typed tensor layout identity");
#endif
  auto group = core->openRoot();
  check(bool(group), "root group failed");
  Result out{}, again{};
  std::optional<RetainedCpuTensor> retained;
  {
    Invocation destination;
    Packet packet{entry->packetLayout, nullptr, 0, &destination};
    auto task = core->submit(group, *entry, packet);
    if (mode == 1 || mode == 2)
      check(core->cancel(group, mode == 2), "cancellation request rejected");
    check(core->drain(), "root failed to settle");
    auto failure = core->failure(group);
    if (failure.kind == TaskFailure::None && task)
      failure = core->failure(task);
    if (failure.kind == TaskFailure::None && task) {
      const bool observed = core->observe(task, *entry->result, destination, &out);
      if (!observed && mode == 3) {
        check(destination.status() == 2 && !out.value.allocated,
              "failed observation published a tensor");
        failure = {TaskFailure::Fault, destination.status()};
      } else {
        check(observed, "result observation failed");
        failure = out.failure;
      }
      if (failure.kind == TaskFailure::None) {
        if (mode != 3) {
          // Same-width, same-callback impostors must never publish or retain data.
          ValueLayout wrong = *entry->result;
          if constexpr (INTEGER_BITS == 64 && !FLOAT_RESULT) {
            constexpr auto opposite = UNSIGNED_RESULT ? NumericType::Int64 : NumericType::UInt64;
            wrong.identity = 0x30000 + 16 * static_cast<unsigned>(opposite) + TENSOR_RANK;
          } else
            wrong.identity ^= 0x100000;
          if constexpr (element == NumericType::Float64) {
            for (auto impostor : {NumericType::Float32, NumericType::Int64, NumericType::UInt64}) {
              auto other = *entry->result;
              other.identity = impostor == NumericType::Float32
                                   ? TENSOR_RANK + 2
                                   : 0x30000 + 16 * static_cast<unsigned>(impostor) + TENSOR_RANK;
              Invocation invalid;
              alignas(Result) std::array<std::byte, sizeof(Result)> bytes;
              bytes.fill(std::byte{0x5a});
              const auto before = bytes;
              check(!core->observe(task, other, invalid, bytes.data()) && invalid.status() == 3 &&
                        bytes == before && !invalid.live(),
                    "numeric impostor published tensor data");
            }
          }
          Invocation invalid;
          alignas(Result) std::array<std::byte, sizeof(Result)> untouched;
          untouched.fill(std::byte{0x5a});
          auto before = untouched;
          check(!core->observe(task, wrong, invalid, untouched.data()) && invalid.status() == 3 &&
                    untouched == before && !invalid.live(),
                "mismatched result layout published data");
          check(core->observe(task, *entry->result, destination, &again),
                "cached result observation failed");
          check(again.failure.kind == TaskFailure::None, "cached result changed variant");
        }
        retained = retainCpuTensor(destination, out.value.view());
        check(bool(retained), "result backing not owned");
        auto second =
            retainCpuTensor(destination, mode == 3 ? out.value.view() : again.value.view());
        check(bool(second), "cached result backing invalid");
        cells(*retained, scenario);
        cells(*second, scenario);
      }
    }
    if (mode == 1 || mode == 2)
      check(failure.kind == (mode == 1 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
            "wrong cancellation kind");
    else if (mode == 4)
      check(failure.kind == TaskFailure::AdmissionRejected, "wrong admission kind");
    else if (scenario == 6)
      check(failure.kind == TaskFailure::Fault && failure.code == 1 && !retained,
            "shape failure exposed absent payload");
    else if (scenario == 7)
      check(failure.kind == TaskFailure::Fault && failure.code == 4 && failure.line == 2 &&
                failure.column == 3 && !retained,
            "arithmetic failure lost kind/location");
    else if (mode == 3 && failure.kind != TaskFailure::None)
      check(((failure.kind == TaskFailure::Fault && (failure.code == 2 || failure.code == 4))
#ifdef CONVERSION_RESULT
             || failure.kind == TaskFailure::AdmissionRejected
#endif
             ) &&
                !retained,
            "allocation sweep produced invalid failure");
    else {
      if (failure.kind != TaskFailure::None || !retained)
        std::cerr << "failure " << static_cast<unsigned>(failure.kind) << " code " << failure.code
                  << " source " << failure.line << ":" << failure.column << " observed "
                  << bool(retained) << "\n";
      check(failure.kind == TaskFailure::None && bool(retained), "successful tensor task failed");
    }
    check(core->close(group) && core->drain() && core->release(group), "root group release failed");
    settled(core->stats());
    core.reset();
    if (retained)
      cells(*retained, scenario); // All workers and task outcome owners are gone.
    destination.settle();
    check(!destination.live() && destination.allocations() == destination.releases(),
          "destination owners leaked");
  }
  if (retained)
    cells(*retained, scenario); // Retained value survives its final Invocation too.
  retained.reset();
  std::cout << describeNumeric(element)->name << " tensor task scenario " << scenario << " workers "
            << workers << " mode " << mode
            << ": exact cells, typed layouts and settled lifetimes passed\n";
}
