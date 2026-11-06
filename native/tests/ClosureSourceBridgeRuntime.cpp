#include "../runtime/Runtime.h"
#include "../runtime/DeviceGraphCapture.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using kalandra::runtime::Invocation;
using namespace kalandra::runtime;
struct Carrier { const void *code, *environment; };
extern "C" void fixture_factory(Carrier *, std::int64_t, std::int64_t, Invocation *);
extern "C" std::uint64_t fixture_identify(const Carrier *, const void **);
extern "C" std::uint64_t fixture_captured_tag(), fixture_named_tag(), fixture_environment_bytes(),
    fixture_environment_alignment(), fixture_offset_field();
static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
static std::expected<std::uint64_t, DeviceGraphValidation>
targetTag(std::span<const std::byte> field) {
  if (field.size() != sizeof(Carrier))
    return std::unexpected(DeviceGraphValidation::Invalid);
  Carrier carrier{};
  std::memcpy(&carrier, field.data(), sizeof(carrier));
  const void *environment = nullptr;
  const auto tag = fixture_identify(&carrier, &environment);
  if (!tag)
    return std::unexpected(DeviceGraphValidation::Invalid);
  return tag;
}
static void packClosure(Invocation &context, Carrier carrier, std::int64_t captured) {
  const auto tag = fixture_captured_tag();
  const std::array guard{DeviceGraphGuard{0, tag}};
  const std::array rootCells{
      DeviceGraphNativeCell{{UINT64_MAX, 0, false, 1}, 0, sizeof(Carrier), targetTag},
      DeviceGraphNativeCell{{0, 2, false, 0, guard}, offsetof(Carrier, environment), sizeof(void *)}};
  const std::array environmentCells{DeviceGraphNativeCell{{UINT64_MAX}, fixture_offset_field(), 8}};
  const std::array nativeLayouts{
      DeviceGraphNativeLayout{rootCells, sizeof(Carrier), true},
      DeviceGraphNativeLayout{environmentCells, fixture_environment_bytes(), true}};
  const std::array captureLayouts{
      DeviceGraphCaptureLayout{sizeof(Carrier), alignof(Carrier), nullptr},
      DeviceGraphCaptureLayout{fixture_environment_bytes(), fixture_environment_alignment(), nullptr,
                               carrier.code}};
  DeviceGraphCapture resolver(context, captureLayouts);
  const auto pack = [&] {
    return packDeviceGraph({1, 1, std::as_bytes(std::span(&carrier, 1))}, nativeLayouts,
                            100, 100, std::ref(resolver), [] { return false; });
  };
  const auto packed = pack();
  const std::vector<std::uint64_t> expected = carrier.environment
      ? std::vector<std::uint64_t>{DeviceGraphWireABI::Magic, 2, 1, 1, 1, 2, 1, tag, 2,
                                    static_cast<std::uint64_t>(captured)}
      : std::vector<std::uint64_t>{DeviceGraphWireABI::Magic, 1, 1, 1, 1, fixture_named_tag(), 0};
  require(packed && *packed == expected, "source closure did not pack to canonical target/capture cells");
  // The resolver's lease keeps the payload alive independently of its creator.
  context.settle();
  require(pack() == packed, "source closure graph lost its retained environment after settlement");
  carrier.code = &context;
  require(!pack(), "unknown native code identity entered the device graph");
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
      packClosure(context, carrier, offset);
      // A fresh invocation exercises the capture-free target after settlement.
      Invocation named;
      fixture_factory(&carrier, offset, 0, &named);
      require(fixture_identify(&carrier, &environment) == fixture_named_tag() && !environment,
              "source named function acquired a capture environment");
      packClosure(named, carrier, 0);
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
