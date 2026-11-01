#include "Async.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace v = kalandra::runtime::vulkan;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, v::Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %d\n", message, int(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
v::TensorValue observe(TaskCore &core, TaskHandle handle) {
  Invocation context;
  alignas(v::TensorValue) std::byte storage[sizeof(v::TensorValue)];
  check(core.observe(handle, v::AsyncRuntime::tensorLayout(), context, storage),
        "observe transfer");
  auto *value = std::launder(reinterpret_cast<v::TensorValue *>(storage));
  auto result = *value;
  value->~TensorValue();
  return result;
}
v::TensorValue transfer(TaskCore &core, v::AsyncRuntime &async, const v::TensorValue &input,
                        bool upload) {
  auto group = core.openRoot();
  auto task = async.transfer(core, group, input, upload);
  check(bool(task) && core.close(group) && core.drain(), "settle transfer");
  check(core.failure(task).kind == TaskFailure::None, "transfer success");
  auto result = observe(core, task);
  check(core.release(group), "release transfer");
  check(!core.stats().records && !async.stats().pending && !async.stats().captureBytes,
        "transfer metadata retired");
  return result;
}
template <NumericType Type> void descriptor(v::Runtime &runtime) {
  auto info = *describeNumeric(Type);
  const auto fullBytes = 12 * info.bytes();
  v::TensorValue value;
  std::vector<std::byte> expected(3 * info.bytes(), std::byte{0x35});
  {
    Invocation producer;
    auto *allocation = producer.allocate(fullBytes);
    check(allocation, "CPU allocation");
    std::memcpy(static_cast<std::byte *>(allocation) + 2 * info.bytes(), expected.data(),
                expected.size());
    std::array<std::int64_t, 1> shape{3}, strides{1};
    CpuTensorView view{allocation, allocation, 2, shape, strides, Type};
    value = take(v::fromCpu(runtime, producer, view), "retain CPU descriptor");
    check(runtime.retainedBytes() == fullBytes, "charge full retained allocation, not subview");
    strides = {2};
    check(!v::fromCpu(runtime, producer, view), "reject noncontiguous descriptor");
    strides = {1};
    view.allocated = reinterpret_cast<void *>(1);
    check(!v::fromCpu(runtime, producer, view), "reject unowned descriptor");
  }
  check(std::ranges::equal(take(v::hostBytes(value), "retained host bytes"), expected),
        "CPU lease survives producer and honors offset");
  Invocation consumer;
  auto cpu = take(v::toCpuTyped<Type, 1>(consumer, value), "copy into CPU invocation");
  check(cpu.sizes[0] == 3 && cpu.strides[0] == 1 &&
            std::memcmp(cpu.aligned, expected.data(), expected.size()) == 0,
        "typed CPU descriptor exact bytes and canonical strides");
  value = {};
  check(!runtime.retainedBytes(), "retained allocation quota returned");
  check(std::memcmp(cpu.aligned, expected.data(), expected.size()) == 0,
        "CPU export independently owns copied backing");
}
struct Hold {
  v::AsyncRuntime &async;
  explicit Hold(v::AsyncRuntime &value) : async(value) { async.holdCompletions(true); }
  ~Hold() { async.holdCompletions(false); }
};
int run(int argc, char **argv) {
  const bool validation = !(argc > 2 && std::string_view(argv[2]) == "--no-validation");
  std::shared_ptr<v::Runtime> runtime =
      take(v::Runtime::create(
               {.validation = validation,
                .deviceIndex = argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0}),
           "runtime");
  for (unsigned workers : {1u, 2u, 4u}) {
    auto core = TaskCore::create({.workers = workers});
    auto async = v::AsyncRuntime::create(runtime);
    check(core && async, "managed transfer setup");
    for (auto type : numericTypes)
      for (unsigned rank : {0u, 1u, 3u, 8u})
        for (bool empty : {false, true}) {
          if (!rank && empty)
            continue;
          std::array<std::int64_t, 8> shape{1, 1, 1, 1, 1, 1, 1, 1};
          if (rank)
            shape[rank - 1] = empty ? 0 : 7;
          std::vector<std::byte> bytes((empty ? 0 : rank ? 7 : 1) * type.bytes());
          for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] =
                std::byte((i * 73 + 91) % 256); // All bit patterns: transfer is not arithmetic.
          auto host = take(v::hostTensor(*runtime, type.type, bytes, std::span(shape).first(rank)),
                           "owned snapshot");
          auto original = bytes;
          std::fill(bytes.begin(), bytes.end(), std::byte{});
          auto gpu = transfer(*core, *async, host, true);
          check(gpu.data && !gpu.host && gpu.rank == rank && gpu.dimensions == host.dimensions,
                "upload residency/shape");
          auto noImplicitRead = v::hostBytes(gpu);
          check(!noImplicitRead && noImplicitRead.error() == v::Error::Residency,
                "explicit download required");
          host = {};
          auto result = transfer(*core, *async, gpu, false);
          gpu = {};
          check(std::ranges::equal(take(v::hostBytes(result), "download bytes"), original),
                "all-bit-pattern transfer reference");
          auto forged = result;
          forged.type =
              type.type == NumericType::Float32 ? NumericType::UInt32 : NumericType::Float32;
          check(!v::hostBytes(forged), "host metadata cannot retype backing");
        }
    check(!runtime->retainedBytes(), "all transfer backing retired");
    for (bool uploading : {false, true})
      for (bool deadline : {false, true}) {
        auto host = take(v::hostTensor(*runtime, NumericType::UInt8, std::array{std::byte{7}},
                                       std::array<std::int64_t, 1>{1}),
                         "cancel snapshot");
        if (!uploading)
          host = transfer(*core, *async, host, true);
        auto group = core->openRoot();
        Hold hold(*async);
        auto task = async->transfer(*core, group, host, uploading);
        host = {};
        check(bool(task) && core->close(group), "held transfer");
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!async->stats().held && std::chrono::steady_clock::now() < end) {
          core->step();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(async->stats().held == 1, "transfer reaches completion gate");
        const auto retained = runtime->retainedBytes();
        check(core->cancel(group, deadline) && !core->settled(group) && retained &&
                  core->stats().externalPending == 1,
              "cancel retains external transfer resources until acknowledgement");
        async->holdCompletions(false);
        check(core->drain() &&
                  core->failure(group).kind ==
                      (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
              "transfer cancellation/deadline precedence");
        check(core->release(group) && !runtime->retainedBytes(),
              "cancelled transfer retires backing");
      }
  }
  descriptor<NumericType::Int8>(*runtime);
  descriptor<NumericType::UInt8>(*runtime);
  descriptor<NumericType::Int16>(*runtime);
  descriptor<NumericType::UInt16>(*runtime);
  descriptor<NumericType::Int32>(*runtime);
  descriptor<NumericType::UInt32>(*runtime);
  descriptor<NumericType::Int64>(*runtime);
  descriptor<NumericType::UInt64>(*runtime);
  descriptor<NumericType::Float32>(*runtime);
  descriptor<NumericType::Float64>(*runtime);
  auto small = take(v::Runtime::create(
                        {.validation = validation,
                         .deviceIndex = argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0,
                         .maxBytes = 3}),
                    "small quota runtime");
  {
    auto a = take(v::hostTensor(*small, NumericType::UInt8, std::array{std::byte{1}, std::byte{2}},
                                std::array<std::int64_t, 1>{2}),
                  "small snapshot");
    auto over = v::hostTensor(*small, NumericType::UInt8, std::array{std::byte{1}, std::byte{2}},
                              std::array<std::int64_t, 1>{2});
    check(!over && over.error() == v::Error::Budget && small->retainedBytes() == 2,
          "aggregate host quota/rollback");
    check(!v::validateTransfer(*runtime, a, true), "foreign registry host snapshot rejected");
    auto failed = v::transferTensor(*small, a, true);
    check(!failed && failed.error() == v::Error::Budget && small->retainedBytes() == 2,
          "host plus device share one quota");
  }
  check(!small->retainedBytes() && !runtime->retainedBytes() && !runtime->validationErrors() &&
            !small->validationErrors(),
        "final ownership/validation");
  {
    auto budget = take(v::Runtime::createByteBudget(8), "registry budget");
    const auto selected = argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0;
    auto first = take(
        v::Runtime::create({.validation = validation, .deviceIndex = selected, .budget = budget}),
        "first registry context");
    auto second = take(
        v::Runtime::create({.validation = validation, .deviceIndex = selected, .budget = budget}),
        "second registry context");
    auto host =
        take(v::hostTensor(*first, NumericType::UInt8, std::array{std::byte{1}, std::byte{2}},
                           std::array<std::int64_t, 1>{2}),
             "registry host value");
    check(bool(v::validateTransfer(*second, host, true)),
          "same-registry host values can target another device context");
    auto reserved = take(second->reserveHostBytes(6), "shared quota remainder");
    auto over = first->reserveHostBytes(1);
    check(!over && over.error() == v::Error::Budget && first->retainedBytes() == 8 &&
              second->retainedBytes() == 8,
          "device contexts share aggregate quota");
    first.reset();
    second.reset();
    check(take(v::hostBytes(host), "escaped registry host")[0] == std::byte{1},
          "host value survives device context teardown");
  }
  std::printf("PASS: typed CPU descriptors, managed explicit transfers, cancellation and combined "
              "host/device quotas on %s\n",
              runtime->device().name.c_str());
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
