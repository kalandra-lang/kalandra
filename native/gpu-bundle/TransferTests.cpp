#include "Language.h"
#include <array>
#include <barrier>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace kalandra;
using namespace kalandra::runtime;
namespace b = kalandra::runtime::bundle;

void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, b::Error> result) {
  if (!result)
    throw std::runtime_error("GPU error " + std::to_string(int(result.error())));
  return std::move(*result);
}
b::Buffer observe(TaskCore &core, GroupHandle group, TaskHandle child) {
  check(child && core.close(group) && core.drain(), "task completion");
  Invocation context;
  alignas(b::Buffer) std::byte slot[sizeof(b::Buffer)];
  check(core.observe(child, b::Runtime::bufferLayout(), context, slot), "tagged result layout");
  auto *value = std::launder(reinterpret_cast<b::Buffer *>(slot));
  auto result = std::move(*value);
  std::destroy_at(value);
  check(core.release(group), "group release");
  return result;
}
int main(int argc, char **argv) {
  try {
    auto runtime = take(b::Runtime::create());
    check(runtime->stats().initializedDevices == 0, "early GPU discovery");
    const bool unavailable = argc == 2 && std::string_view(argv[1]) == "--unavailable";
    std::atomic<bool> passed = true;
    std::barrier firstUse(12);
    std::vector<std::jthread> workers;
    for (int i = 0; i < 12; ++i)
      workers.emplace_back([&] {
        firstUse.arrive_and_wait();
        for (int repeat = 0; repeat < 32; ++repeat)
          if (bool(runtime->device()) == unavailable)
            passed = false;
      });
    workers.clear();
    check(passed, "concurrent first-use result");
    if (unavailable) {
      check(runtime->stats().initializedDevices == 0, "negative discovery");
      return 0;
    }
    auto device = take(runtime->device());
    auto core = TaskCore::create({.workers = 4});
    check(bool(core), "task core");
    Invocation context;
    std::array<std::int32_t, 4> data{-2, 0, 37, 2147483647};
    auto cpu = copyCpuTensor<NumericType::Int32, 1>(context, NumericType::Int32,
                                                    std::as_bytes(std::span(data)),
                                                    std::array<std::int64_t, 1>{4});
    check(bool(cpu), "allocate tensor");
    auto host = take(runtime->fromCpu(context, cpu->view()));
    auto g = core->openRoot();
    auto gpu = observe(*core, g, runtime->upload(*core, g, host, device));
    check(gpu.onDevice(), "resident tensor");
    auto back = core->openRoot();
    auto result = observe(*core, back, runtime->download(*core, back, gpu));
    auto tensor = take(runtime->toCpuTyped<NumericType::Int32, 1>(context, result));
    check(std::equal(data.begin(), data.end(), tensor.aligned), "tensor round trip");
    check(runtime->stats().initializedDevices == 1, "cached context");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
