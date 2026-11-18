#if defined(KALANDRA_TEST_CUDA)
#include "../cuda/Async.h"
namespace backend = kalandra::runtime::cuda;
#else
#include "../vulkan/Async.h"
namespace backend = kalandra::runtime::vulkan;
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool valid, const char *message) {
  if (!valid)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, backend::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
backend::TensorValue observe(TaskCore &core, TaskHandle handle) {
  Invocation invocation;
  alignas(backend::TensorValue) std::byte storage[sizeof(backend::TensorValue)];
  check(core.observe(handle, backend::AsyncRuntime::tensorLayout(), invocation, storage),
        "observe cooperative result");
  auto *value = std::launder(reinterpret_cast<backend::TensorValue *>(storage));
  auto result = *value;
  value->~TensorValue();
  return result;
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 2 || argc == 3,
          "expected source-generated cooperative stencil artifact and optional evidence directory");
    if (argc == 3)
      std::filesystem::create_directories(argv[2]);
    std::shared_ptr<backend::Runtime> runtime = take(backend::Runtime::create(), "runtime");
    auto launch = std::make_shared<WorkgroupLaunch>(
        WorkgroupLaunch{{256}, {1}, {{{WorkgroupShapeAction::Extent, 0, 0, 0}}}, 258 * 4});
    backend::ProgramKernel schema;
    schema.type = NumericType::Float32;
    schema.tensorTypes = {NumericType::Float32};
    schema.scalarBuffers = {0};
    schema.workgroup = launch;
    auto contract = take(backend::kernelContract(schema, NumericProgramKind::Workgroup),
                         "cooperative contract");
    schema.kernel = take(runtime->loadKernel(argv[1], &contract), "cooperative kernel");
    backend::ProgramInstruction instruction{NumericProgramKind::Workgroup, schema, {0}, {}, {}, 1};
    auto program = take(backend::Program::prepare(*runtime, 1, std::span(&instruction, 1), 1,
                                                  NumericType::Float32, false),
                        "cooperative program");
    auto forged = instruction;
    forged.kind = NumericProgramKind::Map;
    check(!backend::Program::prepare(*runtime, 1, std::span(&forged, 1), 1, NumericType::Float32,
                                     false),
          "workgroup ABI cannot masquerade as a map");
    for (unsigned workers : {1u, 4u}) {
      auto core = TaskCore::create({.workers = workers, .records = 4});
      auto async = backend::AsyncRuntime::create(runtime);
      check(bool(core) && bool(async), "scheduler and completion service");
      auto release = [&](GroupHandle group) {
        check(core->drain() && core->release(group), "release cooperative group");
        check(!core->stats().records && !core->stats().externalPending &&
                  !core->stats().retainedBytes,
              "cooperative task ownership retired");
        check(!async->stats().pending && !async->stats().captureBytes,
              "cooperative captures retired");
      };
      for (unsigned count : {0u, 1u, 4u, 255u, 256u, 257u, 512u, 1003u, 65537u}) {
        std::vector<float> values(count), expected(count);
        for (unsigned i = 0; i < count; ++i)
          values[i] = float(int(i % 37) - 18) * 0.25f;
        for (unsigned i = 0; i < count; ++i)
          expected[i] =
              ((values[i ? i - 1 : 0] + values[i]) + values[std::min(i + 1, count - 1)]) / 3.0f;
        backend::TensorValue input{
            take(runtime->uploadBytes(std::as_bytes(std::span(values))), "upload"),
            NumericType::Float32};
        input.rank = 1;
        input.dimensions[0] = count;
        auto group = core->openRoot();
        const auto before = async->stats().stages;
        auto task = async->executeProgram(*core, group, program, std::array{input}, {});
        check(bool(task) && core->close(group) && core->drain(), "execute cooperative stencil");
        check(core->failure(task).kind == TaskFailure::None, "cooperative stencil success");
        auto result = observe(*core, task);
        check(!result.scalar && result.rank == 1 && result.dimensions[0] == count,
              "logical cooperative output shape");
        auto bytes = take(runtime->downloadBytes(result.data), "download cooperative result");
        auto reference = std::as_bytes(std::span(expected));
        if (argc == 3) {
          auto file = std::filesystem::path(argv[2]) /
                      (std::to_string(workers) + "-" + std::to_string(count) + ".bin");
          std::ofstream(file, std::ios::binary)
              .write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
        bool valid = std::equal(bytes.begin(), bytes.end(), reference.begin(), reference.end());
#if !defined(KALANDRA_TEST_CUDA)
        // This fixture's two additions are exact: all inputs are small integer
        // multiples of 1/4. Only the final division needs an error interval.
        // Use Vulkan's existing 2.5-ULP FDiv guarantee, not a tolerance fitted
        // to this GPU. The independent Decimal oracle also audits raw outputs.
        valid = bytes.size() == reference.size();
        for (unsigned i = 0; i < count && valid; ++i) {
          float actual;
          std::memcpy(&actual, bytes.data() + i * 4, 4);
          const long double numerator = static_cast<long double>(values[i ? i - 1 : 0]) +
                                        values[i] + values[std::min(i + 1, count - 1)];
          const long double exact = numerator / 3.0L;
          int exponent = 0;
          std::frexp(std::fabs(exact), &exponent);
          auto ulp = std::ldexp(1.0L, exponent - 24);
          if (std::fabs(exact) == std::ldexp(1.0L, exponent - 1))
            ulp *= 0.5L;
          valid = std::isfinite(actual) &&
                  (exact == 0 ? std::bit_cast<std::uint32_t>(actual) ==
                                    std::bit_cast<std::uint32_t>(expected[i])
                              : std::fabs(static_cast<long double>(actual) - exact) <= 2.5L * ulp);
        }
#endif
        if (!valid) {
          for (unsigned i = 0; i < count; ++i) {
            float actual;
            std::memcpy(&actual, bytes.data() + i * 4, 4);
            if (std::bit_cast<std::uint32_t>(actual) != std::bit_cast<std::uint32_t>(expected[i])) {
              std::cerr << "count=" << count << " index=" << i << " expected=" << std::hex
                        << std::bit_cast<std::uint32_t>(expected[i])
                        << " actual=" << std::bit_cast<std::uint32_t>(actual) << std::dec << '\n';
              break;
            }
          }
          check(false, "exact independent stencil reference");
        }
        check(async->stats().stages - before == (count ? 1u : 0u),
              "empty domain submits no GPU work");
        release(group);
      }
      std::array<float, 4> values{3, 6, 9, 12};
      backend::TensorValue input{
          take(runtime->uploadBytes(std::as_bytes(std::span(values))), "upload cancellation input"),
          NumericType::Float32};
      input.rank = 1;
      input.dimensions[0] = values.size();
      for (bool deadline : {false, true}) {
        async->holdCompletions(true);
        auto group = core->openRoot();
        auto task = async->executeProgram(*core, group, program, std::array{input}, {});
        check(bool(task) && core->close(group), "submit held cooperative group");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!async->stats().held && std::chrono::steady_clock::now() < until) {
          core->step();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(async->stats().held == 1, "hold fence-complete cooperative command");
        check(core->cancel(group, deadline) && !core->settled(group),
              "cancel retains cooperative buffers until acknowledgement");
        async->holdCompletions(false);
        check(core->drain() &&
                  core->failure(group).kind ==
                      (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
              "cooperative cancellation contract");
        release(group);
      }
      // A nonempty output with an empty input must fault safely. Every padded
      // invocation still reaches the barrier; no speculative invalid read.
      auto failingLaunch = std::make_shared<WorkgroupLaunch>(*launch);
      failingLaunch->shape = {{{WorkgroupShapeAction::Constant, 4}}};
      auto failing = instruction;
      failing.kernel->workgroup = failingLaunch;
      auto faultProgram = take(backend::Program::prepare(*runtime, 1, std::span(&failing, 1), 1,
                                                         NumericType::Float32, false),
                               "fault program");
      backend::TensorValue empty{take(runtime->uploadBytes({}), "empty fault input"),
                                 NumericType::Float32};
      empty.rank = 1;
      auto group = core->openRoot();
      auto task = async->executeProgram(*core, group, faultProgram, std::array{empty}, {});
      check(bool(task) && core->close(group) && core->drain(), "cooperative fault settles");
      check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 4,
            "cooperative checked fault preserved");
      release(group);
    }
    std::cout << "PASS: cooperative GPU stencil, independent numerical reference/padding/empty "
                 "domain, 1/4 workers, cancellation/deadline/fault and ownership\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
