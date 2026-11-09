#include "../runtime/Console.h"
#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
using namespace kalandra::runtime;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void settle(Invocation &ctx) {
  ctx.settle();
  require(!ctx.live() && ctx.allocations() == ctx.releases(), "console backing leaked");
}
static void describe(const ConsoleResult &result) {
  std::cout << unsigned(result.kind) << ':' << result.code << ':';
  constexpr char hex[] = "0123456789abcdef";
  for (std::int64_t i = 0; i < result.text.byteLength; ++i) {
    const auto byte = static_cast<unsigned char>(result.text.data[i]);
    std::cout << hex[byte >> 4] << hex[byte & 15];
  }
  std::cout << '\n' << std::flush;
}
namespace {
struct ScheduledInput {
  std::atomic<unsigned> *started, *destroyed;
  bool fail;
};
struct ScheduledFrame {
  ScheduledInput input;
  void *state = nullptr;
};
bool copyInteger(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<int *>(out) = *static_cast<const int *>(in);
  return true;
}
void destroyInteger(void *) noexcept {}
const ValueLayout integerLayout{29001, sizeof(int), alignof(int), copyInteger, destroyInteger};
bool constructScheduled(Invocation &, void *out, const Packet &packet) {
  new (out) ScheduledFrame{*static_cast<const ScheduledInput *>(packet.data)};
  return true;
}
void destroyScheduled(void *memory) noexcept {
  auto &frame = *static_cast<ScheduledFrame *>(memory);
  frame.input.destroyed->fetch_add(1);
  frame.~ScheduledFrame();
}
void resumeScheduled(Task &task) {
  auto &frame = *static_cast<ScheduledFrame *>(task.frame());
  if (frame.input.fail) {
    task.fault(73);
    return;
  }
  ConsoleResult out;
  const bool done = kalandra_console_read_line_step_v1(&task.context(), &out, 100, &frame.state);
  if (!done) {
    frame.input.started->store(1);
    kalandra_console_pause_v1(&task, frame.state);
  } else if (task.context().poll() == 0) {
    // The harness holds an empty pipe open, so a successful read is a failure.
    task.fault(99);
  }
}
const TaskEntry scheduledEntry{1,
                               29002,
                               sizeof(ScheduledInput),
                               alignof(ScheduledInput),
                               sizeof(ScheduledFrame),
                               alignof(ScheduledFrame),
                               constructScheduled,
                               destroyScheduled,
                               resumeScheduled,
                               &integerLayout};
void scheduled(unsigned workers) {
  TaskLimits limits;
  limits.workers = workers;
  auto core = TaskCore::create(limits);
  require(bool(core), "console scheduler creation failed");
  std::atomic<unsigned> started{0}, destroyed{0};
  auto group = core->openRoot();
  ScheduledInput input{&started, &destroyed, false};
  const auto reader = core->submit(group, scheduledEntry, {29002, &input, sizeof(input)});
  require(bool(reader) && core->step() && started.load(), "console reader did not suspend");
  input.fail = true;
  const auto failing = core->submit(group, scheduledEntry, {29002, &input, sizeof(input)});
  require(bool(failing), "console sibling admission failed");
  require(core->close(group) && core->drain(), "console scheduler did not drain");
  require(core->settled(reader) && core->settled(failing) && destroyed.load() == 2,
          "console task frames were retained by blocked OS input");
  require(core->failure(group).kind == TaskFailure::Fault && core->failure(group).code == 73,
          "console cancellation replaced the sibling's primary fault");
  require(core->release(group), "console group release failed");
  const auto stats = core->stats();
  require(!stats.records && !stats.groups && !stats.retainedBytes && !stats.unfinished &&
              !stats.externalPending,
          "console scheduler resources leaked");
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc >= 2, "expected mode");
    const std::string mode = argv[1];
    if (mode == "scheduled") {
      require(argc == 3, "expected scheduler worker count");
      scheduled(std::stoul(argv[2]));
      std::cout << "cancelled\n";
    } else if (mode == "read") {
      require(argc >= 4, "expected read limit and count");
      for (int i = 0; i < std::stoi(argv[3]); ++i) {
        Invocation ctx;
        ConsoleResult out;
        if (argc > 4)
          ctx.failAllocationAt(std::stoull(argv[4]));
        kalandra_console_read_line_v1(&ctx, &out, std::stoull(argv[2]));
        if (ctx.status()) {
          require(!out.text.data, "failed console published text");
          std::cout << "fault:" << ctx.status() << '\n';
        } else {
          require(!out.text.byteLength || validateString(ctx, out.text), "console text invalid");
          describe(out);
        }
        settle(ctx);
      }
    } else if (mode == "write" || mode == "cancel-write" || mode == "broken") {
      Invocation ctx;
      ConsoleResult out;
      StringDescriptor text;
      const auto bytes =
          mode == "cancel-write" ? std::string(8 * 1024 * 1024, 'x') : std::string("α\0é", 5);
      kalandra_string_literal_v1(&ctx, &text, bytes.data(), bytes.size());
      std::atomic<bool> stop{false};
      std::thread cancel;
      if (mode == "cancel-write") {
        ctx.bindCancellation(&stop);
      }
      if (mode == "cancel-write") {
        void *state = nullptr;
        while (!kalandra_console_write_step_v1(&ctx, &out, &text, 1, true, &state)) {
          // Start the cancellation timer after validation and submission, so
          // slow sanitizer validation cannot masquerade as blocked OS I/O.
          if (!cancel.joinable())
            cancel = std::thread([&] {
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
              stop.store(true);
            });
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } else
        kalandra_console_write_v1(&ctx, &out, &text, 1, true);
      if (cancel.joinable())
        cancel.join();
      if (mode == "broken") {
        require(!ctx.status() && out.kind == ConsoleKind::System && out.code &&
                    validateString(ctx, out.text),
                "broken pipe not a typed I/O error");
        std::cerr << "system-error\n";
      } else if (mode == "cancel-write") {
        require(ctx.poll() != 0 && !ctx.status(), "blocked write did not cancel");
        settle(ctx);
        std::cerr << "cancelled\n" << std::flush;
        Invocation next;
        StringDescriptor end;
        kalandra_string_literal_v1(&next, &end, "END", 3);
        kalandra_console_write_v1(&next, &out, &end, 1, true);
        require(!next.status() && out.kind == ConsoleKind::Success,
                "write after cancellation failed");
        settle(next);
      } else
        require(!ctx.status() && out.kind == ConsoleKind::Success, "console write failed");
      settle(ctx);
    } else if (mode == "unbound-reader") {
      std::atomic<bool> started{false};
      Invocation unbound;
      ConsoleResult first;
      std::thread reader([&] {
        started.store(true);
        kalandra_console_read_line_v1(&unbound, &first, 100);
      });
      while (!started.load())
        std::this_thread::yield();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      Invocation bound;
      ConsoleResult out;
      std::atomic<bool> stop{false};
      bound.bindCancellation(&stop);
      std::thread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop.store(true);
      });
      kalandra_console_read_line_v1(&bound, &out, 100);
      cancel.join();
      require(bound.poll() != 0 && !bound.status() && !out.text.data,
              "reader waiting for unbound I/O did not cancel");
      settle(bound);
      std::cout << "cancelled\n" << std::flush;
      reader.join();
      require(!unbound.status() && first.kind == ConsoleKind::Success &&
                  validateString(unbound, first.text),
              "unbound read failed after another caller cancelled");
      describe(first);
      settle(unbound);
    } else if (mode == "cancel-read") {
      Invocation ctx;
      ConsoleResult out;
      std::atomic<bool> stop{false};
      ctx.bindCancellation(&stop);
      std::thread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop.store(true);
      });
      kalandra_console_read_line_v1(&ctx, &out, 100);
      cancel.join();
      require(ctx.poll() != 0 && !ctx.status() && !out.text.data, "blocked read did not cancel");
      settle(ctx);
      std::cout << "cancelled\n" << std::flush;
      Invocation next;
      kalandra_console_read_line_v1(&next, &out, 100);
      require(!next.status() && out.kind == ConsoleKind::Success && validateString(next, out.text),
              "read after cancellation failed");
      describe(out);
      settle(next);
    } else if (mode == "precedence") {
      Invocation ctx;
      ConsoleResult out;
      std::atomic<bool> stop{true};
      ctx.bindCancellation(&stop);
      ctx.fault(7);
      kalandra_console_read_line_v1(&ctx, &out, UINT64_MAX);
      StringDescriptor text;
      kalandra_console_write_v1(&ctx, &out, &text, 99, false);
      require(ctx.status() == 7 && !ctx.allocations() && !out.text.data,
              "I/O changed fault precedence");
      settle(ctx);
    } else
      throw std::runtime_error("unknown mode");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
