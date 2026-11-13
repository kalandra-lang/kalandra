#include "../runtime/GeneratorTasks.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace kalandra::runtime;
extern "C" const TaskEntry *const kalandra_task_test_entry;
// The test exports the compiler's actual next-result layout from its emitted
// module. No production schema identity or Option representation is guessed.
extern "C" const ValueLayout kalandra_iterator_dispatch_layout;
#define CHECK(value)                                                                               \
  do {                                                                                             \
    if (!(value)) {                                                                                \
      std::fprintf(stderr, "iterator dispatch assertion %d: %s\n", __LINE__, #value);              \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
namespace {
struct OptionResult {
  Failure failure;
  std::uint64_t tag = 1;
  std::int64_t value = 0;
};
struct Answer {
  Failure failure;
  std::int64_t value = 0;
};
struct Probe {
  std::atomic<unsigned> produced{0}, destroyed{0}, childDestroyed{0};
  std::atomic<bool> externalReady{false};
  ExternalCompletion completion;
  std::int64_t limit = 0;
  bool fault = false;
};
void nothing(void *) noexcept {}
bool copyAnswer(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  std::memcpy(out, in, sizeof(Answer));
  return true;
}
const ValueLayout answerLayout{0x49544552, sizeof(Answer), alignof(Answer), copyAnswer, nothing};
bool constructChild(Invocation &, void *out, const Packet &packet) {
  new (out) Probe *(*static_cast<Probe *const *>(packet.data));
  return true;
}
void destroyChild(void *out) noexcept { ++(*static_cast<Probe **>(out))->childDestroyed; }
void resumeChild(Task &task) {
  auto &probe = **static_cast<Probe **>(task.frame());
  probe.completion = task.suspendExternal();
  CHECK(probe.completion);
  probe.externalReady.store(true, std::memory_order_release);
}
const TaskEntry child{1,
                      0,
                      sizeof(Probe *),
                      alignof(Probe *),
                      sizeof(Probe *),
                      alignof(Probe *),
                      constructChild,
                      destroyChild,
                      resumeChild,
                      &answerLayout};
struct Producer {
  Probe *probe;
  unsigned index = 0;
};
bool constructProducer(Invocation &, void *out, const Packet &packet) {
  new (out) Producer{*static_cast<Probe *const *>(packet.data)};
  return true;
}
void destroyProducer(void *out) noexcept {
  auto &producer = *static_cast<Producer *>(out);
  CHECK(producer.probe->childDestroyed == unsigned(producer.index != 0));
  ++producer.probe->destroyed;
  producer.~Producer();
}
void resumeProducer(Task &task) {
  auto &producer = *static_cast<Producer *>(task.frame());
  if (!producer.index) {
    const auto group = task.openGroup();
    auto *input = producer.probe;
    CHECK(group && task.spawn(group, child, {0, &input, sizeof(input), &task.context()}));
    CHECK(task.close(group));
  }
  if (producer.index == 1 && producer.probe->fault) {
    task.context().fault(9, 37, 4);
    return;
  }
  OptionResult result;
  if (producer.index != 3) {
    result.tag = 2;
    result.value = 10 + producer.index++;
    ++producer.probe->produced;
  }
  task.complete(&result);
}
const TaskEntry body{1,
                     0,
                     sizeof(Probe *),
                     alignof(Probe *),
                     sizeof(Producer),
                     alignof(Producer),
                     constructProducer,
                     destroyProducer,
                     resumeProducer,
                     &kalandra_iterator_dispatch_layout};
GeneratorStep classify(const void *value) noexcept {
  return static_cast<const OptionResult *>(value)->tag == 2 ? GeneratorStep::Item
                                                            : GeneratorStep::Exhausted;
}
void exhausted(void *value) noexcept { new (value) OptionResult; }
const GeneratorTaskEntry generator{1, &body, classify, exhausted};
struct CursorArgument {
  Failure failure;
  GeneratorDescriptor cursor;
};
struct Arguments {
  CursorArgument cursor;
  Answer limit;
};
struct Consumer {
  Probe *probe;
  Arguments arguments;
  Answer answer;
  bool created = false;
};
bool constructConsumer(Invocation &, void *out, const Packet &packet) {
  new (out) Consumer{*static_cast<Probe *const *>(packet.data), {}, {}};
  return true;
}
void destroyConsumer(void *out) noexcept { static_cast<Consumer *>(out)->~Consumer(); }
void resumeConsumer(Task &task) {
  auto &consumer = *static_cast<Consumer *>(task.frame());
  if (!consumer.created) {
    auto *input = consumer.probe;
    consumer.arguments.cursor.cursor =
        generatorTaskCreate(task, generator, {0, &input, sizeof(input), &task.context()});
    if (task.context().poll())
      return;
    CHECK(consumer.probe->produced == 0);
    consumer.arguments.limit.value = consumer.probe->limit;
    consumer.created = true;
  }
  const auto &entry = *kalandra_task_test_entry;
  CHECK(entry.packetBytes == sizeof(Arguments) && entry.result->bytes == sizeof(Answer));
  if (!task.call(entry,
                 {entry.packetLayout, &consumer.arguments, sizeof(Arguments), &task.context()},
                 &consumer.answer))
    return;
  task.complete(&consumer.answer);
}
const TaskEntry consumer{1,
                         0,
                         sizeof(Probe *),
                         alignof(Probe *),
                         sizeof(Consumer),
                         alignof(Consumer),
                         constructConsumer,
                         destroyConsumer,
                         resumeConsumer,
                         &answerLayout};
} // namespace
int main() {
  CHECK(kalandra_iterator_dispatch_layout.bytes == sizeof(OptionResult));
  for (unsigned workers : {1u, 4u})
    for (std::int64_t limit : {0, 2, 10})
      for (bool fault : {false, true}) {
        TaskLimits limits;
        limits.workers = workers;
        limits.records = 2; // only consumer plus its real child, never a cursor task
        limits.groups = 2;
        limits.ready = 1;
        auto core = TaskCore::create(limits);
        CHECK(core);
        const auto group = core->openRoot();
        Probe probe;
        probe.limit = limit;
        probe.fault = fault;
        auto *input = &probe;
        const auto task = core->submit(group, consumer, {0, &input, sizeof(input)});
        CHECK(task && core->close(group));
        const unsigned count = limit == 0 ? 0 : fault ? 1 : limit == 2 ? 2 : 3;
        if (limit) {
          for (unsigned n = 0;
               n != 100000 &&
               !(probe.externalReady.load(std::memory_order_acquire) && probe.produced == count);
               ++n)
            core->step();
          CHECK(probe.externalReady && probe.produced == count);
          for (unsigned n = 0; n != 128; ++n)
            core->step();
          CHECK(!core->settled(task) && probe.destroyed == 0 && probe.childDestroyed == 0);
          CHECK(probe.completion.signal());
        }
        CHECK(core->drainToCompletion() && probe.destroyed == 1 && probe.produced == count);
        const auto failure = core->failure(task);
        if (fault && limit)
          CHECK(failure.kind == TaskFailure::Fault && failure.code == 9 && failure.line == 37 &&
                failure.column == 4);
        else {
          Invocation receiver;
          Answer answer;
          CHECK(failure.kind == TaskFailure::None &&
                core->observe(task, answerLayout, receiver, &answer));
          CHECK(answer.failure.kind == TaskFailure::None && answer.value == (limit == 0   ? 0
                                                                             : limit == 2 ? 21
                                                                                          : 33));
        }
        CHECK(core->release(group));
        const auto stats = core->stats();
        CHECK(!stats.records && !stats.groups && !stats.unfinished && !stats.retainedBytes &&
              !stats.callBytes && !stats.externalPending);
      }
  std::puts("Generated iterator dispatch: partial close, exhaustion, unstarted drop, faults and "
            "ACK barriers passed");
}
