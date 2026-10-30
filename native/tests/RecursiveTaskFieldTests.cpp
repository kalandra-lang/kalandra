#include "../runtime/Tasks.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct List {
  std::int64_t tag, head;
  const void *tail;
};
static_assert(sizeof(List) == 24);
extern "C" {
const void *make_nested_list(Invocation *);
const void *make_list(Invocation *);
const void *callable_box(const void *, const void *, std::int64_t, Invocation *);
bool task_copy_tail(Invocation *, const void **, const Invocation *, const List *);
bool task_project_tail(Invocation *, const List *, List *);
bool task_project_head(Invocation *, const List *, std::int64_t *);
bool task_project_tag(Invocation *, const List *, std::int64_t *);
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(const Invocation &context) {
  require(!context.live() && !context.callDepth() && context.allocations() == context.releases(),
          "task field owners leaked");
}
static List fixture(Invocation &context) {
  auto *root = make_nested_list(&context);
  auto retained = context.lease(root);
  require(retained && retained->bytes().size() == sizeof(List), "task field fixture failed");
  List value;
  std::memcpy(&value, retained->bytes().data(), sizeof(value));
  return value;
}
static void verify(Invocation &context, const List &input) {
  std::int64_t head = 0;
  require(task_project_head(&context, &input, &head) && head == 10, "inline task head changed");
  List child{};
  require(task_project_tail(&context, &input, &child) && child.tag == 2 && child.head == 42,
          "managed child projection failed");
  require(task_project_head(&context, &child, &head) && head == 42, "projected child unreadable");
  List end{};
  require(task_project_tail(&context, &child, &end) && end.tag == 1,
          "transitive child was not registered");
}
static bool copyList(Invocation &destination, void *output, const Invocation &source,
                     const void *input) noexcept {
  List value;
  std::memcpy(&value, input, sizeof(value));
  std::int64_t tag = 0;
  if (!task_project_tag(&destination, &value, &tag))
    return false;
  if (tag == 2) {
    const void *copied = nullptr;
    if (!task_copy_tail(&destination, &copied, &source, &value))
      return false;
    value.tail = copied;
  }
  std::memcpy(output, &value, sizeof(value));
  return true;
}
static void nothing(void *) noexcept {}
static const ValueLayout layout{83001, sizeof(List), alignof(List), copyList, nothing};
static bool construct(Invocation &destination, void *frame, const Packet &packet) {
  return packet.source && copyList(destination, frame, *packet.source, packet.data);
}
static void resume(Task &task) {
  const auto &input = *static_cast<List *>(task.frame());
  if (input.tag == 2)
    verify(task.context(), input);
  task.complete(&input);
}
static const TaskEntry entry{
    1,         83002,   sizeof(List), alignof(List), sizeof(List), alignof(List),
    construct, nothing, resume,       &layout};
int main() {
  try {
    for (unsigned mode = 0; mode < 16; ++mode) {
      Invocation source, destination, foreign;
      List input = fixture(source);
      if (mode == 1)
        input.tail = reinterpret_cast<void *>(0x1230);
      if (mode == 2)
        input.tail = nullptr;
      if (mode == 3)
        input.tail = make_list(&foreign);
      if (mode == 4)
        input.tail =
            callable_box(nullptr, nullptr, 1, &source); // Same size, different nominal token.
      if (mode == 5)
        input = {1, 0, reinterpret_cast<void *>(0x1230)};
      if (mode == 6)
        input.tag = 0x100000002;
      if (mode == 7)
        input.tag = 0;
      if (mode == 8)
        input.tag = -1;
      std::atomic<bool> cancelled{mode == 9};
      destination.bindCancellation(&cancelled);
      if (mode == 10)
        destination.fault(5);
      if (mode == 11)
        destination.expectedTaskError(1, 2, 0, 0);
      if (mode == 12)
        destination.expectedApplicationError("Problem", "Invalid");
      if (mode == 13)
        destination.settle();
      if (mode == 14)
        source.fault(5);
      if (mode == 15)
        source.settle();
      const void *output = reinterpret_cast<void *>(0x5a5a);
      const bool ok = task_copy_tail(&destination, &output, &source, &input);
      require(ok == (mode == 0), "invalid task managed field copy admitted");
      if (ok) {
        source.settle();
        require(output == input.tail && destination.live() == 2,
                "task field graph ownership changed");
        verify(destination, input);
      } else {
        require(output == reinterpret_cast<void *>(0x5a5a) && !destination.live(),
                "failed task field copy published output");
        require(mode == 9 ||
                    (mode == 11 || mode == 12 ? destination.hasExpectedError()
                                              : destination.status() == (mode == 10 ? 5 : 3)),
                "task field copy lost error state");
      }
      source.settle();
      destination.settle();
      foreign.settle();
      balanced(source);
      balanced(destination);
      balanced(foreign);
    }
    for (unsigned attempt = 1; attempt <= 4; ++attempt) {
      Invocation source, destination;
      auto input = fixture(source);
      auto observer = source.lease(input.tail);
      const auto refs = observer->referenceCount();
      destination.failAllocationAt(attempt);
      const void *output = reinterpret_cast<void *>(0x5a5a);
      const bool ok = task_copy_tail(&destination, &output, &source, &input);
      require(ok == (attempt == 4), "task field failure injection changed");
      if (!ok)
        require(destination.status() == 2 && !destination.live() &&
                    observer->referenceCount() == refs &&
                    output == reinterpret_cast<void *>(0x5a5a),
                "task field rollback leaked");
      source.settle();
      if (ok)
        verify(destination, input);
      destination.settle();
      require(observer->referenceCount() == 1, "managed field dependency retained after cleanup");
      balanced(source);
      balanced(destination);
    }
    for (unsigned mode = 0; mode < 11; ++mode) {
      Invocation context, foreign;
      auto input = fixture(context);
      if (mode == 1)
        input.tail = reinterpret_cast<void *>(0x1230);
      if (mode == 2)
        input.tail = callable_box(nullptr, nullptr, 1, &context);
      if (mode == 3)
        input = {1, 0, reinterpret_cast<void *>(0x1230)};
      if (mode == 4)
        input.tag = 0x100000002;
      std::atomic<bool> cancelled{mode == 5};
      context.bindCancellation(&cancelled);
      if (mode == 6)
        context.fault(5);
      if (mode == 7)
        context.expectedTaskError(1, 2, 0, 0);
      if (mode == 8)
        context.expectedApplicationError("Problem", "Invalid");
      if (mode == 9)
        context.settle();
      List output;
      std::memset(&output, 0x5a, sizeof(output));
      const auto before = output;
      auto &selected = mode == 10 ? foreign : context;
      const bool ok = task_project_tail(&selected, &input, &output);
      require(ok == (mode == 0), "invalid managed field projection admitted");
      if (!ok)
        require(!std::memcmp(&output, &before, sizeof(output)), "failed projection changed output");
      if (mode == 3 || mode == 4) {
        // The full 64-bit tag is checked before inactive pointer bits can be used.
        require(context.status() == 3, "inactive/high-bit tag bypassed field guard");
      }
      context.settle();
      foreign.settle();
      balanced(context);
      balanced(foreign);
    }
    for (unsigned workers : {1u, 2u, 4u}) {
      for (unsigned mode = 0; mode < 4; ++mode) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        require(bool(core), "task field core creation failed");
        Invocation source, observed, survivor;
        auto input = fixture(source);
        if (mode == 1)
          input = {1, 0, reinterpret_cast<void *>(0x1230)};
        auto group = core->openRoot();
        auto task =
            core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source});
        source.settle();
        if (mode >= 2)
          require(core->cancel(group, mode == 3), "task field cancellation failed");
        require(core->drain(), "task field drain failed");
        if (mode < 2) {
          List first{}, second{};
          require(task && core->failure(task).kind == TaskFailure::None &&
                      core->observe(task, layout, observed, &first),
                  "task field result failed");
          const auto owners = observed.live();
          require(core->observe(task, layout, observed, &second) && observed.live() == owners &&
                      first.tag == second.tag && first.tail == second.tail,
                  "task field repeated observation changed ownership");
          require(core->observe(task, layout, survivor, &second),
                  "independent field result failed");
          require(core->close(group) && core->drain() && core->release(group),
                  "field group cleanup");
          const auto stats = core->stats();
          require(!stats.retainedOwners && !stats.retainedBytes && !stats.records && !stats.groups,
                  "field scheduler leaked");
          core.reset();
          if (!mode)
            verify(survivor, second);
          else {
            require(!survivor.live(), "inactive recursive payload acquired owners");
            std::int64_t value = 99;
            require(!task_project_head(&survivor, &second, &value) && value == 99 &&
                        survivor.status() == 3,
                    "inactive scalar payload projected");
          }
        } else {
          require(core->failure(group).kind ==
                      (mode == 2 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
                  "field cancellation variant changed");
          require(core->close(group) && core->drain() && core->release(group),
                  "cancelled field cleanup");
        }
        observed.settle();
        survivor.settle();
        balanced(source);
        balanced(observed);
        balanced(survivor);
      }
    }
    std::cout << "recursive task fields passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
