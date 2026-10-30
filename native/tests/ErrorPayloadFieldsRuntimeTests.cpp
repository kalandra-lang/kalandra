#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct Box {
  Failure cause;
};
struct Value {
  Failure expected;
  Box data;
};
#ifdef KALANDRA_ERROR_PAYLOAD_PURE
extern "C" void kalandra_payload_probe(Box *, Box *, Invocation *);
#else
extern "C" const TaskEntry *const kalandra_task_test_entry;
#endif
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static bool same(Failure x, Failure y) {
  return x.kind == y.kind && x.code == y.code && x.line == y.line && x.column == y.column;
}
int main() {
  try {
    for (unsigned kind = 0; kind <= 5; ++kind) {
      const Failure expected{static_cast<TaskFailure>(kind), 42, 17, 9};
      const bool valid = kind >= 1 && kind <= 4;
      Invocation source, survivor;
#ifdef KALANDRA_ERROR_PAYLOAD_PURE
      Box input{expected}, output{};
      kalandra_payload_probe(&output, &input, &source);
      require(valid ? source.status() == 0 && same(output.cause, expected) : source.status() == 3,
              "synchronous TaskError payload validation changed");
#else
      const auto *entry = kalandra_task_test_entry;
      require(entry && entry->packetBytes == sizeof(Value) && entry->result->bytes == sizeof(Value),
              "TaskError payload layout mismatch");
      Value input{{}, {expected}}, output{};
      auto core = TaskCore::create();
      require(bool(core), "scheduler creation failed");
      auto group = core->openRoot();
      auto task =
          core->submit(group, *entry, {entry->packetLayout, &input, sizeof(input), &source});
      require(task && core->close(group) && core->drain(), "payload task did not settle");
      const auto actual = core->failure(task);
      if ((valid && actual.kind != TaskFailure::None) ||
          (!valid && actual.kind != TaskFailure::Fault))
        std::cerr << "field kind=" << kind << " scheduler kind=" << unsigned(actual.kind)
                  << " code=" << actual.code << '\n';
      require(valid ? actual.kind == TaskFailure::None : actual.kind == TaskFailure::Fault,
              "TaskError field became an expected channel or skipped validation");
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        require(core->observe(task, *entry->result, survivor, &output) == valid,
                "payload observation accepted an invalid field");
        if (valid)
          require(same(output.data.cause, expected), "TaskError payload metadata changed");
      }
      for (unsigned bad : {0U, 5U, 99U}) {
        auto malformed = input;
        malformed.data.cause.kind = static_cast<TaskFailure>(bad);
        alignas(Value) std::array<std::byte, sizeof(Value)> storage;
        storage.fill(std::byte{0x5a});
        const auto before = storage;
        require(!entry->result->copy(survivor, storage.data(), source, &malformed) &&
                    storage == before,
                "invalid TaskError field published a partial value");
      }
      require(core->release(group), "payload group did not release");
      core.reset();
      if (valid)
        require(same(output.data.cause, expected), "payload changed after scheduler teardown");
#endif
      source.settle();
      survivor.settle();
      require(!source.live() && !survivor.live(), "TaskError data retained unexpected storage");
    }
    std::cout << "TaskError fields preserve all four kinds and metadata; invalid fields rejected\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
