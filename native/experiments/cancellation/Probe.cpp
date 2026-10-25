// Restricted Darwin/ARM64 experiment. Not a production task cancellation API.
#include "Control.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <thread>
#include <unistd.h>
#include <vector>
using kalandra::experiment::Control;
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t,
                                                   kalandra::runtime::Invocation *);
extern "C" [[noreturn]] void kalandra_source_abort_v2();
static bool unwindMode = false;
static bool borrowRegister = false;
extern "C" bool kalandra_cancel_borrow_register(bool (*)(void *), void *);
static std::atomic<unsigned> signals{0}, redirects{0};
static Control *state() {
  Control *result;
  asm("mov %0, x28" : "=r"(result));
  return result;
}
extern "C" bool kalandra_source_run_v3(kalandra::runtime::Invocation *, bool (*)(void *),
                                       void *) noexcept;
// Ordinary runtime code may borrow x28 while preserving it across its own call.
// Reinstall the task register at the callback, not merely at the outer entry.
// All frames this adapter adds contain only trivial state.
extern "C" bool kalandra_cancel_source_run_v3(kalandra::runtime::Invocation *invocation,
                                              bool (*callback)(void *), void *arguments) noexcept {
  struct Packet {
    bool (*callback)(void *);
    void *arguments;
    Control *control;
    bool result;
  } packet{callback, arguments, state(), false};
  auto invoke = [](void *opaque) -> bool {
    auto *packet = static_cast<Packet *>(opaque);
    kalandra_cancel_enter(
        [](void *opaque) {
          auto *packet = static_cast<Packet *>(opaque);
          packet->result = packet->callback(packet->arguments);
        },
        packet, packet->control);
    return packet->result;
  };
  struct Adapter {
    bool (*invoke)(void *);
    void *packet;
  } adapter{invoke, &packet};
  return kalandra_source_run_v3(
      invocation,
      [](void *opaque) -> bool {
        auto *adapter = static_cast<Adapter *>(opaque);
        return borrowRegister ? kalandra_cancel_borrow_register(adapter->invoke, adapter->packet)
                              : adapter->invoke(adapter->packet);
      },
      &adapter);
}
extern "C" [[noreturn]] void kalandra_cancel_jump() {
  auto *control = state();
  control->acknowledged.store(1, std::memory_order_release);
  siglongjmp(control->landing, 1);
}
extern "C" [[noreturn]] void kalandra_cancel_unwind() {
  state()->acknowledged.store(1, std::memory_order_release);
  kalandra_source_abort_v2();
}
static void handler(int signal, siginfo_t *info, void *opaque) {
  signals.fetch_add(1, std::memory_order_relaxed);
  auto &thread = static_cast<ucontext_t *>(opaque)->uc_mcontext->__ss;
  auto pc = static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc(thread));
  bool eligible = false;
  for (std::size_t i = 0; i < kalandra_cancel_region_count; ++i) {
    const auto &region = kalandra_cancel_regions[i];
    eligible |= pc >= reinterpret_cast<std::uintptr_t>(region.begin) &&
                pc < reinterpret_cast<std::uintptr_t>(region.end);
  }
  if (!eligible) {
    if (signal != SIGUSR2)
      _exit(120);
    return;
  }
  auto *control = reinterpret_cast<Control *>(thread.__x[28]);
  if (!control || control->acknowledged.load(std::memory_order_acquire) ||
      !control->active.load(std::memory_order_acquire) ||
      !control->requested.load(std::memory_order_acquire))
    return;
  if (signal != SIGUSR2 && info->si_addr != control->page)
    _exit(121);
  if (unwindMode) {
    __darwin_arm_thread_state64_set_lr_fptr(thread, reinterpret_cast<void *>(pc + 4));
    __darwin_arm_thread_state64_set_pc_fptr(thread, &kalandra_cancel_unwind);
  } else {
    __darwin_arm_thread_state64_set_pc_fptr(thread, &kalandra_cancel_jump);
  }
  redirects.fetch_add(1, std::memory_order_relaxed);
}
static void body(void *opaque) {
  auto *control = static_cast<Control *>(opaque);
  control->result =
      _mlir_ciface_kalandra_test_entry(control->seed, control->depth, control->invocation);
}
static void execute(Control *control, bool plain, bool owned) {
  auto &invocation = *control->invocation;
  if (owned) {
    if (!invocation.allocate(4096) || !invocation.allocateRecord(192))
      std::abort();
  }
  invocation.failAllocationAt(control->failAt);
  control->initialDepth = invocation.callDepth();
  kalandra::runtime::InvocationABI::cacheStackBounds(invocation);
  std::uint64_t stackLimit;
  std::memcpy(&stackLimit,
              reinterpret_cast<char *>(&invocation) + kalandra::runtime::InvocationABI::stackLow,
              sizeof(stackLimit));
  control->stackLimit.store(stackLimit, std::memory_order_release);
  control->active.store(1, std::memory_order_release);
  if (plain)
    body(control);
  else if (!sigsetjmp(control->landing, 0)) {
    control->active.store(1, std::memory_order_release);
    kalandra_cancel_enter(body, control, control);
  }
  control->active.store(0, std::memory_order_release);
  while (invocation.callDepth() > control->initialDepth)
    invocation.leaveCall();
  invocation.settle();
  if (invocation.live() || invocation.allocations() != invocation.releases() ||
      invocation.status() != control->expectedStatus) {
    std::fprintf(stderr, "ledger live=%llu allocations=%llu releases=%llu status=%lld depth=%llu\n",
                 (unsigned long long)invocation.live(),
                 (unsigned long long)invocation.allocations(),
                 (unsigned long long)invocation.releases(), (long long)invocation.status(),
                 (unsigned long long)invocation.callDepth());
    std::abort();
  }
  control->finishedNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count(),
                            std::memory_order_release);
  control->done.store(1, std::memory_order_release);
}
int main(int argc, char **argv) {
  if (argc != 6 && argc != 8)
    return 2;
  const std::string mode = argv[1];
  borrowRegister = std::getenv("KALANDRA_PROBE_BORROW_REGISTER") != nullptr;
  const bool plain = mode == "floor" || mode == "current";
  const bool async = mode == "async" || mode == "async-unwind" || mode == "hybrid";
  const bool page = mode == "page";
  unwindMode = mode == "async-unwind";
  const int depth = std::stoi(argv[2]), repetitions = std::stoi(argv[3]);
  const bool cancel = std::stoi(argv[4]), owned = std::stoi(argv[5]);
  struct sigaction action{};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  // Block nested cancellation while the signal trampoline restores context.
  sigaddset(&action.sa_mask, SIGUSR2);
  sigaction(SIGUSR2, &action, nullptr);
  sigaction(SIGSEGV, &action, nullptr);
  sigaction(SIGBUS, &action, nullptr);
  const auto seed =
      std::getenv("KALANDRA_PROBE_SEED") ? std::stoll(std::getenv("KALANDRA_PROBE_SEED")) : 17;
  const auto pageSize = sysconf(_SC_PAGESIZE);
  double checksum = 0;
  unsigned cancelled = 0;
  double maxLatency = 0, totalLatency = 0;
  std::uint64_t allocations = 0;
  std::vector<double> latencies;
  // Warm native code before timing; task-boundary and cleanup costs remain timed.
  if (!cancel)
    for (int warmup = 0; warmup != 20; ++warmup) {
      auto c = std::make_unique<Control>();
      auto inv = std::make_unique<kalandra::runtime::Invocation>();
      c->seed = seed;
      c->depth = depth;
      c->invocation = inv.get();
      if (mode == "current")
        inv->bindCancellation(&c->token);
      if (page)
        c->page = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (argc == 8) {
        c->failAt = std::stoull(argv[6]);
        c->expectedStatus = std::stoll(argv[7]);
      }
      execute(c.get(), plain, owned);
      if (page)
        munmap(c->page, pageSize);
    }
  const auto start = std::chrono::steady_clock::now();
  for (int repeat = 0; repeat < repetitions; ++repeat) {
    auto control = std::make_unique<Control>();
    auto invocation = std::make_unique<kalandra::runtime::Invocation>();
    control->seed = seed;
    control->depth = depth;
    if (argc == 8) {
      control->failAt = std::stoull(argv[6]);
      control->expectedStatus = std::stoll(argv[7]);
    }
    control->invocation = invocation.get();
    if (mode == "current")
      invocation->bindCancellation(&control->token);
    if (page) {
      control->page =
          mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (control->page == MAP_FAILED)
        return 3;
    }
    if (!cancel)
      execute(control.get(), plain, owned);
    else {
      struct Worker {
        Control *control;
        bool plain, owned;
      } packet{control.get(), plain, owned};
      pthread_attr_t attributes;
      pthread_attr_init(&attributes);
      pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
      pthread_t worker;
      if (pthread_create(
              &worker, &attributes,
              [](void *opaque) -> void * {
                auto *packet = static_cast<Worker *>(opaque);
                execute(packet->control, packet->plain, packet->owned);
                return nullptr;
              },
              &packet))
        return 5;
      pthread_attr_destroy(&attributes);
      while (!control->active.load(std::memory_order_acquire) &&
             !control->done.load(std::memory_order_acquire))
        std::this_thread::yield();
      // Let native frames become active, then measure request through cleanup.
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      const auto request = std::chrono::steady_clock::now();
      control->requested.store(1, std::memory_order_release);
      control->token.store(true, std::memory_order_release);
      if (mode == "guard")
        control->stackLimit.store(UINT64_MAX, std::memory_order_release);
      if (page && mprotect(control->page, pageSize, PROT_NONE))
        return 4;
      while (!control->done.load(std::memory_order_acquire)) {
        if (async)
          pthread_kill(worker, SIGUSR2);
        if (std::chrono::steady_clock::now() - request > std::chrono::seconds(2))
          _exit(122);
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      }
      pthread_join(worker, nullptr);
      const auto latency = std::max(
          0.0, std::chrono::duration<double, std::micro>(
                   std::chrono::nanoseconds(control->finishedNs.load(std::memory_order_acquire)) -
                   request.time_since_epoch())
                   .count());
      latencies.push_back(latency);
      totalLatency += latency;
      maxLatency = std::max(maxLatency, latency);
      cancelled += control->acknowledged.load() || (mode == "current" && control->result == 0);
    }
    checksum += control->result;
    allocations += invocation->allocations();
    if (page)
      munmap(control->page, pageSize);
  }
  const auto ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  std::sort(latencies.begin(), latencies.end());
  const auto percentile = [&](double p) {
    return latencies.empty() ? 0.0
                             : latencies[std::min(latencies.size() - 1,
                                                  static_cast<std::size_t>(p * latencies.size()))];
  };
  std::printf("{\"mode\":\"%s\",\"depth\":%d,\"repetitions\":%d,\"ns\":%.0f,"
              "\"checksum\":%.17g,\"cancelled\":%u,\"signals\":%u,\"redirects\":%u,"
              "\"allocations\":%llu,\"p50_cleanup_us\":%.3f,\"p95_cleanup_us\":%.3f,"
              "\"mean_cleanup_us\":%.3f,\"max_cleanup_us\":%.3f}\n",
              mode.c_str(), depth, repetitions, ns, checksum, cancelled, signals.load(),
              redirects.load(), (unsigned long long)allocations, percentile(0.5), percentile(0.95),
              totalLatency / repetitions, maxLatency);
}
