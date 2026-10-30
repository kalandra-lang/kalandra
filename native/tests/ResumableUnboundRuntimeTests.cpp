#include "../runtime/Tasks.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" {
extern const TaskEntry *const kalandra_unbound_entry;
extern void *const kalandra_unbound_code;
}
struct Header {
  std::int32_t kind = 0;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Scalar {
  Header header;
  std::int64_t value = 0;
};
struct Callable {
  Header header;
  void *code = nullptr, *environment = nullptr;
};
struct Arguments {
  Callable callback;
  Scalar value;
};
static_assert(sizeof(Arguments) == 88);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    const auto &entry = *kalandra_unbound_entry;
    require(entry.packetBytes == sizeof(Arguments), "unbound probe packet changed");
    for (unsigned workers : {1, 2, 4}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "unbound probe core unavailable");
      auto group = core->openRoot();
      Invocation source, observer;
      Arguments arguments;
      arguments.callback.code = kalandra_unbound_code;
      arguments.value.value = 21;
      auto task =
          core->submit(group, entry, {entry.packetLayout, &arguments, sizeof(arguments), &source});
      require(bool(task) && core->close(group) && core->drain(), "unbound entry did not settle");
      const auto failure = core->failure(task);
      require(failure.kind == TaskFailure::Fault && failure.code == 3,
              "unbound generic entry executed an unproven coordinating target");
      Scalar output;
      output.value = 123;
      require(!core->observe(task, *entry.result, observer, &output) && output.value == 123,
              "unbound rejection published a success value");
      require(core->release(group), "unbound group release failed");
      const auto stats = core->stats();
      require(!stats.records && !stats.groups && !stats.unfinished && !stats.retainedBytes &&
                  !stats.environmentBytes,
              "unbound rejection leaked scheduler reservations");
      source.settle();
      observer.settle();
      require(!source.live() && !observer.live() && source.allocations() == source.releases() &&
                  observer.allocations() == observer.releases(),
              "unbound rejection leaked ownership");
    }
    std::cout << "Unbound coordinating entry: checked rejection and cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
