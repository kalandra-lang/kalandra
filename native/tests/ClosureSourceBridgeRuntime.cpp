#include "../runtime/Runtime.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using kalandra::runtime::Invocation;
struct Carrier { const void *code, *environment; };
extern "C" void fixture_factory(Carrier *, std::int64_t, std::int64_t, Invocation *);
extern "C" std::uint64_t fixture_identify(const Carrier *, const void **);
extern "C" std::uint64_t fixture_captured_tag(), fixture_named_tag(), fixture_environment_bytes(),
    fixture_environment_alignment(), fixture_offset_field();
static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (auto offset : {INT64_C(-17), INT64_C(0), INT64_C(4294967301)}) {
      Invocation context;
      Carrier carrier{};
      fixture_factory(&carrier, offset, 1, &context);
      require(!context.status(), "source closure factory faulted");
      const void *environment = nullptr;
      require(fixture_identify(&carrier, &environment) == fixture_captured_tag(),
              "source captured closure target identity mismatched");
      const auto *data = static_cast<const std::byte *>(context.readEnvironment(
          environment, fixture_environment_bytes(), fixture_environment_alignment(), carrier.code));
      require(data != nullptr, "source environment ownership/schema mismatch");
      std::int64_t captured = 0;
      std::memcpy(&captured, data + fixture_offset_field(), sizeof(captured));
      require(captured == offset, "source closure capture changed after factory returned");
      fixture_factory(&carrier, offset, 0, &context);
      require(fixture_identify(&carrier, &environment) == fixture_named_tag() && !environment,
              "source named function acquired a capture environment");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "source closure extraction leaked ownership");
    }
    Invocation failed;
    failed.failAllocationAt(1);
    Carrier partial{};
    fixture_factory(&partial, 42, 1, &failed);
    require(failed.status() == 2, "source factory did not preserve allocation failure");
    failed.settle();
    require(!failed.live() && failed.allocations() == failed.releases(), "failed factory leaked");
    Invocation stopped;
    std::atomic<bool> cancel{true};
    stopped.bindCancellation(&cancel);
    fixture_factory(&partial, 42, 1, &stopped);
    require(!stopped.status() && !stopped.allocations(), "cancelled factory allocated captures");
    std::cout << "source-generated closure identity, capture bytes and cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
