#include "Metal.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Typed Metal storage assertion %d: %s\n", __LINE__, #x);                \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
void empty(m::Runtime &runtime) {
  auto s = runtime.stats();
  CHECK(!s.bytes && !s.hostBackings && !s.deviceBackings && !s.metalBuffers && !s.inFlight &&
        !s.stagingBuffers && !s.argumentBytes && !s.argumentPacks);
}
m::Buffer observe(TaskCore &core, TaskHandle child) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(child, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
template <NumericType Type, std::size_t Rank = 1> void roundtrip(m::Runtime &runtime) {
  using T = NumericValue<Type>;
  std::optional<m::Buffer> host;
  constexpr std::size_t count = Rank == 0 ? 1 : 3;
  std::array<std::byte, count * sizeof(T)> pattern{};
  std::array<std::int64_t, Rank> sizes{}, strides{};
  sizes.fill(1);
  strides.fill(count);
  if constexpr (Rank > 0) {
    sizes.back() = count;
    strides.back() = 1;
  }
  for (std::size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = std::byte((i * 83 + 31) & 255);
  {
    Invocation source;
    auto *data = static_cast<T *>(source.allocate(5 * sizeof(T)));
    CHECK(data);
    std::memcpy(data + 1, pattern.data(), pattern.size());
    CpuTensor<Type, Rank> descriptor{};
    descriptor.allocated = descriptor.aligned = data;
    descriptor.offset = 1;
    if constexpr (Rank > 0) {
      descriptor.sizes = sizes;
      descriptor.strides = strides;
    }
    auto imported = runtime.fromCpu(source, descriptor.view());
    CHECK(imported);
    host = *imported;
    CHECK(storageNumericType(host->format()) == Type && host->bytes() == pattern.size());
    CHECK(host->hostBytes()->data() == reinterpret_cast<const std::byte *>(data + 1));
    CHECK(runtime.stats().bytes == 5 * sizeof(T));
    source.settle();
    CHECK(!source.live());
  }
  Invocation output;
  if constexpr (Type == NumericType::Float64)
    CHECK((!runtime.toCpuTyped<NumericType::Float32, Rank>(output, *host)));
  else
    CHECK((!runtime.toCpuTyped<NumericType::Float64, Rank>(output, *host)));
  CHECK(!output.live());
  for (unsigned workers : {1u, 2u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    auto up = runtime.upload(*core, group, *host, *runtime.device());
    CHECK(up && core->drain());
    auto device = observe(*core, up);
    CHECK((!runtime.toCpuTyped<Type, Rank>(output, device)) && !output.live());
    auto down = runtime.download(*core, group, device);
    CHECK(down && core->drain());
    auto result = observe(*core, down);
    auto copied = runtime.toCpuTyped<Type, Rank>(output, result);
    CHECK(copied && !std::memcmp(copied->aligned, pattern.data(), pattern.size()));
    CHECK(copied->offset == 0);
    if constexpr (Rank > 0)
      CHECK(copied->sizes == sizes && copied->strides == strides);
    CHECK(core->close(group) && core->drain() && core->release(group));
    output.release(copied->allocated);
    result = {};
    device = {};
    auto stats = core->stats();
    CHECK(!stats.records && !stats.groups && !stats.retainedBytes && !stats.retainedOwners);
  }
  Invocation failed;
  failed.failAllocationAt(1);
  CHECK((!runtime.toCpuTyped<Type, Rank>(failed, *host)) && !failed.live() && failed.status() == 2);
  host.reset();
  empty(runtime);
  if constexpr (Rank > 0) {
    Invocation zero;
    sizes.back() = 0;
    strides.fill(-1);
    CpuTensor<Type, Rank> descriptor{reinterpret_cast<T *>(1), reinterpret_cast<T *>(3), 0, sizes,
                                     strides};
    auto imported = runtime.fromCpu(zero, descriptor.view());
    CHECK(imported);
    auto copied = runtime.toCpuTyped<Type, Rank>(zero, *imported);
    CHECK(copied && copied->sizes == sizes);
    zero.release(copied->allocated);
    imported = {};
    empty(runtime);
  }
}
int main() {
  auto made = m::Runtime::create();
  CHECK(made);
  auto runtime = std::move(*made);
  CHECK(&m::Runtime::scalarLayout() == numericScalarLayout(NumericType::Float32));
  const auto qualify = [&]<NumericType Type>() {
    [&]<std::size_t... Rank>(std::index_sequence<Rank...>) {
      (roundtrip<Type, Rank>(*runtime), ...);
    }(std::make_index_sequence<9>{});
  };
  qualify.template operator()<NumericType::Int8>();
  qualify.template operator()<NumericType::Int16>();
  qualify.template operator()<NumericType::Int32>();
  qualify.template operator()<NumericType::Int64>();
  qualify.template operator()<NumericType::UInt8>();
  qualify.template operator()<NumericType::UInt16>();
  qualify.template operator()<NumericType::UInt32>();
  qualify.template operator()<NumericType::UInt64>();
  qualify.template operator()<NumericType::Float32>();
  qualify.template operator()<NumericType::Float64>();
  Invocation source;
  auto *data = source.allocate(8);
  CHECK(data);
  CpuTensor<NumericType::Float64, 1> descriptor{
      static_cast<double *>(data), static_cast<double *>(data), 0, {1}, {1}};
  auto raw = descriptor.view();
  raw.element = static_cast<NumericType>(255);
  CHECK(!runtime->fromCpu(source, raw));
  source.release(data);
  empty(*runtime);
  // Invalid untagged empty views must not sneak through the empty-tensor path.
  std::array<std::int64_t, 1> size{0}, stride{1};
  CpuTensorView untagged;
  untagged.sizes = size;
  untagged.strides = stride;
  CHECK(!runtime->fromCpu(source, untagged));
  empty(*runtime);
  std::puts("Typed CPU/Metal storage: all ten numeric types/ranks zero through eight "
            "bit-preserving transfers, scalar layout "
            "identity, zero-copy leases, width-aware offsets/budgets and mismatch/invalid "
            "rejection passed");
}
