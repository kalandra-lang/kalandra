#include "../runtime/NumericStorage.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Data {
  std::uint8_t byte;
};
bool construct(Invocation &, void *out, const Packet &packet) {
  new (out) Data(*static_cast<const Data *>(packet.data));
  return true;
}
void destroy(void *out) noexcept { static_cast<Data *>(out)->~Data(); }
void resume(Task &task) {
  // Deliberately supply the raw byte to exercise validation without ever
  // constructing a noncanonical C++ bool object in the test itself.
  task.complete(&static_cast<Data *>(task.frame())->byte);
}
const TaskEntry booleanEntry{
    1,         0x279b001, sizeof(Data), alignof(Data),      sizeof(Data), alignof(Data),
    construct, destroy,   resume,       &boolScalarLayout()};
const TaskEntry byteEntry{1,
                          0x279b002,
                          sizeof(Data),
                          alignof(Data),
                          sizeof(Data),
                          alignof(Data),
                          construct,
                          destroy,
                          resume,
                          numericScalarLayout(NumericType::UInt8)};
} // namespace
int main() {
  try {
    check(boolScalarLayout().identity != numericScalarLayout(NumericType::UInt8)->identity,
          "Bool and UInt8 share task identity");
    for (auto byte : {0u, 1u, 2u, 255u}) {
      Invocation destination, source;
      bool output = true;
      const auto raw = std::uint8_t(byte);
      const auto copied = boolScalarLayout().copy(destination, &output, source, &raw);
      check(copied == (byte <= 1), "Bool layout accepted invalid byte or rejected canonical byte");
      check(byte <= 1 ? output == (byte != 0) : output && destination.status() == 3,
            "invalid Bool copy published a value or lost Invalid fault");
    }
    for (unsigned workers : {1u, 4u}) {
      for (auto raw : {0u, 1u, 2u, 255u}) {
        auto core = TaskCore::create({.workers = workers});
        check(bool(core), "create core");
        auto group = core->openRoot();
        const Data value{std::uint8_t(raw)};
        auto boolean =
            core->submit(group, booleanEntry, {booleanEntry.packetLayout, &value, sizeof(value)});
        auto numeric =
            core->submit(group, byteEntry, {byteEntry.packetLayout, &value, sizeof(value)});
        check(boolean && numeric && core->close(group) && core->drain(), "execute scalar tasks");
        if (raw > 1) {
          check(core->failure(boolean).kind == TaskFailure::Fault &&
                    core->failure(boolean).code == 3,
                "malformed Bool completion did not produce Invalid");
          core->cancel(group);
          check(core->failure(group).kind == TaskFailure::Fault && core->failure(group).code == 3,
                "late cancellation replaced recorded Bool fault");
        } else {
          Invocation wrong;
          std::uint8_t byte = 99;
          check(!core->observe(boolean, *numericScalarLayout(NumericType::UInt8), wrong, &byte) &&
                    byte == 99 && wrong.status() == 3,
                "Bool was observed using UInt8 layout");
          for (unsigned repeat = 0; repeat < 3; ++repeat) {
            Invocation context;
            bool result = false;
            check(core->observe(boolean, boolScalarLayout(), context, &result) &&
                      result == (raw != 0),
                  "cached Bool observation failed");
          }
          Invocation wrongBool;
          bool result = true;
          check(!core->observe(numeric, boolScalarLayout(), wrongBool, &result) && result,
                "UInt8 was observed using Bool layout");
          Invocation context;
          check(core->observe(numeric, *numericScalarLayout(NumericType::UInt8), context, &byte) &&
                    byte == raw,
                "new Bool identity broke numeric UInt8 task");
        }
        check(core->release(group), "release group");
        auto stats = core->stats();
        check(!stats.records && !stats.groups && !stats.externalPending && !stats.retainedBytes,
              "Bool completion retained task resources");
      }
      auto core = TaskCore::create({.workers = workers});
      auto group = core->openRoot();
      const Data value{1};
      auto task =
          core->submit(group, booleanEntry, {booleanEntry.packetLayout, &value, sizeof(value)});
      check(task && core->close(group) && core->cancel(group) && core->drain() &&
                core->failure(task).kind == TaskFailure::Cancelled && core->release(group),
            "Bool task cancellation was lost");
    }
    std::cout << "PASS Bool task identity, canonical bytes, forged layouts, repeat reads, faults "
                 "and cancellation\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
