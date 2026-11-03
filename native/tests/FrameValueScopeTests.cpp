#include "../runtime/Tasks.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Frame scope assertion %d\n", __LINE__);                                \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
extern "C" bool task_scope_copy_function(Invocation *, void *, const Invocation *, const void *);
extern "C" bool task_scope_copy_function_record(Invocation *, void *, const Invocation *,
                                                const void *);
extern "C" bool task_copy_plain_function(Invocation *, void *, const Invocation *, const void *);
extern "C" std::int64_t kalandra_callable_1(void *, std::int64_t, Invocation *);
struct Function {
  const void *code, *environment;
};
struct FunctionRecord {
  std::int64_t tag;
  Function function;
};
template <class T> struct Value {
  Failure failure;
  T payload;
};
int main() {
  static_assert(sizeof(Value<Function>) == 48 && sizeof(Value<FunctionRecord>) == 56);
  for (unsigned kind = 1; kind <= 4; ++kind) {
    Invocation source, destination, recordDestination, strictDestination;
    destination.failAllocationAt(1);
    recordDestination.failAllocationAt(1);
    Value<Function> input, output;
    std::memset(&input, 0xa5, sizeof(input));
    std::memset(&output, 0x59, sizeof(output));
    input.failure = {static_cast<TaskFailure>(kind), 4, 7, 9};
    CHECK(task_scope_copy_function(&destination, &output, &source, &input));
    CHECK(output.failure.kind == input.failure.kind && output.failure.code == 4 &&
          output.failure.line == 7 && output.failure.column == 9);
    CHECK(!output.payload.code && !output.payload.environment && !destination.live() &&
          !destination.status());
    CHECK(!task_copy_plain_function(&strictDestination, &output, &source, &input));
    CHECK(!strictDestination.live() && !strictDestination.callDepth());
    Value<FunctionRecord> record, recordOutput;
    std::memset(&record, 0xa5, sizeof(record));
    record.failure = input.failure;
    CHECK(task_scope_copy_function_record(&recordDestination, &recordOutput, &source, &record));
    CHECK(recordOutput.failure.kind == input.failure.kind && !recordOutput.payload.tag &&
          !recordOutput.payload.function.code && !recordOutput.payload.function.environment);
    CHECK(!recordDestination.live() && !recordDestination.status() &&
          !recordDestination.callDepth());
  }
  {
    Invocation source, destination;
    Value<Function> input{{}, {reinterpret_cast<const void *>(&kalandra_callable_1), nullptr}},
        output{};
    CHECK(task_scope_copy_function(&destination, &output, &source, &input));
    CHECK(output.payload.code == input.payload.code && !output.payload.environment &&
          !destination.callDepth());
  }
  {
    Invocation source, destination;
    Value<Function> input{{static_cast<TaskFailure>(99), 0, 0, 0}, {}}, output{};
    CHECK(!task_scope_copy_function(&destination, &output, &source, &input));
    destination.fault(5, 11, 12);
    input.failure.kind = TaskFailure::Cancelled;
    CHECK(!task_scope_copy_function(&destination, &output, &source, &input) &&
          destination.status() == 5);
  }
  std::puts("Frame error envelopes skip absent function payloads; plain transport remains strict");
}
