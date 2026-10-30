#include "Metal.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Typed numeric assertion %d: %s\n", __LINE__, #x);                      \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
m::Buffer upload(m::Runtime &runtime, NumericType type, std::span<const std::uint64_t> bits,
                 unsigned rank) {
  const auto width = describeNumeric(type)->bytes();
  std::vector<std::byte> data(bits.size() * width);
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(data.data() + i * width, &bits[i], width);
  std::vector<std::int64_t> shape(rank, 1);
  if (rank)
    shape.back() = bits.size();
  auto host = runtime.hostTensor(data, shape, *m::storageFormat(type));
  CHECK(host);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto task = runtime.upload(*core, group, *host, *runtime.device());
  CHECK(task && core->drain());
  auto result = observe(*core, task);
  CHECK(core->close(group) && core->drain() && core->release(group));
  return result;
}
std::uint64_t reference(NumericType type, std::uint64_t a, std::uint64_t b, std::uint64_t scalar) {
  const auto width = describeNumeric(type)->bits;
  if (isInteger(type)) {
    const auto mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
    return (a + b + scalar) & mask;
  }
  const auto &semantics =
      type == NumericType::Float32 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
  llvm::APFloat x(semantics, llvm::APInt(width, a)), y(semantics, llvm::APInt(width, b)),
      z(semantics, llvm::APInt(width, scalar));
  x.add(y, llvm::APFloat::rmNearestTiesToEven);
  x.add(z, llvm::APFloat::rmNearestTiesToEven);
  if (x.isNaN())
    return width == 32 ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
  return x.bitcastToAPInt().getZExtValue();
}
std::uint64_t execute(m::Runtime &runtime, m::Kernel kernel, m::Buffer a, m::Buffer b,
                      std::uint64_t scalar, unsigned workers, int fault,
                      std::span<const std::uint64_t> expected, std::uint64_t failAt = 0) {
  const auto baseline = runtime.stats();
  std::uint64_t attempts = 0;
  {
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    runtime.failAllocationAt(failAt);
    std::array inputs{a, b};
    std::array scalars{scalar};
    auto task = runtime.numericElementwise(*core, group, kernel, inputs, scalars);
    inputs = {};
    scalars = {};
    CHECK(task && core->drain());
    attempts = runtime.stats().attempts;
    runtime.failAllocationAt(0);
    auto failure = core->failure(task);
    if (fault) {
      CHECK(failure.kind == TaskFailure::Fault && failure.code == fault);
    } else {
      CHECK(failure.kind == TaskFailure::None);
      auto result = observe(*core, task), cached = observe(*core, task);
      CHECK(result.onDevice() && result.format() == a.format() && result.bytes() == a.bytes());
      CHECK(std::equal(result.shape().begin(), result.shape().end(), a.shape().begin(),
                       a.shape().end()));
      auto down = runtime.download(*core, group, cached);
      CHECK(down && core->drain());
      auto host = observe(*core, down);
      auto bytes = host.hostBytes();
      CHECK(bytes);
      const auto width = describeNumeric(*m::storageNumericType(host.format()))->bytes();
      CHECK(bytes->size() == expected.size() * width);
      for (std::size_t i = 0; i < expected.size(); ++i) {
        std::uint64_t actual = 0;
        std::memcpy(&actual, bytes->data() + i * width, width);
        CHECK(actual == expected[i]);
      }
    }
    CHECK(core->close(group) && core->drain() && core->release(group));
    auto state = core->stats();
    CHECK(!state.records && !state.groups && !state.retainedBytes && !state.externalPending);
  }
  auto now = runtime.stats();
  CHECK(now.bytes == baseline.bytes && now.hostBackings == baseline.hostBackings &&
        now.deviceBackings == baseline.deviceBackings &&
        now.metalBuffers == baseline.metalBuffers && !now.inFlight && !now.stagingBuffers &&
        !now.argumentBytes && !now.argumentPacks);
  return attempts;
}
std::uint64_t executeScalar(m::Runtime &runtime, m::Kernel kernel, NumericType type,
                            std::uint64_t scalar, std::uint64_t expected, unsigned workers,
                            int fault = 0, std::uint64_t failAt = 0) {
  const auto baseline = runtime.stats();
  std::uint64_t attempts = 0;
  {
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    runtime.failAllocationAt(failAt);
    if (fault == std::int64_t(m::Error::Device))
      runtime.failNextCompletion();
    std::array arguments{scalar};
    auto task = runtime.numericElementwise(*core, group, kernel, {}, arguments, {}, true);
    arguments = {};
    CHECK(task && core->drain());
    attempts = runtime.stats().attempts;
    runtime.failAllocationAt(0);
    const auto failure = core->failure(task);
    if (fault)
      CHECK(failure.kind == TaskFailure::Fault && failure.code == fault);
    else {
      CHECK(failure.kind == TaskFailure::None);
      for (unsigned read = 0; read < 2; ++read) {
        Invocation context;
        alignas(8) std::byte slot[8]{};
        CHECK(core->observe(task, *numericScalarLayout(type), context, slot));
        std::uint64_t bits = 0;
        std::memcpy(&bits, slot, describeNumeric(type)->bytes());
        CHECK(bits == expected);
      }
    }
    CHECK(core->close(group) && core->drain() && core->release(group));
    const auto stats = core->stats();
    CHECK(!stats.records && !stats.groups && !stats.retainedBytes && !stats.externalPending);
  }
  const auto stats = runtime.stats();
  CHECK(stats.bytes == baseline.bytes && stats.hostBackings == baseline.hostBackings &&
        stats.deviceBackings == baseline.deviceBackings &&
        stats.metalBuffers == baseline.metalBuffers && !stats.inFlight && !stats.stagingBuffers &&
        !stats.argumentBytes && !stats.argumentPacks);
  return attempts;
}
std::uint64_t executeProgram(m::Runtime &runtime, m::Kernel kernel, NumericType type, m::Buffer a,
                             m::Buffer b, m::Buffer predicate, std::uint64_t scalar,
                             std::uint64_t expected, unsigned workers, unsigned mode = 0,
                             int fault = 0, std::uint64_t failAt = 0) {
  const auto baseline = runtime.stats();
  std::uint64_t attempts = 0;
  {
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    runtime.failAllocationAt(failAt);
    std::array inputs{a, b, predicate};
    std::array<std::uint32_t, 2> first{0, 1}, second{3, 1};
    std::array<std::uint32_t, 1> condition{2}, other{1};
    std::array captures{scalar};
    using K = m::NumericInstructionKind;
    std::vector<m::NumericInstruction> code;
    std::uint32_t output = 4;
    if (!mode || mode >= 3)
      code = {{K::Map, kernel, first, captures, 3}, {K::Map, kernel, second, captures, 4}};
    else {
      output = 3;
      if (mode == 1)
        code = {{K::Branch, {}, condition, {}, UINT32_MAX, 3},
                {K::Map, kernel, first, captures, 3},
                {K::Jump, {}, {}, {}, UINT32_MAX, 4},
                {K::Alias, {}, other, {}, 3}};
      else
        code = {{K::Branch, {}, condition, {}, UINT32_MAX, 4},
                {K::EqualShape, {}, first},
                {K::Map, kernel, first, captures, 3},
                {K::Jump, {}, {}, {}, UINT32_MAX, 5},
                {K::Alias, {}, other, {}, 3}};
    }
    if (mode == 3)
      first[0] = 99;
    if (mode == 4)
      code[0].scalars = {};
    if (mode == 5)
      code[0].kind = K::Reduce;
    if (mode == 6) {
      code[0].kind = K::Jump;
      code[0].inputs = {};
      code[0].scalars = {};
      code[0].output = UINT32_MAX;
      code[0].target = 0;
    }
    auto task = runtime.numericProgram(*core, group, inputs, code, output, type, true);
    inputs = {};
    first = {99, 99};
    second = {99, 99};
    condition = {99};
    other = {99};
    captures = {};
    code.clear();
    CHECK(task && core->drain());
    attempts = runtime.stats().attempts;
    runtime.failAllocationAt(0);
    auto failure = core->failure(task);
    if (fault)
      CHECK(failure.kind == TaskFailure::Fault && failure.code == fault);
    else {
      CHECK(failure.kind == TaskFailure::None);
      Invocation context;
      alignas(8) std::byte slot[8]{};
      CHECK(core->observe(task, *numericScalarLayout(type), context, slot));
      std::uint64_t bits = 0;
      std::memcpy(&bits, slot, describeNumeric(type)->bytes());
      CHECK(bits == expected);
    }
    CHECK(core->close(group) && core->drain() && core->release(group));
    const auto state = core->stats();
    CHECK(!state.records && !state.groups && !state.retainedBytes && !state.externalPending);
  }
  const auto now = runtime.stats();
  CHECK(now.bytes == baseline.bytes && now.hostBackings == baseline.hostBackings &&
        now.deviceBackings == baseline.deviceBackings &&
        now.metalBuffers == baseline.metalBuffers && !now.inFlight && !now.stagingBuffers &&
        !now.argumentBytes && !now.argumentPacks);
  return attempts;
}
void cancelProgram(m::Runtime &runtime, m::Kernel kernel, NumericType type, m::Buffer left,
                   m::Buffer right, m::Buffer predicate, std::uint64_t scalar, bool deadline) {
  const auto baseline = runtime.stats();
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    std::array inputs{left, right, predicate};
    std::array<std::uint32_t, 2> a{0, 1}, b{3, 1};
    std::array captures{scalar};
    std::array<m::NumericInstruction, 2> code{
        {{m::NumericInstructionKind::Map, kernel, a, captures, 3},
         {m::NumericInstructionKind::Map, kernel, b, captures, 4}}};
    runtime.holdCompletions(true);
    auto task = runtime.numericProgram(*core, group, inputs, code, 4, type, true);
    CHECK(task && core->step());
    CHECK(core->stats().externalPending == 1 && runtime.stats().inFlight == 1);
    CHECK(core->cancel(group, deadline) && !core->drain(0) && !core->settled(task));
    runtime.holdCompletions(false);
    CHECK(core->drain());
    CHECK(core->failure(group).kind ==
          (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    Invocation context;
    alignas(8) std::byte output[8]{};
    CHECK(!core->observe(task, *numericScalarLayout(type), context, output));
    CHECK(core->close(group) && core->drain() && core->release(group));
    const auto stats = core->stats();
    CHECK(!stats.records && !stats.groups && !stats.retainedBytes && !stats.externalPending);
  }
  const auto stats = runtime.stats();
  CHECK(stats.bytes == baseline.bytes && stats.metalBuffers == baseline.metalBuffers &&
        stats.deviceBackings == baseline.deviceBackings && !stats.argumentBytes &&
        !stats.argumentPacks && !stats.inFlight && !stats.stagingBuffers);
}
std::uint64_t executeResidentReduction(m::Runtime &runtime, m::Kernel kernel, NumericType type,
                                       m::Buffer input, std::uint64_t expected, unsigned workers,
                                       int fault = 0, std::uint64_t failAt = 0, unsigned mode = 0) {
  const auto baseline = runtime.stats();
  std::uint64_t attempts = 0;
  {
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    std::array inputs{input};
    std::array<std::uint32_t, 1> first{0}, second{1};
    std::array<m::NumericInstruction, 2> code{
        {{m::NumericInstructionKind::Reduce, kernel, first, {}, 1},
         {m::NumericInstructionKind::Reduce, kernel, second, {}, 2}}};
    runtime.failAllocationAt(failAt);
    if (mode == 3)
      runtime.failNextCompletion();
    if (mode == 1 || mode == 2)
      runtime.holdCompletions(true);
    auto task = runtime.numericProgram(*core, group, inputs, code, 2, type, true);
    inputs = {};
    first = second = {99};
    code = {};
    CHECK(task);
    if (mode == 1 || mode == 2) {
      CHECK(core->step() && core->stats().externalPending == 1);
      CHECK(core->cancel(group, mode == 2) && !core->drain(0) && !core->settled(task));
      runtime.holdCompletions(false);
    }
    CHECK(core->drain());
    attempts = runtime.stats().attempts;
    runtime.failAllocationAt(0);
    if (mode == 1 || mode == 2) {
      CHECK(core->failure(group).kind ==
            (mode == 2 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
      Invocation context;
      alignas(8) std::byte slot[8]{};
      CHECK(!core->observe(task, *numericScalarLayout(type), context, slot));
    } else if (fault) {
      CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == fault);
    } else {
      CHECK(core->failure(task).kind == TaskFailure::None);
      Invocation context;
      alignas(8) std::byte slot[8]{};
      CHECK(core->observe(task, *numericScalarLayout(type), context, slot));
      std::uint64_t bits = 0;
      std::memcpy(&bits, slot, describeNumeric(type)->bytes());
      CHECK(bits == expected);
    }
    CHECK(core->close(group) && core->drain() && core->release(group));
    auto state = core->stats();
    CHECK(!state.records && !state.groups && !state.retainedBytes && !state.externalPending);
  }
  const auto now = runtime.stats();
  CHECK(now.bytes == baseline.bytes && now.hostBackings == baseline.hostBackings &&
        now.deviceBackings == baseline.deviceBackings &&
        now.metalBuffers == baseline.metalBuffers && !now.inFlight && !now.stagingBuffers &&
        !now.argumentBytes && !now.argumentPacks);
  return attempts;
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  auto made = m::Runtime::create();
  CHECK(made);
  auto runtime = std::move(*made);
  std::uint64_t programs = 0, failures = 0;
  for (const auto description : numericTypes) {
    const auto type = description.type;
    auto kernel = runtime->loadNumericKernel(std::string(argv[1]) + "/numeric-" +
                                                 std::string(description.name) + ".metallib",
                                             *runtime->device(), type, {2, 1});
    if (!kernel)
      std::fprintf(stderr, "numeric kernel %s failed: %lld\n", description.name.data(),
                   (long long)kernel.error());
    CHECK(kernel);
    for (const auto other : numericTypes)
      if (other.type != type && other.bits == description.bits) {
        CHECK(!runtime->loadNumericKernel(std::string(argv[1]) + "/numeric-" +
                                              std::string(description.name) + ".metallib",
                                          *runtime->device(), other.type, {2, 1}));
        break;
      }
    CHECK(!runtime->loadNumericKernel(std::string(argv[1]) + "/numeric-" +
                                          std::string(description.name) + ".metallib",
                                      *runtime->device(), type, {2, 2}));
    {
      const auto path =
          std::string(argv[1]) + "/numeric-" + std::string(description.name) + ".metallib";
      std::array tensorTypes{type, type};
      std::array scalarTypes{type};
      CHECK(runtime->loadNumericKernel(path, *runtime->device(), type, {2, 1}, tensorTypes,
                                       scalarTypes));
      for (const auto &other : numericTypes) {
        if (other.type == type || other.bits != description.bits)
          continue;
        tensorTypes[0] = other.type;
        CHECK(!runtime->loadNumericKernel(path, *runtime->device(), type, {2, 1}, tensorTypes,
                                          scalarTypes));
        tensorTypes[0] = type;
        scalarTypes[0] = other.type;
        CHECK(!runtime->loadNumericKernel(path, *runtime->device(), type, {2, 1}, tensorTypes,
                                          scalarTypes));
        scalarTypes[0] = type;
      }
    }
    const std::uint64_t scalar = isInteger(type)                ? 1
                                 : type == NumericType::Float32 ? UINT64_C(0x3f800000)
                                                                : UINT64_C(0x3ff0000000000000);
    for (unsigned rank : {0u, 3u, 8u})
      for (unsigned count : {0u, 1u, 3u, 65u, 257u}) {
        if (!rank && count != 1)
          continue;
        std::vector<std::uint64_t> a(count), b(count), expected(count);
        for (unsigned i = 0; i < count; ++i) {
          if (isInteger(type)) {
            a[i] = i % 7;
            b[i] = (i + 2) % 7;
          } else if (type == NumericType::Float32) {
            constexpr std::uint64_t edges[] = {0,          0x80000000, 1,
                                               0x007fffff, 0x3f800000, 0xbf800000,
                                               0x7f800000, 0xff800000, 0x7fc12345};
            a[i] = edges[i % 9];
            b[i] = edges[(i + 3) % 9];
          } else {
            constexpr std::uint64_t edges[] = {0,
                                               UINT64_C(0x8000000000000000),
                                               1,
                                               UINT64_C(0x000fffffffffffff),
                                               UINT64_C(0x3ff0000000000000),
                                               UINT64_C(0xbff0000000000000),
                                               UINT64_C(0x7ff0000000000000),
                                               UINT64_C(0xfff0000000000000),
                                               UINT64_C(0x7ff8123456789abc)};
            a[i] = edges[i % 9];
            b[i] = edges[(i + 3) % 9];
          }
          expected[i] = reference(type, a[i], b[i], scalar);
        }
        auto left = upload(*runtime, type, a, rank), right = upload(*runtime, type, b, rank);
        for (unsigned workers : {1u, 2u, 4u}) {
          execute(*runtime, *kernel, left, right, scalar, workers, 0, expected);
          ++programs;
        }
        if (rank == 3 && count == 3) {
          auto checkpoints = execute(*runtime, *kernel, left, right, scalar, 1, 0, expected);
          for (std::uint64_t fail = 1; fail <= checkpoints; ++fail) {
            execute(*runtime, *kernel, left, right, scalar, 1, 2, {}, fail);
            ++failures;
          }
        }
      }
    if (isInteger(type)) {
      std::array a{integerMaximum(type)}, b{UINT64_C(1)};
      auto left = upload(*runtime, type, a, 3), right = upload(*runtime, type, b, 3);
      for (unsigned workers : {1u, 2u, 4u}) {
        execute(*runtime, *kernel, left, right, 0, workers, 4, {});
        ++failures;
      }
    }
    auto scalarKernel = runtime->loadNumericKernel(std::string(argv[1]) + "/numeric-scalar-" +
                                                       std::string(description.name) + ".metallib",
                                                   *runtime->device(), type, {0, 1});
    CHECK(scalarKernel);
    const auto one = isInteger(type)                ? UINT64_C(1)
                     : type == NumericType::Float32 ? UINT64_C(0x3f800000)
                                                    : UINT64_C(0x3ff0000000000000);
    const auto expectedScalar = reference(type, one, 0, scalar);
    for (unsigned workers : {1u, 2u, 4u}) {
      executeScalar(*runtime, *scalarKernel, type, scalar, expectedScalar, workers);
      ++programs;
      executeScalar(*runtime, *scalarKernel, type, scalar, 0, workers,
                    std::int64_t(m::Error::Device));
      ++failures;
      if (isInteger(type)) {
        executeScalar(*runtime, *scalarKernel, type, integerMaximum(type), 0, workers, 4);
        ++failures;
      }
    }
    const auto checkpoints =
        executeScalar(*runtime, *scalarKernel, type, scalar, expectedScalar, 1);
    for (std::uint64_t fail = 1; fail <= checkpoints; ++fail) {
      executeScalar(*runtime, *scalarKernel, type, scalar, 0, 1, 2, fail);
      ++failures;
    }
    {
      const auto two = isInteger(type)                ? UINT64_C(2)
                       : type == NumericType::Float32 ? UINT64_C(0x40000000)
                                                      : UINT64_C(0x4000000000000000);
      std::array leftBits{one}, rightBits{two}, trueBits{UINT64_C(1)}, falseBits{UINT64_C(0)};
      auto left = upload(*runtime, type, leftBits, 0), right = upload(*runtime, type, rightBits, 0);
      auto yes = upload(*runtime, NumericType::UInt8, trueBits, 0),
           no = upload(*runtime, NumericType::UInt8, falseBits, 0);
      const auto first = reference(type, one, two, one), last = reference(type, first, two, one);
      for (unsigned workers : {1u, 2u, 4u}) {
        executeProgram(*runtime, *kernel, type, left, right, yes, one, last, workers);
        ++programs;
        executeProgram(*runtime, *kernel, type, left, right, yes, one, first, workers, 1);
        ++programs;
        executeProgram(*runtime, *kernel, type, left, right, no, one, two, workers, 1);
        ++programs;
      }
      const auto checkpoints =
          executeProgram(*runtime, *kernel, type, left, right, yes, one, last, 1);
      for (bool deadline : {false, true}) {
        cancelProgram(*runtime, *kernel, type, left, right, yes, one, deadline);
        ++failures;
      }
      for (std::uint64_t fail = 1; fail <= checkpoints; ++fail) {
        executeProgram(*runtime, *kernel, type, left, right, yes, one, 0, 1, 0, 2, fail);
        ++failures;
      }
      for (unsigned malformed : {3u, 4u, 5u, 6u}) {
        executeProgram(*runtime, *kernel, type, left, right, yes, one, 0, 1, malformed, 3);
        ++failures;
      }
      auto different = upload(*runtime, type, leftBits, 3);
      executeProgram(*runtime, *kernel, type, different, right, no, one, two, 1, 2);
      ++programs;
      executeProgram(*runtime, *kernel, type, different, right, yes, one, 0, 1, 2, 1);
      ++failures;
      if (isInteger(type)) {
        std::array maxBits{integerMaximum(type)};
        auto maximum = upload(*runtime, type, maxBits, 0);
        executeProgram(*runtime, *kernel, type, maximum, right, no, 0, two, 1, 1);
        ++programs;
        executeProgram(*runtime, *kernel, type, maximum, right, yes, 0, 0, 1, 1, 4);
        ++failures;
      }
    }
    {
      auto reduction = runtime->loadNumericReductionKernel(
          std::string(argv[1]) + "/numeric-reduce-" + std::string(description.name) + ".metallib",
          *runtime->device(), type);
      CHECK(reduction);
      const auto one = isInteger(type)                ? UINT64_C(1)
                       : type == NumericType::Float32 ? UINT64_C(0x3f800000)
                                                      : UINT64_C(0x3ff0000000000000);
      for (unsigned count : {0u, 1u, 3u, 65u}) {
        std::vector<std::uint64_t> values(count, one);
        auto input = upload(*runtime, type, values, 8);
        std::uint64_t expected = 0;
        for (unsigned i = 0; i < count; ++i)
          expected = reference(type, expected, one, 0);
        for (unsigned workers : {1u, 2u, 4u}) {
          executeResidentReduction(*runtime, *reduction, type, input, expected, workers);
          ++programs;
        }
        const auto checkpoints =
            executeResidentReduction(*runtime, *reduction, type, input, expected, 1);
        for (std::uint64_t fail = 1; fail <= checkpoints; ++fail) {
          executeResidentReduction(*runtime, *reduction, type, input, 0, 1, 2, fail);
          ++failures;
        }
        if (count == 65) {
          for (unsigned mode : {1u, 2u, 3u}) {
            executeResidentReduction(*runtime, *reduction, type, input, 0, 1,
                                     mode == 3 ? std::int64_t(m::Error::Device) : 0, 0, mode);
            ++failures;
          }
        }
      }
      if (isInteger(type)) {
        const auto maximum =
            description.bits == 64
                ? (!isUnsigned(type) ? UINT64_C(0x7fffffffffffffff) : UINT64_MAX)
                : (UINT64_C(1) << (description.bits - (!isUnsigned(type) ? 1 : 0))) - 1;
        std::array values{maximum, UINT64_C(1), UINT64_C(0)};
        auto input = upload(*runtime, type, values, 8);
        executeResidentReduction(*runtime, *reduction, type, input, 0, 1, 4);
        ++failures;
      }
    }
    auto stats = runtime->stats();
    CHECK(!stats.bytes && !stats.hostBackings && !stats.deviceBackings && !stats.metalBuffers &&
          !stats.inFlight && !stats.argumentBytes);
  }
  for (bool arguments : {false, true}) {
    m::Options options;
    if (arguments)
      options.maxArgumentBytes = 256;
    else
      options.maxBytes = 48;
    auto limited = m::Runtime::create(options);
    CHECK(limited);
    const auto type = NumericType::Float64;
    auto kernel = (*limited)->loadNumericKernel(std::string(argv[1]) + "/numeric-float64.metallib",
                                                *(*limited)->device(), type, {2, 1});
    CHECK(kernel);
    std::array a{UINT64_C(0x3ff0000000000000)}, b{UINT64_C(0x4000000000000000)},
        condition{UINT64_C(1)};
    auto left = upload(**limited, type, a, 0), right = upload(**limited, type, b, 0);
    auto predicate = upload(**limited, NumericType::UInt8, condition, 0);
    executeProgram(**limited, *kernel, type, left, right, predicate, a[0], 0, 1, 0,
                   std::int64_t(m::Error::Budget));
    ++failures;
  }
  std::printf("Typed numeric tasks: ten types, ranks 0/3/8, %llu successful calls, %llu "
              "allocation/arithmetic/device failures, tensor/scalar tasks, owned captures and "
              "cleanup passed\n",
              (unsigned long long)programs, (unsigned long long)failures);
}
