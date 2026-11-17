#if defined(KALANDRA_TEST_CUDA)
#include "../cuda/Language.h"
namespace backend = kalandra::runtime::cuda::language;
#elif defined(KALANDRA_TEST_VULKAN)
#include "../vulkan/Language.h"
namespace backend = kalandra::runtime::vulkan::language;
#else
#include "../metal/Metal.h"
namespace backend = kalandra::runtime::metal;
#endif
#include <array>
#include <bit>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, backend::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
backend::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation invocation;
  alignas(backend::Buffer) std::byte storage[sizeof(backend::Buffer)];
  check(core.observe(task, backend::Runtime::bufferLayout(), invocation, storage),
        "observe cooperative outcome");
  auto *value = std::launder(reinterpret_cast<backend::Buffer *>(storage));
  auto out = *value;
  std::destroy_at(value);
  return out;
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 2 && argc != 3)
      return 2;
    const bool uniform = argc == 3;
    const bool fault = uniform && std::string_view(argv[2]) == "fault";
    auto runtime = take(backend::Runtime::create(), "runtime");
    auto device = take(runtime->device(), "device");
    WorkgroupLaunch launch{{4}, {}, {{{WorkgroupShapeAction::Scalar, 0, 0}}}, 16, {}, 3};
    const std::array scalarTypes{NumericType::Int64};
    auto kernel = take(runtime->loadWorkgroupKernel(argv[1], device, NumericType::Float32, {},
                                                    scalarTypes, launch),
                       "error kernel");
    auto forged = launch;
    forged.errorWords = 4;
    check(!runtime->loadWorkgroupKernel(argv[1], device, NumericType::Float32, {}, scalarTypes,
                                        forged),
          "error envelope cannot masquerade as another payload ABI");
    const std::array<std::uint64_t, 1> scalars{4};
    backend::NumericInstruction instruction{NumericProgramKind::Workgroup, kernel, {}, scalars, 0};
    auto executable = take(runtime->prepareNumericProgram(0, std::span(&instruction, 1), 0,
                                                          NumericType::Float32, false),
                           "error program");
    for (auto workers : {1u, 4u}) {
      auto core = TaskCore::create({.workers = workers, .records = 32});
      check(bool(core), "core");
      if (fault) {
        // A fatal fault cancels sibling tasks in the same group. Isolate these
        // launches so every nonempty case actually executes its checked work.
        for (unsigned count : {0u, 1u, 3u, 5u, 65537u}) {
          auto group = core->openRoot();
          const std::array<std::uint64_t, 1> capture{count};
          auto task = runtime->executeNumericProgram(*core, group, executable, {}, capture);
          check(bool(task) && core->close(group) && core->drain(), "settle isolated fault outcome");
          if (count)
            check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 4,
                  "checked arithmetic fault precedes application-error export");
          else {
            auto result = observe(*core, task);
            auto outcome = backend::Runtime::workgroupOutcome(result);
            check(outcome && outcome->error.empty(), "empty domain does not execute faulting work");
          }
          check(core->release(group) && !core->stats().records && !core->stats().externalPending,
                "retire fault outcome ownership");
        }
        continue;
      }
      auto group = core->openRoot();
      std::vector<std::pair<TaskHandle, unsigned>> tasks;
      for (unsigned count : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 65537u, 4u, 5u, 0u}) {
        const std::array<std::uint64_t, 1> capture{count};
        auto task = runtime->executeNumericProgram(*core, group, executable, {}, capture);
        check(bool(task), "admit cooperative outcome");
        tasks.emplace_back(task, count);
      }
      check(core->close(group) && core->drain(), "settle concurrent outcomes");
      for (auto [task, count] : tasks) {
        check(core->failure(task).kind == TaskFailure::None,
              "expected error stays separate from device failure");
        auto result = observe(*core, task);
        auto outcome = backend::Runtime::workgroupOutcome(result);
        check(bool(outcome) && outcome->type == NumericType::Float32 && outcome->rank == 1 &&
                  outcome->dimensions[0] == count,
              "private outcome metadata and shape");
        if (count < (uniform ? 3u : 5u))
          check(outcome->error.empty(), "success isolated from failing neighbors");
        else if (uniform) {
          check(
              outcome->error.size() == 3 && outcome->error[0] == 1 &&
                  outcome->error[1] < ((std::uint64_t(count) + 3) / 4) * 4 &&
                  outcome->error[2] == std::bit_cast<std::uint32_t>(float(outcome->error[1] % 7)),
              "one complete error payload, including many simultaneous failures before a barrier");
        } else
          check(outcome->error == std::vector<std::uint64_t>{1, 5, UINT64_C(0x40400000)},
                "typed error discriminator, index and float payload including padding lanes");
      }
      check(core->release(group) && core->stats().records == 0 && !core->stats().externalPending,
            "retire expected-error ownership");
      for (bool deadline : {false, true}) {
        runtime->holdCompletions(true);
        group = core->openRoot();
        const std::array<std::uint64_t, 1> capture{6};
        auto task = runtime->executeNumericProgram(*core, group, executable, {}, capture);
        check(bool(task), "admit cancellation outcome");
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (runtime->stats().inFlight == 0 && std::chrono::steady_clock::now() < limit) {
          core->step();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(runtime->stats().inFlight != 0, "pending device acknowledgement");
        check(core->cancel(group, deadline), "cancel/deadline outcome");
        check(core->close(group), "close canceled outcome");
        runtime->holdCompletions(false);
        check(core->drain(), "acknowledge canceled outcome");
        check(core->failure(group).kind ==
                  (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
              "cancellation remains a scheduling failure");
        check(core->release(group) && core->stats().records == 0 && !core->stats().externalPending,
              "retire canceled outcome");
      }
    }
    std::cout << (fault ? "PASS: checked faults precede application errors; empty domains skip "
                          "faulting work; owners retire\n"
                        : "PASS: cooperative typed-error publication, concurrent isolation, "
                          "padding, cancellation and retirement\n");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
