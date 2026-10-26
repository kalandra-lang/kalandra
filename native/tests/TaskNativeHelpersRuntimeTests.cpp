#include "../runtime/Tasks.h"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
extern "C" const TaskEntry *const kalandra_task_test_entry;
struct Input { std::int64_t kind, seed, rounds; };
struct PacketValue { Failure failure{}; Input value; };
struct Output { Failure failure; std::int64_t value; };
static void check(bool ok, int line) {
  if (!ok) throw std::runtime_error("native task helper failure at " + std::to_string(line));
}
#define CHECK(value) check(bool(value), __LINE__)
static void clean(Invocation &context) {
  CHECK(!context.callDepth());
  context.settle();
  CHECK(!context.live() && context.allocations() == context.releases());
}
static void clean(const TaskCore &core) {
  const auto stats = core.stats();
  CHECK(!stats.retainedOwners && !stats.retainedBytes && !stats.environmentBytes &&
        !stats.recursiveBytes && !stats.records && !stats.groups && !stats.unfinished &&
        !stats.pending && !stats.externalPending);
}
static std::int64_t oracle(Input input) {
  if (input.kind == 2) return input.seed*3+input.rounds;
  if (input.kind == 3) return input.seed*2+input.rounds%8;
  std::array<std::int64_t, 8> values{};
  const auto size = input.kind ? 8u : 1u;
  for (unsigned i = 0; i < size; ++i) values[i] = input.seed + i;
  for (std::int64_t n = 0; n < input.rounds; ++n)
    for (unsigned i = 0; i < size; ++i)
      values[i] = (values[i]*13 + (input.kind ? 7 : input.seed+7)) % 1000003;
  std::int64_t result = input.kind ? input.seed : 0;
  for (unsigned i = 0; i < size; ++i) result += values[i];
  return result;
}
static std::size_t run(unsigned workers, Input input, std::size_t bytes = 1024*1024,
                        bool fault = false) {
  TaskLimits limits;
  limits.workers = workers;
  limits.bytes = bytes;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Invocation source, observer;
  const auto &entry = *kalandra_task_test_entry;
  PacketValue packet{{},input};
  const auto before = packet;
  CHECK(entry.packetBytes == sizeof(packet) && entry.result->bytes == sizeof(Output));
  const auto root = core->openRoot();
  const auto task = core->submit(root, entry, {entry.packetLayout,&packet,sizeof(packet),&source});
  source.settle();
  CHECK(task && core->close(root) && core->drain());
  Output output;
  std::memset(&output,0x5a,sizeof(output));
  const auto untouched = output;
  if (fault) {
    CHECK(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 2);
    CHECK(!core->observe(task,*entry.result,observer,&output));
    CHECK(!std::memcmp(&output,&untouched,sizeof(output)));
  } else {
    CHECK(core->failure(task).kind == TaskFailure::None);
    CHECK(core->observe(task,*entry.result,observer,&output));
    CHECK(output.failure.kind == TaskFailure::None && output.value == oracle(input));
  }
  const auto peak = core->stats().peakBytes;
  CHECK(!std::memcmp(&packet,&before,sizeof(packet)) && peak <= bytes);
  CHECK(core->release(root));
  clean(*core);
  clean(source);
  clean(observer);
  return peak;
}
static void cancellation(unsigned workers, bool deadline) {
  TaskLimits limits;
  limits.workers = workers;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Invocation source, observer;
  const auto &entry = *kalandra_task_test_entry;
  PacketValue packet{{},{0,17,1000000000000LL}};
  const auto root = core->openRoot();
  const auto task = core->submit(root,entry,{entry.packetLayout,&packet,sizeof(packet),&source});
  CHECK(task && core->close(root));
  std::jthread stop([&] {
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!core->stats().peakRunning && std::chrono::steady_clock::now() < until)
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(core->cancel(root,deadline));
  });
  const auto start = std::chrono::steady_clock::now();
  CHECK(core->drain());
  stop.join();
  CHECK(std::chrono::steady_clock::now()-start < std::chrono::seconds(5));
  CHECK(core->failure(root).kind ==
        (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
  CHECK(core->failure(task).kind == TaskFailure::Cancelled);
  Output output{};
  CHECK(!core->observe(task,*entry.result,observer,&output));
  CHECK(core->release(root));
  clean(*core);
  clean(source);
  clean(observer);
}
int main() {
  for (unsigned workers : {1u,2u,4u}) {
    for (std::int64_t seed : {1,17,31})
      for (std::int64_t rounds : {0,1,7,16,256})
        for (std::int64_t kind : {0,1,2,3}) run(workers,{kind,seed,rounds});
    const auto shallow = run(workers,{0,17,1});
    const auto deep = run(workers,{0,17,1000000},64*1024);
    CHECK(deep == shallow); // Recursion must not consume per-step heap frames.
    const Input arrays{1,17,256};
    const auto peak = run(workers,arrays);
    run(workers,arrays,peak-1,true);
    run(workers,arrays,peak);
    cancellation(workers,false);
    cancellation(workers,true);
  }
  std::cout << "Native task helpers: 180 exact oracle/immutable/aliased-view/shape cases, million-step "
               "constant-space recursion, exact/rejected budgets, cancellation/deadline and "
               "balanced ownership with 1/2/4 workers passed\n";
}
