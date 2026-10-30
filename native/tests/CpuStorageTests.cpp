#include "../runtime/Tensor.h"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "CPU storage assertion %d: %s\n", __LINE__, #x);                        \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
static_assert(sizeof(CpuTensor<NumericType::Int8, 0>) == 24);
static_assert(sizeof(CpuTensor<NumericType::Float64, 1>) == 40);
static_assert(sizeof(CpuTensor<NumericType::UInt32, 2>) == 56);
static_assert(std::is_standard_layout_v<CpuTensor<NumericType::Int64, 8>>);
template <NumericType Type> void tensors() {
  using T = NumericValue<Type>;
  std::optional<RetainedCpuTensor> retained;
  std::array<std::byte, 4 * sizeof(T)> pattern{};
  for (std::size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = std::byte((i * 71 + 13) & 255);
  {
    Invocation context;
    auto *data = static_cast<T *>(context.allocate(6 * sizeof(T)));
    CHECK(data);
    std::memcpy(data + 1, pattern.data(), pattern.size());
    CpuTensor<Type, 2> descriptor{data, data, 1, {2, 2}, {2, 1}};
    retained = retainCpuTensor(context, descriptor.view());
    CHECK(retained && retained->element() == Type && retained->bytes() == pattern.size() &&
          retained->offsetBytes() == sizeof(T) && retained->retainedBytes() == 6 * sizeof(T));
    descriptor.sizes = {99, 99};
    CHECK(retained->shape()[0] == 2 && retained->shape()[1] == 2);
    context.release(data);
    CHECK(!context.live());
  }
  CHECK(!std::memcmp(retained->data().data(), pattern.data(), pattern.size()));
  // Cross-thread immutable access and independent owned exports.
  std::thread reader(
      [&] { CHECK(!std::memcmp(retained->data().data(), pattern.data(), pattern.size())); });
  reader.join();
  Invocation output;
  auto copied = copyCpuTensor<Type, 2>(output, Type, retained->data(), retained->shape());
  CHECK(copied && output.live() == 1 && copied->strides[0] == 2 && copied->strides[1] == 1 &&
        copied->offset == 0);
  CHECK(!std::memcmp(copied->aligned, pattern.data(), pattern.size()));
  retained.reset();
  CHECK(!std::memcmp(copied->aligned, pattern.data(), pattern.size()));
  output.release(copied->allocated);
  CHECK(!output.live());
  // Type/width, rank, offset, alignment, stride and backing ownership validation.
  Invocation source;
  auto *data = static_cast<T *>(source.allocate(8 * sizeof(T)));
  CHECK(data);
  CpuTensor<Type, 1> good{data, data, 0, {2}, {1}};
  CHECK(retainCpuTensor(source, good.view()));
  auto bad = good.view();
  bad.element = static_cast<NumericType>(255);
  CHECK(!retainCpuTensor(source, bad));
  bad = good.view();
  bad.offset = -1;
  CHECK(!retainCpuTensor(source, bad));
  bad = good.view();
  bad.offset = INT64_MAX;
  CHECK(!retainCpuTensor(source, bad));
  bad = good.view();
  bad.aligned = reinterpret_cast<const void *>(reinterpret_cast<std::uintptr_t>(data) - 1);
  CHECK(!retainCpuTensor(source, bad));
  if constexpr (alignof(T) > 1) {
    bad = good.view();
    bad.aligned = reinterpret_cast<const std::byte *>(data) + 1;
    CHECK(!retainCpuTensor(source, bad));
  }
  std::array<std::int64_t, 1> stride{2};
  bad = good.view();
  bad.strides = stride;
  CHECK(!retainCpuTensor(source, bad));
  std::array<std::int64_t, 1> negative{-1};
  bad = good.view();
  bad.sizes = negative;
  CHECK(!retainCpuTensor(source, bad));
  std::array<std::int64_t, 1> oversized{9};
  bad = good.view();
  bad.sizes = oversized;
  CHECK(!retainCpuTensor(source, bad));
  Invocation foreign;
  CHECK(!retainCpuTensor(foreign, good.view()));
  source.settle();
  CHECK(!retainCpuTensor(source, good.view()));
  Invocation empty;
  CpuTensor<Type, 2> zero{
      reinterpret_cast<T *>(1), reinterpret_cast<T *>(3), INT64_MAX, {0, 3}, {-7, -1}};
  auto value = retainCpuTensor(empty, zero.view());
  CHECK(value && !value->bytes() && !value->retainedBytes());
  auto emptyCopy = copyCpuTensor<Type, 2>(empty, Type, {}, value->shape());
  CHECK(emptyCopy && empty.live() == 1 && emptyCopy->strides[0] == 3 && emptyCopy->strides[1] == 1);
  empty.release(emptyCopy->allocated);
  // Rank zero stores one element; canonical LLVM descriptors have no dimension arrays.
  Invocation scalar;
  std::array<std::byte, sizeof(T)> bits{};
  auto rankZero = copyCpuTensor<Type, 0>(scalar, Type, bits, {});
  CHECK(rankZero && retainCpuTensor(scalar, rankZero->view())->bytes() == sizeof(T));
  scalar.release(rankZero->allocated);
  Invocation rejected;
  auto other = Type == NumericType::Float32 ? NumericType::UInt32 : NumericType::Float32;
  CHECK((!copyCpuTensor<Type, 2>(rejected, other, pattern, std::array<std::int64_t, 2>{2, 2})));
  CHECK((!copyCpuTensor<Type, 1>(rejected, Type, pattern, std::array<std::int64_t, 2>{2, 2})));
  CHECK(!rejected.live());
  Invocation failure;
  failure.failAllocationAt(1);
  CHECK((!copyCpuTensor<Type, 2>(failure, Type, pattern, std::array<std::int64_t, 2>{2, 2})));
  CHECK(failure.status() == 2 && !failure.live());
}
template <NumericType Type> struct ScalarTask {
  using T = NumericValue<Type>;
  static bool construct(Invocation &, void *out, const Packet &packet) {
    std::memcpy(out, packet.data, sizeof(T));
    return true;
  }
  static void destroy(void *) noexcept {}
  static void resume(Task &task) { task.complete(task.frame()); }
  static void test() {
    std::array<std::byte, sizeof(T)> bits{};
    for (std::size_t i = 0; i < bits.size(); ++i)
      bits[i] = std::byte(0xa0 + i);
    T value{};
    std::memcpy(&value, bits.data(), sizeof(T));
    const auto *layout = numericScalarLayout(Type);
    CHECK(layout && layout->bytes == sizeof(T) && layout->alignment == alignof(T));
    const TaskEntry entry{1,          123,       sizeof(T), alignof(T), sizeof(T),
                          alignof(T), construct, destroy,   resume,     layout};
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      auto child = core->submit(group, entry, {123, &value, sizeof(T)});
      CHECK(child && core->drain() && core->failure(child).kind == TaskFailure::None);
      Invocation context;
      T first{}, second{};
      CHECK(core->observe(child, *layout, context, &first) &&
            core->observe(child, *layout, context, &second));
      CHECK(!std::memcmp(&first, bits.data(), sizeof(T)) &&
            !std::memcmp(&second, bits.data(), sizeof(T)));
      auto wrong = Type == NumericType::Float32 ? NumericType::UInt32 : NumericType::Float32;
      Invocation mismatched;
      alignas(16) std::array<std::byte, 16> out{};
      CHECK(!core->observe(child, *numericScalarLayout(wrong), mismatched, out.data()) &&
            mismatched.status() == 3);
      CHECK(core->close(group) && core->drain() && core->release(group));
      auto stats = core->stats();
      CHECK(!stats.records && !stats.groups && !stats.retainedBytes && !stats.retainedOwners);
    }
  }
};
template <NumericType Type> void test() {
  tensors<Type>();
  ScalarTask<Type>::test();
}
int main() {
  test<NumericType::Int8>();
  test<NumericType::Int16>();
  test<NumericType::Int32>();
  test<NumericType::Int64>();
  test<NumericType::UInt8>();
  test<NumericType::UInt16>();
  test<NumericType::UInt32>();
  test<NumericType::UInt64>();
  test<NumericType::Float32>();
  test<NumericType::Float64>();
  std::set<std::uint64_t> identities;
  for (auto type : numericTypes)
    CHECK(identities.insert(numericScalarLayout(type.type)->identity).second);
  CHECK(numericScalarLayout(NumericType::Float32)->identity == 0x4b414c46533332ULL);
  CHECK(!numericScalarLayout(static_cast<NumericType>(255)));
  const std::array<std::int64_t, 2> huge{INT64_MAX, INT64_MAX};
  CHECK(!cpuTensorBytes(NumericType::UInt64, huge));
  std::puts("10 CPU storage types: descriptor ABI, typed retention/copies, bounds, exact scalar "
            "task layouts, lifetime, allocation failures and 1/2/4-worker cleanup passed");
}
