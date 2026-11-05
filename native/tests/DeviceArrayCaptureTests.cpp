#include "../runtime/DeviceArrayWire.h"
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <NumericType Element, bool Boolean = false>
static auto makeCapture(Invocation &context, const CpuTensor<Element, 1> &descriptor,
                        std::uint64_t origin) {
  auto capture = std::make_unique<DeviceArrayCapture<Element, Boolean>>();
  if (!capture->initialize(context, descriptor, origin))
    capture.reset();
  return capture;
}
template <NumericType Element, bool Boolean = false> static void checkType() {
  using Cell = NumericValue<Element>;
  using Capture = DeviceArrayCapture<Element, Boolean>;
  constexpr std::uint64_t count = DeviceArrayWireABI::PageWords * 2 + 3;
  Invocation context;
  auto *data = static_cast<Cell *>(context.allocate((count + 2) * sizeof(Cell)));
  require(data != nullptr, "input allocation failed");
  for (std::uint64_t i = 0; i < count + 2; ++i) {
    // Fill bytes without floating operations: include arbitrary NaN payloads,
    // signed zeros, subnormals, integer endpoints and sign bits.
    std::uint64_t bits = Boolean ? i % 2 : i * UINT64_C(0x9e3779b97f4a7c15);
    std::memcpy(data + i, &bits, sizeof(Cell));
  }
  CpuTensor<Element, 1> descriptor{data, data, 1, {count}, {1}};
  {
    auto capture = makeCapture<Element, Boolean>(context, descriptor, 7);
    require(bool(capture) && !capture->wire(), "capture published before packing");
    require(!capture->advance() && capture->packedElements() == 0, "header step copied payload");
    for (unsigned step = 0; !capture->complete(); ++step) {
      require(step < 4, "paged packing did not finish");
      const auto before = capture->packedElements();
      capture->advance();
      require(capture->packedElements() - before <= DeviceArrayWireABI::PageWords,
              "packing exceeded 64KiB wire page");
    }
    auto wire = *capture->wire();
    require(wire.sizes[0] == count + 4 && wire.aligned[0] == DeviceArrayWireABI::Magic &&
                wire.aligned[1] == DeviceArrayWireABI::layout(Element, Boolean) &&
                wire.aligned[2] == count && wire.aligned[3] == 7,
            "typed wire metadata differs from expected protocol");
    for (std::uint64_t i = 0; i < count; ++i) {
      std::uint64_t expected = 0;
      std::memcpy(&expected, data + i + 1, sizeof(Cell));
      require(wire.aligned[i + 4] == expected, "wire packing changed raw numeric bits or slice");
    }
    require(capture->original().allocated == data && capture->original().offset == 1,
            "capture lost original alias descriptor");
    auto retainedWire = retainCpuTensor(context, wire.view());
    require(bool(retainedWire), "wire could not transfer retained allocation ownership");
    static_assert(!std::is_move_constructible_v<Capture>);
    const auto *address = capture.get();
    auto moved = std::move(capture);
    require(moved.get() == address && moved->complete() && !capture,
            "moving capture owner relocated its registered ownership scope");
    std::uint64_t expectedFirst = 0, retainedFirst = 0;
    std::memcpy(&expectedFirst, data + 1, sizeof(Cell));
    context.release(data);
    std::memcpy(&retainedFirst, moved->original().aligned + 1, sizeof(Cell));
    require(retainedFirst == expectedFirst, "capture did not retain input");
  }
  if (context.live() != 0 || context.allocations() != context.releases())
    std::cerr << "type=" << unsigned(Element) << " bool=" << Boolean << " live=" << context.live()
              << " alloc=" << context.allocations() << " released=" << context.releases()
              << " fault=" << context.status() << '\n';
  require(context.live() == 0 && context.allocations() == context.releases(),
          "capture destruction retained invocation owners");
}
int main() {
  try {
    checkType<NumericType::Int8>();
    checkType<NumericType::Int16>();
    checkType<NumericType::Int32>();
    checkType<NumericType::Int64>();
    checkType<NumericType::UInt8>();
    checkType<NumericType::UInt16>();
    checkType<NumericType::UInt32>();
    checkType<NumericType::UInt64>();
    checkType<NumericType::Float32>();
    checkType<NumericType::Float64>();
    checkType<NumericType::UInt8, true>();
    for (unsigned byte : {2u, 255u}) {
      Invocation context;
      auto *data = static_cast<std::uint8_t *>(context.allocate(1));
      *data = byte;
      CpuTensor<NumericType::UInt8, 1> d{data, data, 0, {1}, {1}};
      auto capture = makeCapture<NumericType::UInt8, true>(context, d, 1);
      require(bool(capture) && !capture->advance() && !capture->advance() &&
                  context.status() == 3 && !capture->wire(),
              "invalid Bool reached completed wire");
    }
    for (unsigned at : {0u, 1u, 2u}) {
      Invocation context;
      auto *data = static_cast<std::int64_t *>(context.allocate(10000 * 8));
      CpuTensor<NumericType::Int64, 1> d{data, data, 0, {10000}, {1}};
      std::memset(data, 0, 10000 * 8);
      context.failAllocationAt(at);
      {
        auto capture = makeCapture<NumericType::Int64>(context, d, 1);
        if (capture) {
          for (unsigned i = 0; i < 4 && !capture->complete() && !context.poll(); ++i)
            capture->advance();
          require(!context.status() || context.status() == 2, "allocation failure changed kind");
        }
      }
      context.release(data);
      require(!context.live(), "allocation failure leaked packing storage");
    }
    {
      Invocation context;
      std::atomic<bool> cancel{false};
      context.bindCancellation(&cancel);
      auto *data = static_cast<std::uint64_t *>(context.allocate(10000 * 8));
      std::memset(data, 0, 10000 * 8);
      CpuTensor<NumericType::UInt64, 1> d{data, data, 0, {10000}, {1}};
      auto capture = makeCapture<NumericType::UInt64>(context, d, 1);
      require(bool(capture) && !capture->advance(), "cancellation setup failed");
      cancel = true;
      require(!capture->advance() && !capture->wire() && !context.status(),
              "cancelled packing published partial data or fabricated a fault");
    }
    {
      Invocation context;
      CpuTensor<NumericType::Int8, 1> empty{};
      auto capture = makeCapture<NumericType::Int8>(context, empty, 1);
      require(bool(capture) && capture->advance() && capture->wire()->sizes[0] == 4,
              "empty native descriptor was rejected or dereferenced");
    }
    {
      Invocation context;
      auto *data = static_cast<std::int64_t *>(context.allocate(8));
      *data = 42;
      CpuTensor<NumericType::Int64, 1> descriptor{data, data, 0, {1}, {1}};
      auto capture = makeCapture<NumericType::Int64>(context, descriptor, 1);
      require(bool(capture), "scope capture setup failed");
      {
        Invocation::OwnershipScope temporary(context);
        Invocation::OwnershipSelection selection(temporary);
        require(!capture->advance(), "header step completed nonempty payload");
      }
      require(capture->advance() && capture->wire()->aligned[4] == 42,
              "temporary caller scope retired pending wire storage");
    }
    {
      Invocation context;
      auto *data = static_cast<std::int64_t *>(context.allocate(8));
      *data = 42;
      CpuTensor<NumericType::Int64, 1> descriptor{data, data, 0, {1}, {1}};
      auto capture = makeCapture<NumericType::Int64>(context, descriptor, 1);
      require(capture && !capture->advance() && capture->advance(), "retirement setup failed");
      auto wire = *capture->wire();
      auto retained = retainCpuTensor(context, wire.view());
      require(bool(retained), "snapshot lease failed");
      capture->retireWire();
      require(!capture->wire() && !capture->complete() && !capture->advance() && !context.status(),
              "retired packing became available or fabricated a fault");
      const auto *words = reinterpret_cast<const std::uint64_t *>(retained->data().data());
      require(words[0] == DeviceArrayWireABI::Magic && words[4] == 42,
              "retirement destroyed an externally retained snapshot");
      retained.reset();
      context.release(data);
      capture.reset();
      require(!context.live() && context.allocations() == context.releases(),
              "retirement retained staging or source ownership");
    }
    std::cout << "Eleven array types: exact raw bits, offsets, bounded pages, aliases, moves, "
                 "invalid Bool, allocation faults and cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
