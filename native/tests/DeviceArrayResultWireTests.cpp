#include "../runtime/DeviceArrayResultWire.h"
#include "../runtime/HostArrays.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
using Wire = CpuTensor<NumericType::UInt64, 1>;
Wire makeWire(Invocation &context, std::uint64_t layout, std::uint64_t length,
              std::uint64_t origin = 0) {
  const auto count = 4 + (origin ? 0 : length);
  auto *words = static_cast<std::uint64_t *>(context.allocate(count * 8));
  check(words != nullptr, "allocate test wire");
  words[0] = DeviceArrayWireABI::ResultMagic;
  words[1] = layout;
  words[2] = length;
  words[3] = origin;
  return {words, words, 0, {std::int64_t(count)}, {1}};
}
template <NumericType Element, bool Boolean = false> void fresh() {
  using Cell = NumericValue<Element>;
  using Decode = DeviceArrayResultDecode<Element, Boolean>;
  constexpr std::uint64_t n = DeviceArrayWireABI::PageWords * 2 + 3;
  Invocation context, published;
  auto wire = makeWire(context, DeviceArrayWireABI::layout(Element, Boolean), n);
  for (std::uint64_t i = 0; i < n; ++i) {
    auto bits = UINT64_C(0x7ff8000000000001) ^ i * UINT64_C(0x9e3779b97f4a7c15);
    if constexpr (Boolean)
      bits = i & 1;
    else if constexpr (sizeof(Cell) < 8)
      bits &= (UINT64_C(1) << (8 * sizeof(Cell))) - 1;
    wire.aligned[4 + i] = bits;
  }
  typename Decode::Descriptor result;
  {
    Decode decode;
    unsigned resolutions = 0;
    check(decode.initialize(context, wire,
                            [&](auto) -> std::optional<typename Decode::Descriptor> {
                              ++resolutions;
                              return {};
                            }),
          "initialize fresh wire");
    check(!decode.result() && !decode.advance(), "published fresh payload before copy");
    unsigned steps = 0;
    while (!decode.complete()) {
      auto before = decode.decodedElements();
      decode.advance();
      check(decode.decodedElements() - before <= DeviceArrayWireABI::PageWords,
            "decode exceeded 64KiB page");
      check(++steps <= 3, "decode failed to make progress");
    }
    check(!resolutions && decode.origin() == 0, "fresh result attempted alias resolution");
    auto value = *decode.result();
    check(reinterpret_cast<void *>(value.allocated) != wire.allocated && value.offset == 0 &&
              value.sizes[0] == n,
          "fresh output reused wire backing");
    for (std::uint64_t i = 0; i < n; ++i) {
      std::uint64_t bits = 0;
      std::memcpy(&bits, value.aligned + i, sizeof(Cell));
      check(bits == wire.aligned[4 + i], "decode changed integer/FP raw bits");
    }
    check(hostArrayLayout<Element, Boolean>().copy(published, &result, context, &value),
          "publish native result ownership");
  }
  auto retained = retainCpuTensor(published, result.view());
  check(bool(retained) && retained->bytes() == n * sizeof(Cell),
        "destroyed decoder retired published backing");
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, retained->data().data() + i * sizeof(Cell), sizeof(Cell));
    check(bits == wire.aligned[4 + i], "published raw bits changed after decoder destruction");
  }
  retained.reset();
  published.release(result.allocated);
  context.release(wire.allocated);
  check(!context.live() && !published.live(), "fresh decode leaked native backing");
}
template <NumericType Element, bool Boolean = false> void alias() {
  using Decode = DeviceArrayResultDecode<Element, Boolean>;
  using Cell = NumericValue<Element>;
  Invocation context, published;
  typename Decode::Descriptor observed;
  auto *data = static_cast<Cell *>(context.allocate(2 * sizeof(Cell)));
  std::memset(data, 0, 2 * sizeof(Cell));
  typename Decode::Descriptor original{data, data, 0, {2}, {1}};
  auto wire = makeWire(context, DeviceArrayWireABI::layout(Element, Boolean), 2, 7);
  {
    Decode decode;
    check(decode.initialize(context, wire,
                            [&](auto id) -> std::optional<typename Decode::Descriptor> {
                              return id == 7 ? std::optional(original) : std::nullopt;
                            }),
          "initialize alias");
    while (!decode.complete())
      check(decode.advance(), "validate Bool alias");
    auto result = *decode.result();
    check(result.allocated == original.allocated && result.aligned == original.aligned &&
              result.offset == original.offset && result.sizes == original.sizes &&
              result.strides == original.strides,
          "returned input lost native alias identity");
    result.aligned[0] = Cell(1);
    check(data[0] == Cell(1), "mutation of returned alias did not reach original");
    check(hostArrayLayout<Element, Boolean>().copy(published, &observed, context, &result),
          "publish alias ownership");
  }
  context.release(data);
  check(observed.allocated == data && observed.aligned[0] == Cell(1),
        "published alias lost backing after source release");
  published.release(observed.allocated);
  context.release(wire.allocated);
  check(!context.live() && !published.live(), "alias decode leaked");
}
void malformed() {
  for (unsigned which = 0; which < 8; ++which) {
    Invocation context;
    auto wire = makeWire(context, 1, 1);
    wire.aligned[4] = 42;
    if (which == 0)
      wire.aligned[0] = DeviceArrayWireABI::Magic;
    if (which == 1)
      wire.aligned[1] = 11;
    if (which == 2)
      wire.aligned[2] = UINT64_MAX;
    if (which == 3)
      wire.aligned[3] = std::uint64_t(UINT32_MAX) + 1;
    if (which == 4)
      wire.aligned[3] = 1;
    if (which == 5)
      wire.sizes[0] = 3;
    if (which == 6)
      wire.strides[0] = 2;
    if (which == 7)
      wire.aligned[2] = 0;
    DeviceArrayResultDecode<NumericType::Int8> decode;
    check(!decode.initialize(
              context, wire,
              [](auto) -> std::optional<CpuTensor<NumericType::Int8, 1>> { return {}; }),
          "malformed header accepted");
    check(context.status() == 3 && !decode.result(),
          "malformed header lost fault or published result");
  }
  for (bool boolean : {false, true}) {
    Invocation context;
    auto wire = makeWire(context, boolean ? 11 : 5, 1);
    wire.aligned[4] = boolean ? 2 : 256;
    if (boolean) {
      DeviceArrayResultDecode<NumericType::UInt8, true> d;
      check(
          d.initialize(context, wire,
                       [](auto) -> std::optional<CpuTensor<NumericType::UInt8, 1>> { return {}; }),
          "Bool malformed setup");
      d.advance();
      d.advance();
      check(context.status() == 3 && !d.result(), "noncanonical Bool accepted");
    } else {
      DeviceArrayResultDecode<NumericType::UInt8> d;
      check(
          d.initialize(context, wire,
                       [](auto) -> std::optional<CpuTensor<NumericType::UInt8, 1>> { return {}; }),
          "padding malformed setup");
      d.advance();
      d.advance();
      check(context.status() == 3 && !d.result(), "nonzero narrow padding accepted");
    }
  }
}
void emptyAndView() {
  std::atomic<bool> cancel{false};
  Invocation context;
  {
    auto wire = makeWire(context, 4, 0);
    DeviceArrayResultDecode<NumericType::Int64> decode;
    check(decode.initialize(
              context, wire,
              [](auto) -> std::optional<CpuTensor<NumericType::Int64, 1>> { return {}; }),
          "empty decode initialize");
    auto result = decode.result();
    check(result && !result->allocated && !result->aligned && result->sizes[0] == 0,
          "empty result allocated or dereferenced payload");
    context.release(wire.allocated);
  }
  {
    auto *data = static_cast<std::int32_t *>(context.allocate(12));
    data[0] = 1;
    data[1] = 2;
    data[2] = 3;
    CpuTensor<NumericType::Int32, 1> view{data, data, 1, {2}, {1}};
    auto wire = makeWire(context, 3, 2, 17);
    DeviceArrayResultDecode<NumericType::Int32> decode;
    check(decode.initialize(context, wire,
                            [&](auto id) -> std::optional<CpuTensor<NumericType::Int32, 1>> {
                              return id == 17 ? std::optional(view) : std::nullopt;
                            }),
          "view decode initialize");
    auto result = decode.result();
    check(result && result->offset == 1 && result->allocated == data && result->aligned == data,
          "alias view lost native offset");
    result->aligned[result->offset] = 42;
    check(data[1] == 42 && data[0] == 1, "alias view updated wrong element");
    context.release(data);
    context.release(wire.allocated);
  }
  {
    auto *data = static_cast<std::uint8_t *>(context.allocate(1));
    *data = 255;
    CpuTensor<NumericType::UInt8, 1> original{data, data, 0, {1}, {1}};
    auto wire = makeWire(context, 11, 1, 1);
    DeviceArrayResultDecode<NumericType::UInt8, true> decode;
    check(decode.initialize(
              context, wire,
              [&](auto) -> std::optional<CpuTensor<NumericType::UInt8, 1>> { return original; }),
          "invalid Bool alias setup");
    check(!decode.advance() && context.status() == 3 && !decode.result(),
          "invalid native Bool alias published");
    cancel = true;
    context.bindCancellation(&cancel);
    check(context.status() == 3 && !decode.result(),
          "later cancellation replaced recorded decode fault");
    context.release(data);
    context.release(wire.allocated);
  }
  check(!context.live(), "empty/view/invalid alias retained owners");
}
void cancellationAndAllocation() {
  for (unsigned failure = 1; failure <= 4; ++failure) {
    Invocation context;
    auto wire = makeWire(context, 4, 2);
    wire.aligned[4] = 11;
    wire.aligned[5] = 12;
    context.failAllocationAt(failure);
    {
      DeviceArrayResultDecode<NumericType::Int64> d;
      if (d.initialize(context, wire,
                       [](auto) -> std::optional<CpuTensor<NumericType::Int64, 1>> { return {}; }))
        for (unsigned i = 0; i < 3 && !d.complete() && !context.poll(); ++i)
          d.advance();
      check(!context.status() || context.status() == 2, "allocation failure changed fault");
    }
    context.release(wire.allocated);
    check(!context.live(), "allocation failure leaked");
  }
  std::atomic<bool> cancel = false;
  Invocation context;
  context.bindCancellation(&cancel);
  auto wire = makeWire(context, 4, DeviceArrayWireABI::PageWords + 1);
  std::fill_n(wire.aligned + 4, DeviceArrayWireABI::PageWords + 1, 42);
  {
    DeviceArrayResultDecode<NumericType::Int64> d;
    check(d.initialize(context, wire,
                       [](auto) -> std::optional<CpuTensor<NumericType::Int64, 1>> { return {}; }),
          "cancel initialize");
    d.advance();
    check(!d.advance() && d.decodedElements() == DeviceArrayWireABI::PageWords,
          "cancel page setup");
    cancel = true;
    check(!d.advance() && !d.result() && !context.status(),
          "cancelled decode published partial result/fault");
  }
  context.release(wire.allocated);
  check(!context.live(), "cancelled decode leaked");
}
void envelopeRegion() {
  using Decode = DeviceArrayResultDecode<NumericType::Int64>;
  const auto noCapture = [](auto) -> std::optional<Decode::Descriptor> { return {}; };
  {
    Invocation context;
    auto wire = makeWire(context, 4, 8);
    wire.aligned[6] = 17;
    wire.aligned[7] = 29;
    {
      Decode decode;
      check(decode.initializeRegion(context, wire, 6, 2, 0, noCapture),
            "embedded region rejected");
      context.release(wire.allocated);
      check(!decode.advance() && !decode.result(), "partial region published");
      check(decode.advance(), "embedded region copy did not settle");
      auto result = decode.result();
      check(result && result->sizes[0] == 2 &&
                result->aligned[0] == 17 && result->aligned[1] == 29,
            "embedded region decoded from wrong offset");
    }
    check(!context.live(), "embedded region leaked its owners");
  }
  for (const auto offset : {UINT64_C(11), UINT64_MAX}) {
    Invocation context;
    auto wire = makeWire(context, 4, 8);
    Decode decode;
    check(!decode.initializeRegion(context, wire, offset, 2, 0, noCapture) &&
              context.status() == 3 && !decode.result(),
          "out-of-bounds embedded region accepted");
    context.release(wire.allocated);
  }
}
} // namespace
int main() {
  try {
#define EACH(T)                                                                                    \
  fresh<NumericType::T>();                                                                         \
  alias<NumericType::T>();
    EACH(Int8)
    EACH(Int16)
    EACH(Int32)
    EACH(Int64) EACH(UInt8) EACH(UInt16) EACH(UInt32) EACH(UInt64) EACH(Float32) EACH(Float64)
#undef EACH
        fresh<NumericType::UInt8, true>();
    alias<NumericType::UInt8, true>();
    malformed();
    emptyAndView();
    envelopeRegion();
    cancellationAndAllocation();
    std::cout << "PASS: eleven typed result decoders, exact bits, retained aliases, bounded pages, "
                 "faults, cancellation and owner retirement\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
