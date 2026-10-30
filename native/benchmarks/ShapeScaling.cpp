#include "../runtime/Tasks.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <string_view>
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
struct Shape {
  const char *name;
  int rows, inner, columns;
  bool control = false;
};
#ifdef KALANDRA_BLOCKING_SHAPES
static constexpr Shape shapes[] = {
    {"control-32", 32, 32, 32},           {"control-96", 96, 96, 96},
    {"wide-512", 64, 256, 512},           {"square-256", 256, 256, 256},
    {"tall-512", 512, 128, 256},          {"deep-1024", 128, 1024, 128},
    {"deep-tail", 128, 1025, 131},        {"wide-1024", 64, 64, 1024},
    {"mixed-tail", 65, 257, 259},         {"deep-4097", 32, 4097, 67},
    {"deep-8193", 16, 8193, 257},         {"square-512", 512, 512, 512},
    {"large-rectangle", 128, 1024, 1024}, {"streaming-wide", 8, 1025, 8193}};
static constexpr unsigned jobs = 4, warmups = 2, samples = 5;
#else
static constexpr unsigned jobs = 16, warmups = 3, samples = 9;
static constexpr Shape shapes[] = {{"control", 0, 0, 0, true},
                                   {"empty-rows", 0, 9, 7},
                                   {"empty-inner", 3, 0, 7},
                                   {"empty-columns", 3, 9, 0},
                                   {"scalar", 1, 1, 1},
                                   {"singleton-four", 1, 1, 4},
                                   {"singleton-seven", 1, 1, 7},
                                   {"tiny-one", 1, 3, 1},
                                   {"tiny-two", 1, 3, 2},
                                   {"tiny-three", 1, 3, 3},
                                   {"tiny-four", 1, 3, 4},
                                   {"tiny-five", 1, 3, 5},
                                   {"tiny-six", 1, 3, 6},
                                   {"tiny-seven", 1, 3, 7},
                                   {"row-one", 1, 513, 1},
                                   {"row-three", 1, 513, 3},
                                   {"row-four", 1, 513, 4},
                                   {"row-six", 1, 513, 6},
                                   {"row-seven", 1, 513, 7},
                                   {"row-eight", 1, 513, 8},
                                   {"tall-one", 256, 64, 1},
                                   {"tall-three", 256, 64, 3},
                                   {"tall-four", 256, 64, 4},
                                   {"tall-seven", 256, 64, 7},
                                   {"tall-eight", 256, 64, 8},
                                   {"wide", 8, 64, 256},
                                   {"wide-tail", 8, 64, 257},
                                   {"deep-seven", 4, 8193, 7},
                                   {"deep-eight", 4, 8193, 8},
                                   {"rect-four", 32, 129, 64},
                                   {"rect-tail-one", 32, 129, 65},
                                   {"rect-tail-two", 32, 129, 66},
                                   {"rect-tail-three", 32, 129, 67},
                                   {"rect-next-tile", 32, 129, 68},
                                   {"square-eight", 8, 8, 8},
                                   {"square-thirty-two", 32, 32, 32},
                                   {"square-ninety-six", 96, 96, 96}};
#endif
// Independent host reference. Materialize products and build new adjacent-pair
// levels, forcing float32 rounding at each operation. A double grand total is
// insufficient once rectangular/odd shapes expose different rounding trees.
static float balanced(std::vector<float> values) {
  if (values.empty())
    return 0;
  while (values.size() > 1) {
    std::vector<float> next;
    for (std::size_t i = 0; i < values.size(); i += 2) {
      if (i + 1 == values.size())
        next.push_back(values[i]);
      else {
        volatile float rounded = values[i] + values[i + 1];
        next.push_back(static_cast<float>(rounded));
      }
    }
    values = std::move(next);
  }
  return values.front();
}
struct Data {
  Shape shape;
  std::vector<float> left, right;
  Matrix a, b;
  float expected;
  explicit Data(Shape shape)
      : shape(shape), left(shape.rows * shape.inner), right(shape.inner * shape.columns),
        a{left.data(), left.data(), 0, {shape.rows, shape.inner}, {shape.inner, 1}},
        b{right.data(), right.data(), 0, {shape.inner, shape.columns}, {shape.columns, 1}} {
    for (std::size_t i = 0; i < left.size(); ++i)
      left[i] = 1 + (i % 3) * 0.25f;
    for (std::size_t i = 0; i < right.size(); ++i)
      right[i] = 0.5f + (i % 5) * 0.125f;
    std::vector<float> output;
    for (int row = 0; row < shape.rows; ++row)
      for (int col = 0; col < shape.columns; ++col) {
        std::vector<float> products;
        for (int k = 0; k < shape.inner; ++k) {
          volatile float rounded = left[row * shape.inner + k] * right[k * shape.columns + col];
          products.push_back(static_cast<float>(rounded));
        }
        output.push_back(balanced(std::move(products)));
      }
    expected = shape.control ? 1 : balanced(std::move(output));
  }
  void unchanged() const {
    for (std::size_t i = 0; i < left.size(); ++i)
      check(left[i] == 1 + (i % 3) * 0.25f);
    for (std::size_t i = 0; i < right.size(); ++i)
      check(right[i] == 0.5f + (i % 5) * 0.125f);
  }
};
static float kernel(Data &data, Invocation &ctx) {
  return data.shape.control ? 1 : _mlir_ciface_kalandra_test_entry(&data.a, &data.b, &ctx);
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
    check(std::bit_cast<std::uint32_t>(result) == std::bit_cast<std::uint32_t>(data.expected));
  if (core) {
    const auto s = core->stats();
    check(!s.retainedBytes && !s.retainedOwners && !s.unfinished && !s.records && !s.groups &&
          !s.ready && !s.pending && !s.maintenanceTasks);
  }
  return elapsed;
}
int main(int argc, char **argv) {
  bool verify = false;
  unsigned seed = 20261015;
  if (argc == 2 && std::string_view(argv[1]) == "--verify")
    verify = true;
  else if (argc == 3 && std::string_view(argv[1]) == "--seed") {
    char *end;
    auto parsed = std::strtoul(argv[2], &end, 10);
    if (!*argv[2] || *end || parsed > UINT32_MAX)
      return 2;
    seed = parsed;
  } else if (argc != 1)
    return 2;
  const std::array<unsigned, 5> workers{0, 1, 2, 4, 8};
  std::array<std::unique_ptr<TaskCore>, 5> pools;
  for (unsigned i = 1; i < workers.size(); ++i) {
    TaskLimits limits;
    limits.workers = workers[i];
    pools[i] = TaskCore::create(limits);
    check(bool(pools[i]));
  }
  std::mt19937 random(seed);
  auto cases = std::to_array(shapes);
  std::shuffle(cases.begin(), cases.end(), random);
  for (auto shape : cases) {
    Data data(shape);
    Invocation diagnostic;
    check(std::bit_cast<std::uint32_t>(kernel(data, diagnostic)) ==
          std::bit_cast<std::uint32_t>(data.expected));
    check(!diagnostic.status());
    diagnostic.settle();
    check(!diagnostic.live() && diagnostic.allocations() == diagnostic.releases());
    std::printf("{\"case\":\"%s\",\"rows\":%d,\"inner\":%d,\"columns\":%d,\"control\":%s,"
                "\"expected_bits\":%u,\"allocations\":%llu,\"releases\":%llu}\n",
                shape.name, shape.rows, shape.inner, shape.columns,
                shape.control ? "true" : "false", std::bit_cast<std::uint32_t>(data.expected),
                (unsigned long long)diagnostic.allocations(),
                (unsigned long long)diagnostic.releases());
    for (unsigned i = 0; i < workers.size(); ++i)
      for (unsigned warmup = 0; warmup < (verify ? 0 : warmups); ++warmup)
        batch(data, pools[i].get());
    std::array<unsigned, 5> order{0, 1, 2, 3, 4};
    for (unsigned round = 0; round < (verify ? 1 : samples); ++round) {
      std::shuffle(order.begin(), order.end(), random);
      for (const auto i : order)
        std::printf("{\"case\":\"%s\",\"workers\":%u,\"round\":%u,\"jobs\":%u,\"ms\":%.9f}\n",
                    shape.name, workers[i], round, jobs, batch(data, pools[i].get()));
    }
    data.unchanged();
  }
}
