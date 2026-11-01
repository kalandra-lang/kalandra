#include "Async.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using namespace kalandra::runtime;
namespace v = kalandra::runtime::vulkan;
namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, v::Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: error %lld\n", message, static_cast<long long>(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
bool poisons(v::FailurePoint point) {
  return point == v::FailurePoint::QueueSubmit || point == v::FailurePoint::FenceWait;
}
v::Error expected(v::FailurePoint point) {
  return point == v::FailurePoint::FenceWait ? v::Error::Device : v::Error::Allocation;
}
struct HoldGate {
  v::AsyncRuntime &runtime;
  explicit HoldGate(v::AsyncRuntime &value) : runtime(value) { runtime.holdCompletions(true); }
  ~HoldGate() { runtime.holdCompletions(false); }
};
void raw(unsigned device, bool validation, const char *kernelPath) {
  for (auto point : {v::FailurePoint::BufferCreate, v::FailurePoint::MemoryAllocate,
                    v::FailurePoint::MemoryBind, v::FailurePoint::MemoryMap,
                    v::FailurePoint::PipelineLayout, v::FailurePoint::PipelineCreate,
                    v::FailurePoint::DescriptorAllocate, v::FailurePoint::CommandAllocate,
                    v::FailurePoint::CommandBegin, v::FailurePoint::CommandEnd,
                    v::FailurePoint::FenceCreate, v::FailurePoint::QueueSubmit,
                    v::FailurePoint::FenceWait}) {
    auto driver = take(v::Runtime::create({.validation = validation, .deviceIndex = device}), "driver");
    {
      auto input = take(driver->upload(std::array{2.0f}), "input");
      const auto baseline = driver->retainedBytes();
      driver->injectFailure(point);
      auto failure = driver->add(input, input);
      check(!failure && failure.error() == expected(point), "injected raw failure mapping");
      check(driver->retainedBytes() == baseline, "partial allocation rollback");
      auto retry = driver->add(input, input);
      if (poisons(point))
        check(!retry && retry.error() == v::Error::Device, "failed queue remains poisoned");
      else {
        check(bool(retry), "recoverable failure retries");
        check(take(driver->download(*retry), "retry output") == std::vector<float>{4},
              "retry exact output");
        check(take(driver->download(input), "input immutable") == std::vector<float>{2},
              "failed operation preserves input");
      }
    }
    check(!driver->retainedBytes() && !driver->validationErrors(), "raw failure ownership/validation");
  }
  // Also exercise generic compiler-owned kernel pipeline rollback, not only
  // the lazy legacy primitive slots.
  for (auto point : {v::FailurePoint::PipelineLayout, v::FailurePoint::PipelineCreate}) {
    auto driver = take(v::Runtime::create({.validation = validation, .deviceIndex = device}), "generic driver");
    driver->injectFailure(point);
    auto failure = driver->loadKernel(kernelPath);
    check(!failure && failure.error() == v::Error::Allocation, "generic partial pipeline failure");
    check(bool(driver->loadKernel(kernelPath)), "generic pipeline retry");
    check(!driver->retainedBytes() && !driver->validationErrors(), "generic rollback validation");
  }
}
void managed(unsigned device, bool validation, const char *kernelPath) {
  for (auto point : {v::FailurePoint::MemoryAllocate, v::FailurePoint::DescriptorAllocate,
                    v::FailurePoint::QueueSubmit, v::FailurePoint::FenceWait})
    for (unsigned workers : {1u, 2u, 4u})
      for (unsigned cancellation : {0u, 1u, 2u}) {
        std::shared_ptr<v::Runtime> driver =
            take(v::Runtime::create({.validation = validation, .deviceIndex = device}), "managed driver");
        auto kernel = take(driver->loadKernel(kernelPath), "managed kernel");
        auto async = v::AsyncRuntime::create(driver);
        auto core = TaskCore::create({.workers = workers});
        check(bool(async) && bool(core), "managed services");
        HoldGate gate(*async);
        auto group = core->openRoot();
        TaskHandle task;
        {
          auto left = take(driver->upload(std::array{1.0f, 2.0f, 3.0f}), "managed left");
          auto right = take(driver->upload(std::array{4.0f, 5.0f, 6.0f}), "managed right");
          std::array inputs{left, right};
          std::array parameters{3u, UINT32_MAX};
          driver->injectFailure(point);
          task = async->execute(*core, group, kernel, inputs, parameters, 12, 1, 1);
        }
        check(bool(task) && core->close(group), "submit managed failure");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!async->stats().held && std::chrono::steady_clock::now() < deadline) {
          core->step();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Always release the test gate before throwing/destructing services.
        const bool held = async->stats().held == 1;
        const auto retained = driver->retainedBytes();
        const bool pending = core->stats().externalPending == 1 && !core->settled(group);
        const bool cancelled = !cancellation || core->cancel(group, cancellation == 2);
        const bool pinned = driver->retainedBytes() == retained && retained > 0;
        async->holdCompletions(false);
        check(held && pending && cancelled && pinned, "failure/cancellation retains acknowledgement ownership");
        check(core->drain(), "failure drain");
        const auto failure = core->failure(task);
        // TaskCore records the deadline on the group and cancellation on its
        // children. Preserve that existing distinction in the GPU adapter.
        const auto kind = cancellation ? TaskFailure::Cancelled : TaskFailure::Fault;
        const auto groupKind = cancellation == 2 ? TaskFailure::DeadlineExceeded : kind;
        if (failure.kind != kind)
          std::fprintf(stderr, "point=%u workers=%u cancellation=%u task=%u/%lld group=%u/%lld expected=%u\n",
                       unsigned(point), workers, cancellation, unsigned(failure.kind),
                       static_cast<long long>(failure.code), unsigned(core->failure(group).kind),
                       static_cast<long long>(core->failure(group).code), unsigned(kind));
        check(failure.kind == kind, "cancellation/deadline wins pending raw failure");
        check(core->failure(group).kind == groupKind, "group deadline/fault distinction preserved");
        if (!cancellation)
          check(failure.code == static_cast<std::int64_t>(expected(point)), "raw fault code preserved");
        check(core->release(group), "failure group release");
        check(!core->stats().records && !core->stats().externalPending &&
                  !core->stats().retainedBytes && !async->stats().pending &&
                  !async->stats().captureBytes && !driver->retainedBytes(),
              "all failure ownership/quotas retired");
        check(!driver->validationErrors(), "managed failure validation");
      }
}
} // namespace
int main(int argc, char **argv) {
  if (argc < 2 || argc > 4)
    return 2;
  const unsigned device = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0;
  const bool validation = argc < 4 || std::string_view(argv[3]) != "--no-validation";
  try {
    // Keep the ICD loaded across independent logical contexts. Repeatedly
    // unloading/reloading NVIDIA's TLS module can exhaust glibc static TLS;
    // the ordinary language registry likewise retains its default context.
    auto anchor = take(v::Runtime::create({.validation = validation, .deviceIndex = device}),
                       "retained ICD context");
    raw(device, validation, argv[1]);
    managed(device, validation, argv[1]);
    check(!anchor->retainedBytes() && !anchor->validationErrors(), "anchor ownership/validation");
    std::puts("PASS: 13 raw failure points, generic pipeline rollback, 36 managed fault/cancel/deadline cases");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
