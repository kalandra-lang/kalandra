#include "Async.h"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace gpu = kalandra::runtime::cuda;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, gpu::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
gpu::TensorValue observe(TaskCore &core, TaskHandle task) {
  Invocation invocation;
  alignas(gpu::TensorValue) std::byte bytes[sizeof(gpu::TensorValue)];
  check(core.observe(task, gpu::AsyncRuntime::tensorLayout(), invocation, bytes), "observe");
  auto value = std::launder(reinterpret_cast<gpu::TensorValue *>(bytes));
  auto result = *value;
  value->~TensorValue();
  return result;
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected source-generated step kernel");
    std::shared_ptr<gpu::Runtime> runtime = take(gpu::Runtime::create(), "runtime");
    gpu::ProgramKernel step{take(runtime->loadKernel(argv[1]), "kernel"),
                            NumericType::Float32,
                            {NumericType::Float32},
                            {},
                            {0},
                            false};
    std::vector<gpu::ProgramInstruction> code;
    constexpr unsigned stages = 257;
    for (unsigned i = 0; i < stages; ++i)
      code.push_back({NumericProgramKind::Map, step, {i}, {}, {}, i + 1});
    auto program = take(
        gpu::Program::prepare(*runtime, 1, code, stages, NumericType::Float32, false), "program");
    std::array<float, 3> initial{1, 2, 3};
    gpu::TensorValue input{take(runtime->uploadBytes(std::as_bytes(std::span(initial))), "input"),
                           NumericType::Float32};
    input.rank = 3;
    input.dimensions = {1, 3, 1};
    for (unsigned workers : {1u, 2u, 4u}) {
      auto core = TaskCore::create({.workers = workers, .records = 1});
      auto async = gpu::AsyncRuntime::create(runtime);
      check(bool(core) && bool(async), "core and completion service");
      auto release = [&](GroupHandle group) {
        check(core->drain() && core->release(group), "release group");
        check(!core->stats().records && !core->stats().externalPending &&
                  !core->stats().retainedBytes,
              "task ownership retired");
        check(!async->stats().pending && !async->stats().captureBytes, "capture ownership retired");
      };
      auto group = core->openRoot();
      auto task = async->executeProgram(*core, group, program, std::array{input}, {});
      check(bool(task) && core->close(group) && core->drain(), "257 stages in one task record");
      check(core->failure(task).kind == TaskFailure::None, "success");
      auto value = observe(*core, task);
      auto bytes = take(runtime->downloadBytes(value.data), "result");
      std::array<float, 3> expected{772, 773, 774};
      auto reference = std::as_bytes(std::span(expected));
      check(std::equal(bytes.begin(), bytes.end(), reference.begin(), reference.end()),
            "exact source-generated graph result");
      check(value.rank == 3 && value.dimensions == input.dimensions, "rank metadata retained");
      release(group);
      if (workers == 1) {
        for (unsigned rank = 0; rank <= 8; ++rank) {
          const std::array<float, 1> singleton{1};
          auto ranked = input;
          ranked.rank = rank;
          ranked.dimensions.fill(1);
          if (!rank)
            ranked.data =
                take(runtime->uploadBytes(std::as_bytes(std::span(singleton))), "rank-zero tensor");
          else
            ranked.dimensions[rank - 1] = 3;
          group = core->openRoot();
          task = async->executeProgram(*core, group, program, std::array{ranked}, {});
          check(bool(task) && core->close(group) && core->drain(), "ranked program");
          if (core->failure(task).kind != TaskFailure::None)
            throw std::runtime_error("rank " + std::to_string(rank) + " failure " +
                                     std::to_string(core->failure(task).code));
          auto result = observe(*core, task);
          check(!result.scalar && result.rank == rank && result.dimensions == ranked.dimensions,
                "ranks zero through eight preserved");
          auto actual = take(runtime->downloadBytes(result.data), "ranked readback");
          auto oracle = reference.first(rank ? reference.size() : sizeof(float));
          check(std::equal(actual.begin(), actual.end(), oracle.begin(), oracle.end()),
                "ranked exact output");
          release(group);
        }
        // Empty tensor storage remains a host descriptor; no zero-byte CUDA
        // allocation or dispatch is necessary for a graph of elementwise maps.
        auto hostEmpty = take(
            gpu::hostTensor(*runtime, NumericType::Float32, {}, std::array<std::int64_t, 1>{0}),
            "empty tensor");
        auto empty = take(gpu::transferTensor(*runtime, hostEmpty, true), "empty upload");
        group = core->openRoot();
        task = async->executeProgram(*core, group, program, std::array{empty}, {});
        check(bool(task) && core->close(group) && core->drain(), "empty graph");
        auto result = observe(*core, task);
        check(result.rank == 1 && result.dimensions[0] == 0 && result.bytes() == 0,
              "empty result shape");
        release(group);
      }
      for (bool deadline : {false, true}) {
        async->holdCompletions(true);
        group = core->openRoot();
        auto before = async->stats().stages;
        task = async->executeProgram(*core, group, program, std::array{input}, {});
        check(bool(task) && core->close(group), "held submit");
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!async->stats().held && std::chrono::steady_clock::now() < until) {
          core->step();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(async->stats().held == 1, "completed device stage held for acknowledgement");
        check(core->cancel(group, deadline) && !core->settled(group),
              "cancellation retains unacknowledged captures");
        async->holdCompletions(false);
        check(core->drain() &&
                  core->failure(group).kind ==
                      (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
              "cancellation precedence");
        check(async->stats().stages - before == 1, "cancel skips remaining 256 stages");
        release(group);
      }
      async->failNextCompletion();
      group = core->openRoot();
      task = async->executeProgram(*core, group, program, std::array{input}, {});
      check(bool(task) && core->close(group) && core->drain(),
            "simulated completion failure settled");
      check(core->failure(task).kind != TaskFailure::None, "failure surfaced");
      release(group);
    }
    std::cout << "PASS: source-generated CUDA graph, 257 stages/one task record; 1/2/4 workers, "
                 "exact results, ranks0..8/empty tensors, cancellation/deadline acknowledgement, "
                 "simulated failure and ownership retirement\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
