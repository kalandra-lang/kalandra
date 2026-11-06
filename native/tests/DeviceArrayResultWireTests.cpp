#include "../runtime/DeviceArrayResultWire.h"
#include "../runtime/DeviceAggregateResult.h"
#include "../runtime/DeviceGraphCapture.h"
#include "../runtime/DeviceGraphArena.h"
#include "../runtime/DeviceGraphMaterialize.h"
#include <functional>
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
struct AggregateResultTestPolicy {
  struct alignas(8) Result { std::array<std::byte, 88> data{}; };
  using F = DeviceAggregateResultField;
  inline static constexpr std::array<F, 4> fields{{
      {F::Record, F::None, F::None, 0, 0, 0, NumericType::UInt8, false},
      {F::Scalar, 0, F::None, 0, 0, 2, NumericType::Int64, false},
      {F::Array, 0, 1, 8, 2, 0, NumericType::Int8, false},
      {F::Array, 0, 1, 48, 2, 0, NumericType::Int8, false},
  }};
};
void aggregateEnvelope() {
  using A = DeviceAggregateEnvelopeABI;
  using W = DeviceArrayWireABI;
  constexpr std::array<std::uint64_t, 13> valid{
      A::Magic, 3, 2, 0, 0, 2, 0, 11, 2, 0, 11, 17, 29};
  enum Mutation { None, Tag, Inactive, Overlap, DifferentLength, Origin, Narrow, Trailing, Cancel };
  for (auto mutation : {None, Tag, Inactive, Overlap, DifferentLength, Origin, Narrow, Trailing, Cancel}) {
    Invocation context;
    std::atomic<bool> cancel{false};
    context.bindCancellation(&cancel);
    auto wire = makeWire(context, W::layout(NumericType::UInt64, false),
                         valid.size() + (mutation == Trailing));
    auto *data = wire.aligned + W::HeaderWords;
    std::copy(valid.begin(), valid.end(), data);
    switch (mutation) {
    case Tag: data[2] = 3; break;
    case Inactive: data[2] = 1; break;
    case Overlap: data[10] = 12; break;
    case DifferentLength: data[8] = 1; break;
    case Origin: data[6] = 1; data[7] = 0; break;
    case Narrow: data[12] = 256; break;
    case Trailing: data[13] = 0; break;
    default: break;
    }
    {
      Invocation::OwnershipScope scope(context);
      Invocation::OwnershipSelection select(scope);
      DeviceAggregateResultDecode<AggregateResultTestPolicy> decode;
      AggregateResultTestPolicy::Result result;
      check(decode.initialize(context, wire), "aggregate envelope setup");
      auto resolve = [](auto...) { return false; };
      bool complete = false;
      for (unsigned step = 0; step < 16 && !context.poll(); ++step) {
        if (step == 3 && mutation == Cancel)
          cancel = true;
        if (decode.advance(context, result, resolve)) {
          complete = true;
          break;
        }
      }
      if (mutation == None) {
        check(complete && !context.poll(), "valid aggregate result not published");
        CpuTensor<NumericType::Int8, 1> left, right;
        std::memcpy(&left, result.data.data() + 8, sizeof(left));
        std::memcpy(&right, result.data.data() + 48, sizeof(right));
        check(left.allocated == right.allocated && left.sizes[0] == 2 && right.sizes[0] == 2 &&
                  left.aligned[0] == 17 && right.aligned[1] == 29,
              "fresh aggregate regions lost alias identity or content");
      } else
        check(!complete && context.status() == (mutation == Cancel ? 0 : 3),
              "malformed or cancelled envelope published or changed fault");
    }
    context.release(wire.allocated);
    check(!context.live(), "aggregate envelope leaked an owner");
  }
}
void graphWire() {
  using V = DeviceGraphValidation;
  constexpr std::array guards{DeviceGraphGuard{0, 2}};
  const std::array node{
      DeviceGraphCell{2, 0, false, 1},
      DeviceGraphCell{0, 2, false, 0, guards},
      DeviceGraphCell{0, 2, false, 0, guards},
  };
  constexpr std::array numbers{DeviceGraphCell{255}};
  const std::array layouts{DeviceGraphLayout{node, true}, DeviceGraphLayout{numbers, false}};
  const std::vector<std::uint64_t> good{DeviceGraphWireABI::Magic, 2, 1, 1, 1, 2, 2,
                                        2, 2, 2, 10, 255};
  const auto validate = [&](const auto &wire, std::size_t work = 100) {
    return validateDeviceGraph(wire, layouts, 1, work, [] { return false; });
  };
  check(validate(good) == V::Valid, "valid graph with shared edge rejected");
  constexpr std::array arenaLayouts{12u, 5u};
  const auto image = makeDeviceGraphArena(good, layouts, arenaLayouts, 1, 14, 100,
                                          [] { return false; });
  constexpr auto mark = DeviceHeapBlockABI::ReadOnly | 1;
  const std::vector<std::uint64_t> relocated{0, 7, 12, 3, mark, 2, 8, 8, 6, 5, 2, mark, 10, 255};
  check(image && image->root == 1 && image->words == relocated,
        "arena image did not relocate shared edges or preserve payload bits");
  const auto tooSmall = makeDeviceGraphArena(good, layouts, arenaLayouts, 1, 13, 100,
                                             [] { return false; });
  check(!tooSmall && tooSmall.error() == V::Budget, "arena image ignored header budget");
  for (auto [index, value] : std::array<std::pair<std::size_t, std::uint64_t>, 9>{
           {{0, 0}, {1, UINT64_MAX}, {2, 2}, {4, 2}, {6, UINT64_MAX}, {7, 0},
            {8, 1}, {9, 3}, {11, 256}}}) {
    auto bad = good;
    bad[index] = value;
    check(validate(bad) == V::Invalid, "malformed graph accepted");
  }
  const std::array nativeCells{DeviceGraphNativeCell{node[0], 0, 8},
                               DeviceGraphNativeCell{node[1], 8, 8},
                               DeviceGraphNativeCell{node[2], 16, 8}};
  const std::array nativeBytes{DeviceGraphNativeCell{numbers[0], 0, 1}};
  const std::array nativeLayouts{DeviceGraphNativeLayout{nativeCells, 24, true},
                                 DeviceGraphNativeLayout{nativeBytes, 1, false}};
  const std::array<std::uint8_t, 2> shared{10, 255}, independent{10, 255};
  std::array<std::uint64_t, 3> record{2, 1, 1};
  const auto resolve = [&](std::size_t layout, std::span<const std::byte> field)
      -> std::expected<DeviceGraphNativeView, V> {
    std::uint64_t key = 0;
    std::memcpy(&key, field.data(), sizeof(key));
    if (layout != 2 || (key != 1 && key != 2))
      return std::unexpected(V::Invalid);
    const auto &storage = key == 1 ? shared : independent;
    return DeviceGraphNativeView{2, storage.size(), std::as_bytes(std::span(storage))};
  };
  const auto pack = [&] {
    return packDeviceGraph(DeviceGraphNativeView{1, 1, std::as_bytes(std::span(record))},
                           nativeLayouts, 100, 100, resolve, [] { return false; });
  };
  auto packed = pack();
  check(packed && *packed == good, "native graph packing lost shared allocation identity");
  record[2] = 2;
  packed = pack();
  check(packed && (*packed)[1] == 3 && validate(*packed) == V::Valid,
        "native graph packing merged separate equal allocations");
  auto inactive = good;
  inactive[7] = 1;
  check(validate(inactive) == V::Invalid, "inactive enum reference was read");
  inactive[8] = inactive[9] = 0;
  check(validate(inactive) == V::Invalid, "unreachable graph allocation accepted");
  const std::array empty{DeviceGraphWireABI::Magic, UINT64_C(1), UINT64_C(1),
                         UINT64_C(1), UINT64_C(1), UINT64_C(1), UINT64_C(0), UINT64_C(0)};
  check(validate(empty) == V::Valid, "inactive enum with canonical payload rejected");
  check(validate(good, 6) == V::Budget, "graph guard work escaped budget");
  unsigned polls = 0;
  check(validateDeviceGraph(good, layouts, 1, 100, [&] { return ++polls == 5; }) ==
            V::Cancelled,
        "graph validation ignored cancellation");
  constexpr std::array self{DeviceGraphCell{0, 1, false}};
  const std::array cyclicLayouts{DeviceGraphLayout{self, true}};
  const std::array cycle{DeviceGraphWireABI::Magic, UINT64_C(1), UINT64_C(1),
                         UINT64_C(1), UINT64_C(1), UINT64_C(1)};
  check(validateDeviceGraph(cycle, cyclicLayouts, 1, 100, [] { return false; }) == V::Valid,
        "typed cycle did not terminate in structural validation");
  constexpr std::array cycleArenaLayouts{13u};
  const auto loop = makeDeviceGraphArena(cycle, cyclicLayouts, cycleArenaLayouts, 1, 6, 100,
                                         [] { return false; });
  check(loop && loop->root == 1 && loop->words.back() == loop->root,
        "arena image did not relocate a self reference");
}
void closureGraphCapture() {
  using V = DeviceGraphValidation;
  Invocation source;
  const char code = 0, other = 0, nominal = 0;
  const std::uint64_t payload = 71;
  auto *flat = source.allocateEnvironment(sizeof(payload), &code);
  check(flat != nullptr, "allocate closure environment");
  std::memcpy(flat, &payload, sizeof(payload));
  const void *recursive = source.makeRecursiveValue(std::as_bytes(std::span(&payload, 1)), &nominal);
  check(recursive && source.bindRecursiveEnvironment(recursive, &code), "bind recursive environment");
  const std::array layouts{
      DeviceGraphCaptureLayout{8, 8, nullptr, &code},
      DeviceGraphCaptureLayout{8, 8, nullptr, &other},
      DeviceGraphCaptureLayout{16, 8, nullptr, &code},
      DeviceGraphCaptureLayout{8, 3, nullptr, &code},
      DeviceGraphCaptureLayout{8, 8, &nominal, &code}};
  DeviceGraphCapture resolver(source, layouts);
  for (const void *pointer : {static_cast<const void *>(flat), recursive}) {
    auto field = std::as_bytes(std::span(&pointer, 1));
    auto retained = resolver(1, field);
    check(retained && retained->data.size() == 8, "closure payload lease rejected");
    for (std::size_t id = 2; id <= layouts.size(); ++id)
      check(!resolver(id, field), "closure capture accepted wrong identity, size or alignment");
  }
  auto pointer = static_cast<const void *>(flat);
  source.settle();
  auto retained = resolver(1, std::as_bytes(std::span(&pointer, 1)));
  check(retained && std::memcmp(retained->data.data(), &payload, sizeof(payload)) == 0,
        "closure graph lease did not outlive the source");

  alignas(8) const std::array<std::uint64_t, 2> embedded{17, 29};
  const std::array inlineLayouts{
      DeviceGraphCaptureLayout{16, 8, nullptr, nullptr, true},
      DeviceGraphCaptureLayout{16, 8, nullptr, &code, true}};
  DeviceGraphCapture inlineResolver(source, inlineLayouts);
  const auto inlineField = std::as_bytes(std::span(embedded));
  const auto inlineView = inlineResolver(1, inlineField);
  check(inlineView && inlineView->data.data() == inlineField.data(),
        "embedded carrier lost its parent graph view");
  check(!inlineResolver(1, inlineField.first(8)) && !inlineResolver(2, inlineField),
        "malformed embedded carrier layout accepted");

  std::array<std::uint64_t, 2> carrier{71, 99};
  const auto project = +[](std::span<const std::byte> field) -> std::expected<std::uint64_t, V> {
    return field.size() == 16 ? std::expected<std::uint64_t, V>(2) : std::unexpected(V::Invalid);
  };
  std::array cells{DeviceGraphNativeCell{{2, 0, false, 1}, 0, sizeof(carrier), project}};
  const std::array native{DeviceGraphNativeLayout{cells, sizeof(carrier), true}};
  bool stop = false;
  const auto pack = [&](std::size_t budget = 10) {
    return packDeviceGraph({1, 1, std::as_bytes(std::span(carrier))}, native, 100, budget,
        [](auto, auto) -> std::expected<DeviceGraphNativeView, V> { return std::unexpected(V::Invalid); },
        [&] { return stop; });
  };
  auto wire = pack();
  check(wire && wire->back() == 2, "native identity projection copied pointer bytes");
  cells[0].wire.maximum = 1;
  check(!pack() && pack().error() == V::Invalid, "projected tag escaped canonical range check");
  cells[0].wire.maximum = 2;
  check(!pack(0) && pack(0).error() == V::Budget, "identity projection escaped work budget");
  stop = true;
  check(!pack() && pack().error() == V::Cancelled, "identity projection ignored cancellation");
  stop = false;
  cells[0].wire.target = 1;
  check(!pack(), "scalar projector accepted on a reference cell");
  cells[0].wire.target = 0;
  cells[0].bytes = 0;
  check(!pack(), "zero-byte projection accepted");
}
void graphCapture() {
  Invocation source;
  const char key = 0, wrongKey = 0;
  const std::uint64_t boxValue = 41;
  auto *box = source.makeRecursiveValue(std::as_bytes(std::span(&boxValue, 1)), &key);
  auto *array = static_cast<std::uint64_t *>(source.allocate(3 * sizeof(std::uint64_t)));
  check(box && array, "graph capture allocation failed");
  array[0] = 2;
  array[1] = 3;
  array[2] = 5;
  CpuTensor<NumericType::UInt64, 1> descriptor{array, array, 1, {2}, {1}};
  const std::array captureLayouts{DeviceGraphCaptureLayout{8, 8, &key},
                                  DeviceGraphCaptureLayout{8, 8, nullptr},
                                  DeviceGraphCaptureLayout{8, 8, &wrongKey}};
  DeviceGraphCapture capture(source, captureLayouts);
  const auto boxField = std::as_bytes(std::span(&box, 1));
  const auto arrayField = std::as_bytes(std::span(&descriptor, 1));
  auto retainedBox = capture(1, boxField);
  auto retainedArray = capture(2, arrayField);
  check(retainedBox && retainedArray && retainedArray->elements == 2,
        "graph capture rejected valid backing");
  check(!capture(3, boxField), "graph capture accepted an incorrect nominal key");
  descriptor.sizes[0] = 3;
  check(!capture(2, arrayField), "graph capture accepted an out-of-backing view");
  descriptor.sizes[0] = 2;
  descriptor.strides[0] = 2;
  check(!capture(2, arrayField), "graph capture accepted a non-contiguous descriptor");
  descriptor.strides[0] = 1;

  struct Root {
    const void *box;
    CpuTensor<NumericType::UInt64, 1> array;
  } root{box, descriptor};
  const std::array rootCells{DeviceGraphNativeCell{{0, 1}, offsetof(Root, box), sizeof(root.box)},
                              DeviceGraphNativeCell{{0, 2, true}, offsetof(Root, array), sizeof(root.array)}};
  const std::array scalar{DeviceGraphNativeCell{{UINT64_MAX}, 0, 8}};
  const std::array nativeLayouts{DeviceGraphNativeLayout{scalar, 8, true},
                                 DeviceGraphNativeLayout{scalar, 8, false},
                                 DeviceGraphNativeLayout{rootCells, sizeof(Root), true}};
  std::vector<DeviceGraphNativeView> views;
  auto packed = packDeviceGraph({3, 1, std::as_bytes(std::span(&root, 1))}, nativeLayouts,
                                100, 100, std::ref(capture), [] { return false; }, &views);
  const std::vector<std::uint64_t> expected{DeviceGraphWireABI::Magic, 3, 1,
                                           3, 1, 1, 1, 2, 2, 2, 3, 41, 3, 5};
  check(packed && *packed == expected, "real runtime backing did not pack to canonical graph");
  const std::array rootRules{rootCells[0].wire, rootCells[1].wire};
  const std::array scalarRules{scalar[0].wire};
  const std::array wireLayouts{DeviceGraphLayout{scalarRules, true},
                               DeviceGraphLayout{scalarRules, false},
                               DeviceGraphLayout{rootRules, true}};
  constexpr std::array layoutIDs{12u, 8u, 14u};
  std::vector<std::uint64_t> handles;
  const auto image = makeDeviceGraphArena(*packed, wireLayouts, layoutIDs, 3, 100, 100,
                                          [] { return false; }, &handles);
  check(bool(image), "could not relocate the capture graph");
  // A descriptor root and an environment/record field referring to the same
  // array must share one node, even though one view was resolved before packing.
  const std::array joinCells{DeviceGraphNativeCell{{0, 2, true}, 0, 1},
                             DeviceGraphNativeCell{{0, 3}, 1, 1}};
  const std::array joinedNative{nativeLayouts[0], nativeLayouts[1], nativeLayouts[2],
                                DeviceGraphNativeLayout{joinCells, 2, true}};
  const std::array joinRules{joinCells[0].wire, joinCells[1].wire};
  const std::array joinedWire{wireLayouts[0], wireLayouts[1], wireLayouts[2],
                              DeviceGraphLayout{joinRules, true}};
  const std::array joinedIDs{12u, 8u, 14u, 0u};
  const std::array roots{*retainedArray,
                         DeviceGraphNativeView{3, 1, std::as_bytes(std::span(&root, 1))}};
  const auto joined = packDeviceGraphCaptures(roots, joinedNative, 4, 100, 100,
                                              std::ref(capture), [] { return false; });
  check(joined && joined->at(1) == 4, "joined descriptor root duplicated a captured backing");
  const auto joinedImage = makeDeviceGraphArena(*joined, joinedWire, joinedIDs, 4, 100, 100,
                                               [] { return false; }, nullptr, true);
  check(joinedImage && joinedImage->captureRoots.size() == 2 &&
            joinedImage->words.at(joinedImage->captureRoots[1] + 5) == joinedImage->captureRoots[0],
        "joined graph lost identity between a direct array and captured field");
  const auto origins = makeDeviceGraphOrigins(views, handles, layoutIDs);
  check(origins && origins->size() == 2, "capture origin table included inline root");
  std::vector<std::uint64_t> envelope{DeviceGraphWireABI::OriginMagic, 3, 0, handles[1], handles[2]};
  envelope.insert(envelope.end(), packed->begin(), packed->end());
  const auto decode = [&] {
    return decodeDeviceGraphOrigins(envelope, wireLayouts, layoutIDs, 3, *origins, 100,
                                    [] { return false; });
  };
  const auto decoded = decode();
  check(decoded && verifyDeviceGraphOriginContents(*decoded, nativeLayouts, 100,
        [] { return false; }) == DeviceGraphValidation::Valid, "valid captured result rejected");
  const std::array<const void *, 3> keys{&key, &wrongKey, &rootCells};
  Root returned{};
  {
    Invocation::OwnershipScope scope(source);
    Invocation::OwnershipSelection selection(scope);
    check(materializeDeviceGraph(source, decoded->graph, wireLayouts, nativeLayouts, keys, 3,
          std::as_writable_bytes(std::span(&returned, 1)), 100, 100, decoded->nodes) ==
          DeviceGraphValidation::Valid && returned.box == box && returned.array.allocated == array &&
          returned.array.offset == 1, "captured result replaced original native identity or view");
  }
  envelope.back() = 99;
  const auto forged = decode();
  check(forged && verifyDeviceGraphOriginContents(*forged, nativeLayouts, 100,
        [] { return false; }) == DeviceGraphValidation::Invalid, "forged captured content accepted");
  source.release(array);
  source.release(const_cast<void *>(box));
  std::uint64_t retained = 0;
  std::memcpy(&retained, retainedArray->data.data(), sizeof(retained));
  check(retained == 3 && !source.live(), "graph capture lease did not survive source release");
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
    aggregateEnvelope();
    graphWire();
    graphCapture();
    closureGraphCapture();
    cancellationAndAllocation();
    std::cout << "PASS: eleven typed result decoders, exact bits, retained aliases, bounded pages, "
                 "faults, cancellation and owner retirement\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
