#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace kalandra::runtime;
extern "C" {
extern const TaskEntry *const kalandra_aggregate_test_array;
extern const TaskEntry *const kalandra_aggregate_test_enum;
extern const TaskEntry *const kalandra_aggregate_test_captured;
extern const TaskEntry *const kalandra_aggregate_test_words;
extern const TaskEntry *const kalandra_aggregate_test_apply;
extern void *const kalandra_aggregate_test_code;
}
struct Header {
  std::int32_t kind = 0;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Scalar {
  Header header;
  std::int64_t value = 21;
};
struct Payload {
  void *code = nullptr, *environment = nullptr;
};
struct Descriptor {
  void *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0, count = 0, stride = 0;
};
struct Array {
  Header header;
  Descriptor value;
};
struct Enum {
  Header header;
  std::int64_t tag = 1;
  Payload callback;
};
struct Function {
  Header header;
  Payload value;
};
static_assert(sizeof(Array) == 72 && sizeof(Enum) == 56 && sizeof(Payload) == 16 &&
              sizeof(Descriptor) == 40);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void balanced(Invocation &ctx) {
  require(!ctx.callDepth(), "aggregate admission leaked");
  ctx.settle();
  require(!ctx.live() && !ctx.environmentBytes() && ctx.allocations() == ctx.releases(),
          "aggregate storage/reservations leaked");
}
static Array make(Invocation &ctx) {
  auto *numbers = static_cast<std::int64_t *>(ctx.allocateRecord(8));
  require(numbers, "numbers allocation failed");
  *numbers = 40;
  Descriptor numeric{numbers, numbers, 0, 1, 1};
  void *environment = ctx.allocateEnvironment(40, kalandra_aggregate_test_code);
  require(environment, "environment allocation failed");
  std::memcpy(environment, &numeric, 40);
  auto *cells = static_cast<Payload *>(ctx.allocateRecord(32));
  require(cells, "array allocation failed");
  cells[0] = cells[1] = {kalandra_aggregate_test_code, environment};
  return {{}, {cells, cells, 0, 2, 1}};
}
using Invoke = std::int64_t (*)(void *, std::int64_t, Invocation *);
static std::int64_t invoke(Payload value, Invocation &ctx) {
  return reinterpret_cast<Invoke>(value.code)(value.environment, 2, &ctx);
}
int main() {
  try {
    const auto &entry = *kalandra_aggregate_test_array;
    const auto &layout = *entry.result;
    const auto &enumEntry = *kalandra_aggregate_test_enum;
    require(layout.bytes == sizeof(Array) && enumEntry.result->bytes == sizeof(Enum),
            "private aggregate ABI changed");
    auto parent = std::make_unique<Invocation>();
    auto original = make(*parent);
    std::array<Payload, 2> originalCells;
    std::memcpy(originalCells.data(), original.value.allocated, 32);
    Invocation first, second;
    Array a, b;
    require(layout.copy(first, &a, *parent, &original) &&
                layout.copy(second, &b, *parent, &original),
            "aggregate copy failed");
    require(a.value.allocated != original.value.allocated &&
                a.value.allocated != b.value.allocated && first.environmentBytes() == 112 &&
                second.environmentBytes() == 112,
            "aggregate storage was shared or not charged");
    require(!std::memcmp(original.value.allocated, originalCells.data(), 32),
            "aggregate copy mutated source cells");
    balanced(*parent);
    parent.reset();
    for (unsigned n = 0; n < 128; ++n)
      require(invoke(static_cast<Payload *>(a.value.aligned)[0], first) == 42 && !first.status(),
              "aggregate lost creator lifetime");
    balanced(first);
    require(invoke(static_cast<Payload *>(b.value.aligned)[1], second) == 42 && !second.status(),
            "sibling settlement invalidated aggregate");
    balanced(second);
    Invocation source;
    auto valid = make(source);
    for (unsigned mode = 0; mode < 12; ++mode) {
      Array input = valid;
      Invocation dst;
      std::atomic<bool> cancelled{true};
      if (mode == 0)
        input.header.kind = 1;
      if (mode == 1)
        input.value.allocated = reinterpret_cast<void *>(1);
      if (mode == 2)
        input.value.aligned = static_cast<std::byte *>(input.value.allocated) + 8;
      if (mode == 3)
        input.value.offset = 1;
      if (mode == 4)
        input.value.count = -1;
      if (mode == 5)
        input.value.count = INT64_MAX;
      if (mode == 6)
        input.value.stride = 2;
      if (mode == 7 || mode == 8 || mode == 9) {
        auto *cells = static_cast<Payload *>(source.allocateRecord(32));
        std::memcpy(cells, valid.value.allocated, 32);
        if (mode == 7)
          cells[1].code = reinterpret_cast<void *>(1);
        if (mode == 8)
          cells[1].environment = nullptr;
        if (mode == 9)
          cells[1].environment = source.allocateEnvironment(32, kalandra_aggregate_test_code);
        input.value.allocated = input.value.aligned = cells;
      }
      if (mode == 10)
        dst.fault(4, 8, 9);
      if (mode == 11)
        dst.bindCancellation(&cancelled);
      alignas(Array) std::array<std::byte, sizeof(Array)> out;
      out.fill(std::byte{0x5a});
      const auto before = out;
      std::array<Payload, 2> cellsBefore;
      std::memcpy(cellsBefore.data(), valid.value.allocated, 32);
      require(!layout.copy(dst, out.data(), source, &input) && out == before,
              "bad aggregate published output");
      require(dst.status() == (mode == 10   ? 4
                               : mode == 11 ? 0
                                            : 3),
              "bad aggregate changed primary failure");
      require(!std::memcmp(valid.value.allocated, cellsBefore.data(), 32),
              "failed aggregate copy mutated source");
      balanced(dst);
    }
    for (unsigned attempt = 0; attempt < 7; ++attempt) {
      Invocation dst;
      dst.failAllocationAt(attempt);
      Array out;
      bool ok = layout.copy(dst, &out, source, &valid);
      require(attempt >= 1 && attempt <= 5 ? !ok && dst.status() == 2 : ok && !dst.status(),
              "aggregate allocation failure changed behavior");
      dst.failAllocationAt(0);
      if (ok)
        require(invoke(static_cast<Payload *>(out.value.aligned)[0], dst) == 42,
                "allocated aggregate changed result");
      balanced(dst);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
      Invocation dst;
      Enum input;
      input.tag = mode == 0 ? 1 : mode == 1 ? 2 : 99;
      input.callback = mode == 0 ? Payload{reinterpret_cast<void *>(1), reinterpret_cast<void *>(1)}
                                 : static_cast<Payload *>(valid.value.aligned)[0];
      std::array<std::byte, sizeof(Enum)> before;
      std::memcpy(before.data(), &input, sizeof input);
      Enum out;
      out.tag = 123;
      const auto ok = enumEntry.result->copy(dst, &out, source, &input);
      require(mode < 2 ? ok && !dst.status() : !ok && dst.status() == 3 && out.tag == 123,
              "enum active tag validation failed");
      require(!std::memcmp(&input, before.data(), sizeof input), "enum copy mutated source");
      if (mode == 1)
        require(invoke(out.callback, dst) == 42, "enum closure changed result");
      balanced(dst);
    }
    Invocation oversized;
    auto *large = source.allocateRecord(16 * 65537);
    require(large, "oversized test storage failed");
    Array huge{{}, {large, large, 0, 65537, 1}};
    Array notPublished;
    notPublished.value.count = -9;
    require(!layout.copy(oversized, &notPublished, source, &huge) && oversized.status() == 2 &&
                notPublished.value.count == -9,
            "function-array storage bypassed invocation copy budget");
    balanced(oversized);
    const auto &words = *kalandra_aggregate_test_words;
    require(words.result->identity != layout.identity,
            "function array signatures share a layout identity");
    auto wordCore = TaskCore::create();
    require(bool(wordCore), "word core failed");
    auto wordGroup = wordCore->openRoot();
    Invocation wordSource;
    auto wordTask =
        wordCore->submit(wordGroup, words, {words.packetLayout, nullptr, 0, &wordSource});
    require(bool(wordTask) && wordCore->close(wordGroup) && wordCore->drain(), "word task failed");
    Array wrong;
    require(wordCore->observe(wordTask, *words.result, wordSource, &wrong),
            "word observation failed");
    require(wordCore->release(wordGroup), "word group release failed");
    wordCore.reset();
    Invocation mismatch;
    Array wrongOutput;
    wrongOutput.value.count = -9;
    require(!layout.copy(mismatch, &wrongOutput, wordSource, &wrong) && mismatch.status() == 3 &&
                wrongOutput.value.count == -9,
            "known wrong-signature array target was published");
    balanced(mismatch);
    balanced(wordSource);
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      const auto &apply = *kalandra_aggregate_test_apply;
      auto argumentCore = TaskCore::create(limits);
      require(bool(argumentCore), "argument core failed");
      auto argumentGroup = argumentCore->openRoot();
      auto argumentSource = std::make_unique<Invocation>();
      auto argument = make(*argumentSource);
      std::vector<TaskHandle> argumentTasks;
      for (unsigned n = 0; n < 8; ++n) {
        auto task = argumentCore->submit(
            argumentGroup, apply,
            {apply.packetLayout, &argument, sizeof argument, argumentSource.get()});
        require(bool(task), "aggregate argument rejected");
        argumentTasks.push_back(task);
      }
      require(argumentCore->stats().environmentBytes == 8 * 112,
              "aggregate argument storage not charged");
      balanced(*argumentSource);
      argumentSource.reset();
      require(argumentCore->close(argumentGroup) && argumentCore->drain(),
              "aggregate arguments failed after producer destruction");
      Invocation argumentObserver;
      for (auto task : argumentTasks) {
        Scalar result;
        require(argumentCore->observe(task, *apply.result, argumentObserver, &result) &&
                    result.value == 42 && !result.header.kind,
                "aggregate argument lost ownership");
      }
      require(argumentCore->release(argumentGroup) && !argumentCore->stats().retainedBytes &&
                  !argumentCore->stats().environmentBytes,
              "aggregate argument group leaked");
      balanced(argumentObserver);
      auto cancelCore = TaskCore::create(limits);
      auto cancelGroup = cancelCore->openRoot();
      auto cancelled = cancelCore->submit(cancelGroup, apply,
                                          {apply.packetLayout, &valid, sizeof valid, &source});
      require(bool(cancelled) && cancelCore->cancel(cancelGroup) &&
                  cancelCore->close(cancelGroup) && cancelCore->drain(),
              "aggregate argument cancellation failed");
      require(cancelCore->failure(cancelled).kind == TaskFailure::Cancelled &&
                  cancelCore->release(cancelGroup) && !cancelCore->stats().retainedBytes &&
                  !cancelCore->stats().environmentBytes,
              "cancelled aggregate argument leaked");
      auto core = TaskCore::create(limits);
      require(bool(core), "aggregate core failed");
      auto group = core->openRoot();
      Scalar args;
      Invocation producer;
      std::vector<TaskHandle> tasks;
      for (unsigned n = 0; n < 12; ++n) {
        auto task = core->submit(group, entry, {entry.packetLayout, &args, sizeof args, &producer});
        require(bool(task), "aggregate task rejected");
        tasks.push_back(task);
      }
      balanced(producer);
      require(core->close(group) && core->drain(), "aggregate tasks failed to settle");
      require(core->stats().environmentBytes == 12 * 112, "aggregate cached storage not charged");
      Invocation observer;
      std::vector<Array> values;
      for (auto task : tasks) {
        require(core->failure(task).kind == TaskFailure::None, "aggregate worker failed");
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
          Array out;
          require(core->observe(task, layout, observer, &out), "aggregate observation failed");
          values.push_back(out);
        }
      }
      require(core->release(group), "aggregate group release failed");
      auto stats = core->stats();
      require(!stats.retainedBytes && !stats.environmentBytes && !stats.retainedOwners &&
                  !stats.records && !stats.groups,
              "aggregate core leaked");
      core.reset();
      for (auto value : values) {
        auto *cells = static_cast<Payload *>(value.value.aligned);
        require(invoke(cells[0], observer) + invoke(cells[1], observer) == 42 && !observer.status(),
                "aggregate died with its task/group/core");
      }
      balanced(observer);
      TaskLimits tiny;
      tiny.workers = workers;
      tiny.bytes = entry.frameBytes + entry.result->bytes + 111;
      auto quota = TaskCore::create(tiny);
      require(bool(quota), "aggregate quota core failed");
      auto qgroup = quota->openRoot();
      Invocation qsource;
      auto task = quota->submit(qgroup, entry, {entry.packetLayout, &args, sizeof args, &qsource});
      require(bool(task) && quota->close(qgroup) && quota->drain(),
              "aggregate quota did not settle");
      require(quota->failure(task).kind == TaskFailure::Fault && quota->failure(task).code == 2,
              "aggregate bypassed quota");
      require(quota->release(qgroup) && !quota->stats().retainedBytes &&
                  !quota->stats().environmentBytes,
              "aggregate quota leaked");
      balanced(qsource);
    }
    // Forge a cycle through an array captured by a closure. Copies must stop at
    // checked admission rather than mutate the producer or overflow the stack.
    const auto &captured = *kalandra_aggregate_test_captured;
    auto core = TaskCore::create();
    require(bool(core), "cycle core failed");
    auto group = core->openRoot();
    Invocation src;
    auto task = core->submit(group, captured, {captured.packetLayout, nullptr, 0, &src});
    require(bool(task) && core->close(group) && core->drain(), "cycle producer failed");
    Function cycle;
    require(core->observe(task, *captured.result, src, &cycle), "cycle observation failed");
    require(core->release(group), "cycle group release failed");
    core.reset();
    auto *descriptor = static_cast<Descriptor *>(cycle.value.environment);
    require(descriptor->count == 1, "cycle capture layout changed");
    *static_cast<Payload *>(descriptor->aligned) = cycle.value;
    Invocation denied;
    alignas(Function) std::array<std::byte, sizeof(Function)> sentinel;
    sentinel.fill(std::byte{0x5a});
    const auto before = sentinel;
    require(!captured.result->copy(denied, sentinel.data(), src, &cycle) && denied.status() == 3 &&
                sentinel == before,
            "cyclic aggregate was not rejected by active-path validation");
    balanced(denied);
    balanced(src);
    balanced(source);
    std::cout << "Callable aggregates: 129 post-creator calls; 12 bad/stopped arrays; 7 allocation "
                 "probes; 3 enum tags; 36 tasks/72 observations; 144 post-core calls; 3 quotas; "
                 "cyclic capture admission passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
