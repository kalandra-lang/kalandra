#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
static void check(bool condition, int line) {
  if (!condition) {
    std::fprintf(stderr, "Compiled worker assertion at line %d\n", line);
    std::exit(1);
  }
}
#define CHECK(x) check(bool(x), __LINE__)
template <class F> static void until(F ready) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready()) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}
struct Matrix {
  float *allocated, *aligned;
  std::int64_t offset, sizes[2], strides[2];
};
extern "C" float _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Invocation *);
struct Data {
  std::vector<float> left, right;
  Matrix a, b;
  Data(std::int64_t rows, std::int64_t inner, std::int64_t columns)
      : left(rows * inner, 1), right(inner * columns, 1),
        a{left.data(), left.data(), 0, {rows, inner}, {inner, 1}},
        b{right.data(), right.data(), 0, {inner, columns}, {columns, 1}} {}
};
static bool copyFloat(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<float *>(out) = *static_cast<const float *>(in);
  return true;
}
static void nothing(void *) noexcept {}
static const ValueLayout scalar{1, sizeof(float), alignof(float), copyFloat, nothing};
struct Shared {
  std::atomic<unsigned> entered{0};
  std::atomic<std::uint64_t> polls[2]{};
  std::atomic<bool> finished[2]{}, stopped[2]{};
  bool barrier = false;
};
struct Input {
  Shared *shared;
  Data *data;
  unsigned index;
};
static bool construct(Invocation &, void *frame, const Packet &packet) {
  new (frame) Input{*static_cast<const Input *>(packet.data)};
  return true;
}
static void resume(Task &task) {
  auto &f = *static_cast<Input *>(task.frame());
  auto &s = *f.shared;
  task.context().watchPolls(&s.polls[f.index]);
  if (s.barrier) {
    ++s.entered;
    until([&] { return s.entered.load() == 2; });
  }
  const auto value = _mlir_ciface_kalandra_test_entry(&f.data->a, &f.data->b, &task.context());
  if (task.context().status())
    task.fault(task.context().status(), task.context().line(), task.context().column());
  else if (task.context().poll())
    s.stopped[f.index] = true;
  else {
    task.complete(&value);
    s.finished[f.index] = true;
  }
}
static const TaskEntry entry{
    1,         5,       sizeof(Input), alignof(Input), sizeof(Input), alignof(Input),
    construct, nothing, resume,        &scalar};
static TaskHandle submit(TaskCore &core, GroupHandle group, Input input) {
  return core.submit(group, entry, {5, &input, sizeof(input)});
}
static void clean(TaskCore &core) {
  auto s = core.stats();
  CHECK(!s.retainedBytes && !s.retainedOwners && !s.unfinished && !s.records && !s.groups);
}
static void observe(TaskCore &core, TaskHandle task, float expected) {
  Invocation context;
  float value = 0;
  CHECK(core.observe(task, scalar, context, &value) && value == expected);
}
static void concurrentKernels() {
  for (const auto &shape :
       {std::array{64, 64, 64}, std::array{7, 513, 33}, std::array{8, 8193, 17}}) {
    auto [rows, inner, columns] = shape;
    Data data(rows, inner, columns);
    const float expected = rows * inner * columns;
    for (auto workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      Shared shared;
      shared.barrier = workers > 1;
      auto root = core->openRoot();
      auto a = submit(*core, root, {&shared, &data, 0});
      auto b = submit(*core, root, {&shared, &data, 1});
      CHECK(core->drain());
      CHECK(shared.finished[0] && shared.finished[1] && shared.polls[0] > 0 && shared.polls[1] > 0);
      if (workers > 1)
        CHECK(core->stats().peakRunning >= 2);
      observe(*core, a, expected);
      observe(*core, a, expected);
      observe(*core, b, expected);
      CHECK(core->close(root) && core->drain() && core->release(root));
      clean(*core);
    }
  }
}
static void cancelKernel(bool deadline, std::int64_t columns) {
  // Narrow/pair/tail kernels need enough contraction work to cancel while
  // inside generated loops rather than merely at the task entry boundary.
  Data large(257, columns < 256 ? 8193 : 512, columns), small(8, 8, 8);
  // A finite kernel can finish before the controller gets CPU time. Require
  // a genuine interrupted invocation, without assuming a scheduling latency.
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    TaskLimits limits;
    limits.workers = 2;
    // Wide kernels reserve a packed operand larger than the default 1 MiB.
    // Exercise cancellation rather than exhausting that unrelated task budget.
    limits.bytes = 16 * 1024 * 1024;
    auto core = TaskCore::create(limits);
    CHECK(core);
    Shared shared;
    auto root = core->openRoot(), independent = core->openRoot();
    auto running = submit(*core, root, {&shared, &large, 0});
    auto other = submit(*core, independent, {&shared, &small, 1});
    std::jthread runner([&] { CHECK(core->drain()); });
    until([&] { return shared.polls[0].load() >= 1000; });
    CHECK(core->cancel(root, deadline));
    runner.join();
    if (shared.finished[0]) {
      // The controller may be descheduled after observing a poll while the
      // kernel completes. That run did not exercise in-loop cancellation.
      // Settle it and retry; never count successful completion as interruption.
      CHECK(core->close(independent) && core->drain());
      CHECK(core->release(root) && core->release(independent));
      clean(*core);
      continue;
    }
    CHECK(shared.stopped[0] && !shared.finished[0] &&
          core->failure(running).kind == TaskFailure::Cancelled);
    CHECK(core->failure(root).kind ==
          (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    observe(*core, other, 512);
    CHECK(core->close(independent) && core->drain());
    CHECK(core->release(root) && core->release(independent));
    clean(*core);
    for (auto x : large.left)
      CHECK(x == 1);
    for (auto x : large.right)
      CHECK(x == 1);
    return;
  }
  CHECK(false); // No actual in-loop cancellation observed in the bounded retries.
}
static void faultCancelsKernel() {
  Data large(256, 512, 256), bad(8, 8, 8), small(8, 8, 8);
  bad.b.sizes[0] = 7;
  TaskLimits limits;
  limits.workers = 3;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared;
  shared.barrier = true;
  Shared healthy;
  auto group = core->openRoot(), independent = core->openRoot();
  auto a = submit(*core, group, {&shared, &large, 0});
  auto b = submit(*core, group, {&shared, &bad, 1});
  auto other = submit(*core, independent, {&healthy, &small, 0});
  CHECK(core->drain());
  CHECK(core->failure(group).kind == TaskFailure::Fault && core->failure(group).code == 1);
  CHECK(core->failure(a).kind == TaskFailure::Cancelled &&
        core->failure(b).kind == TaskFailure::Fault);
  CHECK(!shared.finished[0]);
  observe(*core, other, 512);
  CHECK(core->close(independent) && core->drain() && core->release(group) &&
        core->release(independent));
  clean(*core);
}
static void cancelledEntry() {
  Data data(8, 8, 8);
  std::atomic<bool> token{true};
  std::atomic<std::uint64_t> polls{0};
  Invocation context;
  context.bindCancellation(&token);
  context.watchPolls(&polls);
  (void)_mlir_ciface_kalandra_test_entry(&data.a, &data.b, &context);
  CHECK(!context.status() && !context.allocations() && polls > 0);
  context.settle();
  CHECK(!context.live());
}
int main() {
  cancelledEntry();
  concurrentKernels();
  for (auto columns : {1, 2, 3, 6, 7, 16, 17, 18, 19, 31, 32, 33, 256}) {
    cancelKernel(false, columns);
    cancelKernel(true, columns);
  }
  faultCancelsKernel();
  std::puts("PASS generated CPU kernels across 1/2/4 workers, in-loop cancellation/deadlines, "
            "checked faults, unchanged inputs, and complete cleanup");
}
