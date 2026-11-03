// Compile beside the generated host.cpp, replacing only its CLI entry point.
#define main kalandra_generated_cli
#include "host.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
static std::atomic<std::uint64_t> observedPolls;
static std::uint64_t allocationFailure;
static bool constructObserved(Invocation &context, void *frame, const Packet &packet) {
  context.failAllocationAt(allocationFailure);
  context.watchPolls(&observedPolls);
  return entry.construct(context, frame, packet);
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected shader directory");
    auto created = m::Runtime::create();
    require(bool(created), "GPU runtime creation failed");
    auto runtime = std::move(*created);
    for (unsigned workers : {1u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "task core creation failed");
      auto observed = entry;
      observed.construct = constructObserved;
      for (unsigned mode = 0; mode < 67; ++mode) {
        allocationFailure = mode >= 3 ? mode - 2 : 0;
        observedPolls = 0;
        auto group = core->openRoot();
        PacketData input{runtime.get(), argv[1]};
        auto task =
            core->submit(group, observed, {observed.packetLayout, &input, sizeof(input), nullptr});
        if (mode == 1)
          require(core->cancel(group), "pre-cancellation failed");
        std::atomic<bool> finished{false};
        bool drained = false;
        std::jthread running([&] {
          drained = core->drain();
          finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::uint64_t peakBytes = 0, peakOwners = 0;
        bool cancelledRunning = false;
        while (!finished.load(std::memory_order_acquire)) {
          const auto stats = core->stats();
          peakBytes = std::max<std::uint64_t>(peakBytes, stats.retainedBytes);
          peakOwners = std::max<std::uint64_t>(peakOwners, stats.retainedOwners);
          if (mode == 2 && !cancelledRunning && observedPolls.load() >= 64) {
            require(core->cancel(group), "running cancellation failed");
            cancelledRunning = true;
          }
          require(std::chrono::steady_clock::now() < deadline, "host array task did not settle");
          std::this_thread::yield();
        }
        running.join();
        require(drained, "host array task drain failed");
        auto failure = core->failure(group);
        if (failure.kind == TaskFailure::None && task)
          failure = core->failure(task);
        Invocation output;
        std::int64_t value = -1;
        if (failure.kind == TaskFailure::None && task) {
          require(core->observe(task, *entry.result, output, &value),
                  "host array observation failed");
          require(value == 2538, "mixed array aliases or numerical result changed");
        }
        if (mode == 0)
          require(task && failure.kind == TaskFailure::None && peakBytes < 1024 * 1024 &&
                      peakOwners < 128,
                  "structured array loop failed or retained discarded storage");
        if (mode == 1 || mode == 2)
          require(failure.kind == TaskFailure::Cancelled && (mode != 2 || cancelledRunning),
                  "host array cancellation became a fault or published success");
        if (mode >= 3 && failure.kind != TaskFailure::None)
          require(failure.kind == TaskFailure::AdmissionRejected ||
                      (failure.kind == TaskFailure::Fault && failure.code == 2),
                  "allocation failure changed failure precedence");
        require(core->close(group) && core->drain() && core->release(group),
                "group cleanup failed");
        const auto stats = core->stats();
        require(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.groups &&
                    !stats.unfinished,
                "host array frame leaked scheduler resources");
        output.settle();
        require(!output.live() && output.allocations() == output.releases(), "observation leaked");
      }
    }
    std::cout << "Mixed host arrays: workers 1/4, bounded live storage, cached aliases, "
                 "64 allocation failures, pre/running cancellation and cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
