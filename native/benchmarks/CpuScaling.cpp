#include "../runtime/Tasks.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <vector>
using namespace kalandra::runtime;
using Clock = std::chrono::steady_clock;
static void checked(bool ok, int line) {
  if (!ok) {
    std::fprintf(stderr, "CPU benchmark validation failed at line %d\n", line);
    std::exit(1);
  }
}
#define check(x) checked(bool(x), __LINE__)
static double ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct Matrix {
  float *allocated, *aligned;
  std::int64_t offset, sizes[2], strides[2];
};
extern "C" float _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Invocation *);
struct Data {
  std::vector<float> left, right;
  Matrix a, b;
  float expected = 1;
  explicit Data(int n)
      : left(n * n), right(n * n), a{left.data(), left.data(), 0, {n, n}, {n, 1}},
        b{right.data(), right.data(), 0, {n, n}, {n, 1}} {
    if (!n)
      return;
    for (int i = 0; i < n * n; ++i) {
      left[i] = 1 + (i % 3) * 0.25f;
      right[i] = 0.5f + (i % 5) * 0.125f;
    }
    double reference = 0;
    for (int r = 0; r < n; ++r)
      for (int c = 0; c < n; ++c)
        for (int k = 0; k < n; ++k)
          reference += double(left[r * n + k]) * right[k * n + c];
    expected = static_cast<float>(reference);
  }
};
static float kernel(Data &data, Invocation &ctx) {
  return data.left.empty() ? 1 : _mlir_ciface_kalandra_test_entry(&data.a, &data.b, &ctx);
}
static bool copy(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<float *>(out) = *static_cast<const float *>(in);
  return true;
}
static void nothing(void *) noexcept {}
static const ValueLayout scalar{1, sizeof(float), alignof(float), copy, nothing};
static bool construct(Invocation &, void *frame, const Packet &packet) {
  new (frame) Data *{*static_cast<Data *const *>(packet.data)};
  return true;
}
static void resume(Task &task) {
  auto &data = **static_cast<Data **>(task.frame());
  const auto result = kernel(data, task.context());
  if (task.context().status())
    task.fault(task.context().status());
  else
    task.complete(&result);
}
static const TaskEntry entry{
    1,         2,       sizeof(Data *), alignof(Data *), sizeof(Data *), alignof(Data *),
    construct, nothing, resume,         &scalar};
static constexpr unsigned jobs = 16;
static double batch(Data &data, TaskCore *core) {
  std::array<float, jobs> results{};
  const auto start = Clock::now();
  if (!core) {
    for (auto &result : results) {
      Invocation ctx;
      result = kernel(data, ctx);
      check(!ctx.status());
      ctx.settle();
      check(!ctx.live());
    }
  } else {
    const auto group = core->openRoot();
    std::array<TaskHandle, jobs> tasks;
    auto *input = &data;
    for (auto &task : tasks)
      task = core->submit(group, entry, {2, &input, sizeof(input)});
    check(core->drain());
    check(core->failure(group).kind == TaskFailure::None);
    for (unsigned i = 0; i < jobs; ++i) {
      Invocation reader;
      check(core->observe(tasks[i], scalar, reader, &results[i]));
    }
    check(core->close(group) && core->drain());
    check(core->failure(group).kind == TaskFailure::None);
    check(core->release(group));
  }
  const auto elapsed = ms(start);
  for (auto result : results)
    check(result == data.expected);
  if (core) {
    const auto s = core->stats();
    check(!s.retainedBytes && !s.retainedOwners && !s.unfinished && !s.records && !s.groups &&
          !s.ready && !s.pending);
  }
  return elapsed;
}
int main() {
  const std::array<unsigned, 5> workers{0, 1, 2, 4, 8};
  std::array<std::unique_ptr<TaskCore>, 5> pools;
  for (unsigned i = 1; i < workers.size(); ++i) {
    TaskLimits limits;
    limits.workers = workers[i];
    const auto start = Clock::now();
    pools[i] = TaskCore::create(limits);
    check(bool(pools[i]));
    std::printf("{\"startup_workers\":%u,\"ms\":%.9f}\n", workers[i], ms(start));
  }
  std::mt19937 random(20261015);
  for (const int n : {0, 8, 32, 96}) {
    Data data(n);
    Invocation diagnostic;
    check(kernel(data, diagnostic) == data.expected && !diagnostic.status());
    diagnostic.settle();
    check(!diagnostic.live());
    std::printf("{\"dimension\":%d,\"expected_result\":%.9g,\"allocations_per_kernel\":%llu,"
                "\"releases_per_kernel\":%llu}\n",
                n, double(data.expected), static_cast<unsigned long long>(diagnostic.allocations()),
                static_cast<unsigned long long>(diagnostic.releases()));
    for (unsigned i = 0; i < workers.size(); ++i)
      for (int warmup = 0; warmup < 3; ++warmup)
        batch(data, pools[i].get());
    std::array<unsigned, 5> order{0, 1, 2, 3, 4};
    for (int round = 0; round < 9; ++round) {
      std::shuffle(order.begin(), order.end(), random);
      for (const auto i : order)
        std::printf("{\"dimension\":%d,\"workers\":%u,\"round\":%d,\"jobs\":%u,\"ms\":%.9f}\n", n,
                    workers[i], round, jobs, batch(data, pools[i].get()));
    }
    for (std::size_t i = 0; i < data.left.size(); ++i) {
      check(data.left[i] == 1 + (i % 3) * 0.25f);
      check(data.right[i] == 0.5f + (i % 5) * 0.125f);
    }
    for (unsigned i = 1; i < workers.size(); ++i) {
      const auto s = pools[i]->stats();
      std::printf("{\"dimension\":%d,\"workers\":%u,\"peak_running_cumulative\":%u,"
                  "\"peak_frame_result_bytes_cumulative\":%zu}\n",
                  n, workers[i], s.peakRunning, s.peakBytes);
    }
  }
}
