#pragma once
#include "DeviceArrayResultWire.h"
#include "DeviceAggregateResult.h"
#include "DeviceGraphResult.h"
#include "DeviceArrayWire.h"
#include "HostArrays.h"
#include <tuple>
#include <variant>
namespace kalandra::runtime {
template <NumericType Element, bool Boolean = false> struct DeviceArrayArgument {
  static constexpr auto element = Element;
  static constexpr bool boolean = Boolean;
  static constexpr bool graph = false;
  static constexpr bool resident = false;
  using Descriptor = CpuTensor<Element, 1>;
  using Capture = DeviceArrayCapture<Element, Boolean>;
  static const ValueLayout &layout() noexcept { return hostArrayLayout<Element, Boolean>(); }
};
// A resident tensor is already an immutable backend lease. Keep it in the
// admitted frame and bind it in source order without packing or uploading.
template <class Runtime, class Buffer> struct DeviceResidentArgument {
  using Descriptor = Buffer;
  using Capture = std::monostate;
  static constexpr bool graph = false;
  static constexpr bool resident = true;
  static const ValueLayout &layout() noexcept { return Runtime::bufferLayout(); }
};
// Private source bridge. Packing and upload belong to this admitted child, not
// its caller, so spawn does not insert an implicit await in the parent program.
// Runtime/Buffer/Device/Executable are the existing backend facade types.
template <class Runtime, class Buffer, class Device, class Executable, std::uint64_t Identity,
          NumericType ResultElement, bool BooleanResult, bool ArrayResult, bool BufferResult, std::size_t ScalarCount,
          class AggregatePolicy,
          class... Arguments>
class DeviceArrayTask final {
  static constexpr std::size_t Count = sizeof...(Arguments);
  using ArgumentTypes = std::tuple<Arguments...>;
  template <std::size_t I> using ArgumentAt = std::tuple_element_t<I, ArgumentTypes>;
  template <class Argument> static constexpr bool resident() {
    if constexpr (requires { Argument::resident; })
      return Argument::resident;
    return false;
  }
  static constexpr std::size_t Packed = (std::size_t{0} + ... + !resident<Arguments>());
  static constexpr bool Aggregate = !std::is_void_v<AggregatePolicy>;
  static_assert(Count > 0 || ArrayResult);
  static constexpr bool GraphResult = requires { AggregatePolicy::graphResult; };
  using Decoder = std::conditional_t<GraphResult, DeviceGraphResultDecode<AggregatePolicy>,
      std::conditional_t<Aggregate, DeviceAggregateResultDecode<AggregatePolicy>,
                         DeviceArrayResultDecode<ResultElement, BooleanResult>>>;
  using Result =
      std::conditional_t<BufferResult, Buffer,
          std::conditional_t<ArrayResult, typename Decoder::Result,
                             std::conditional_t<BooleanResult, bool, NumericValue<ResultElement>>>>;
  using Descriptors = std::tuple<typename Arguments::Descriptor...>;
  using Captures = std::tuple<typename Arguments::Capture...>;
  inline static const std::array<const ValueLayout *, Count> layouts{&Arguments::layout()...};

public:
  struct PacketData {
    Runtime *runtime;
    Executable executable;
    Device device;
    std::array<std::uint64_t, ScalarCount> scalars;
    Descriptors arguments;
  };

private:
  struct Frame {
    Runtime *runtime;
    Executable executable;
    Device device;
    std::array<std::uint64_t, ScalarCount> scalars;
    Descriptors arguments;
    HostArrayScopes scopes;
    Captures captures;
    std::array<Buffer, Count> snapshots{}, inputs{};
    std::array<TaskHandle, Count> uploads{};
    std::array<std::uint64_t, Count> origins{};
    GroupHandle group{};
    TaskHandle computation{};
    std::size_t packing = 0;
    unsigned phase = 0;
    Result value{};
    Buffer output{}, page{};
    TaskHandle download{};
    std::optional<DeviceArrayResultMetadata> metadata;
    std::optional<Invocation::OwnershipScope> resultScope;
    Decoder decoder;
    CpuTensor<NumericType::UInt64, 1> wire{};
    std::uint64_t cursor = 0, pageWords = 0;
    explicit Frame(const PacketData &packet)
        : runtime(packet.runtime), executable(packet.executable), device(packet.device),
          scalars(packet.scalars), arguments(packet.arguments) {}
  };
  template <class F, std::size_t... I> static void each(F &&function, std::index_sequence<I...>) {
    (function.template operator()<I>(), ...);
  }
  template <class F> static void each(F &&function) {
    each(std::forward<F>(function), std::make_index_sequence<Count>{});
  }
  static const ValueLayout &resultLayout() noexcept {
    if constexpr (BufferResult)
      return Runtime::bufferLayout();
    else if constexpr (Aggregate)
      return AggregatePolicy::layout();
    else if constexpr (ArrayResult)
      return hostArrayLayout<ResultElement, BooleanResult>();
    else if constexpr (BooleanResult)
      return boolScalarLayout();
    else
      return *numericScalarLayout(ResultElement);
  }
  static bool construct(Invocation &context, void *out, const Packet &packet) {
    if (!packet.data || !packet.source) {
      context.fault(3);
      return false;
    }
    const auto &data = *static_cast<const PacketData *>(packet.data);
    if (!data.runtime || !data.executable || !data.device || !data.executable.on(data.device)) {
      context.fault(3);
      return false;
    }
    auto &frame = *new (out) Frame(data);
    bool valid = true;
    if constexpr (Count > 0)
      valid = frame.scopes.initialize(context, layouts);
    std::size_t origin = 0;
    each([&]<std::size_t I>() {
      if constexpr (resident<ArgumentAt<I>>()) {
        const auto &buffer = std::get<I>(frame.arguments);
        valid = valid && bool(buffer) && buffer.onDevice();
        if (!valid && !context.status())
          context.fault(3);
      } else {
        if (valid)
          valid = frame.scopes.capture(context, I, *layouts[I], &std::get<I>(frame.arguments),
                                       *packet.source, &std::get<I>(data.arguments));
        frame.origins[I] = ++origin;
      }
    });
    // Distinct capture slots can name the same native array. Canonicalize their
    // origin IDs without allocating an uncharged hash table.
    if constexpr (!(Arguments::graph || ...)) {
      each([&]<std::size_t I>() {
        if constexpr (!resident<ArgumentAt<I>>()) {
          each([&]<std::size_t J>() {
            if constexpr (J < I && std::is_same_v<std::tuple_element_t<I, std::tuple<Arguments...>>,
                                                  std::tuple_element_t<J, std::tuple<Arguments...>>>) {
              const auto &a = std::get<I>(frame.arguments), &b = std::get<J>(frame.arguments);
              if (a.allocated == b.allocated && a.aligned == b.aligned && a.offset == b.offset &&
                  a.sizes == b.sizes && a.strides == b.strides)
                frame.origins[I] = frame.origins[J];
            }
          });
        }
      });
    }
    if (!valid || context.poll()) {
      frame.~Frame();
      return false;
    }
    return true;
  }
  static void destroy(void *memory) noexcept { static_cast<Frame *>(memory)->~Frame(); }
  static bool readBuffer(Task &task, TaskHandle child, Buffer &destination) {
    alignas(Buffer) std::byte storage[sizeof(Buffer)];
    if (!task.read(child, Runtime::bufferLayout(), storage))
      return false;
    auto *value = std::launder(reinterpret_cast<Buffer *>(storage));
    destination = *value;
    value->~Buffer();
    return true;
  }
  static void resume(Task &task) {
    auto &frame = *static_cast<Frame *>(task.frame());
    if (task.cancelled())
      return;
    auto &runtime = *frame.runtime;
    if (frame.phase == 0) {
      const auto selected = frame.packing;
      bool valid = true;
      each([&]<std::size_t I>() {
        if (selected != I)
          return;
        if constexpr (resident<ArgumentAt<I>>()) {
          frame.inputs[I] = std::get<I>(frame.arguments);
          ++frame.packing;
        } else {
          auto &capture = std::get<I>(frame.captures);
          if (!capture.origin()) {
            using Argument = std::tuple_element_t<I, std::tuple<Arguments...>>;
            const auto &descriptor = std::get<I>(frame.arguments);
            if constexpr (Argument::graph)
              valid = Argument::prepare(task.context(), capture, descriptor, frame.origins[I],
                                        GraphResult || (ArrayResult && !Aggregate));
            else
              valid = capture.initialize(task.context(), descriptor, frame.origins[I]);
            if (!valid)
              return;
          }
          if (capture.advance()) {
            auto snapshot = runtime.fromCpu(task.context(), capture.wire()->view());
            if (!snapshot) {
              task.fault(std::int64_t(snapshot.error()));
              valid = false;
              return;
            }
            frame.snapshots[I] = std::move(*snapshot);
            ++frame.packing;
          } else if (task.context().poll())
            valid = false;
        }
      });
      if (!valid) {
        hostArrayFailure(task);
        return;
      }
      if (frame.packing != Count) {
        task.yield();
        return;
      }
      frame.phase = 1;
    }
    if (frame.phase == 1) {
      if constexpr (Packed == 0) {
        frame.phase = 3;
      } else {
        frame.group = task.openGroup();
        if (!frame.group) {
          hostArrayFailure(task);
          return;
        }
        each([&]<std::size_t I>() {
          if constexpr (!resident<ArgumentAt<I>>())
            frame.uploads[I] = runtime.upload(task, frame.group, frame.snapshots[I], frame.device);
        });
        frame.phase = 2;
      }
    }
    if (frame.phase == 2) {
      for (std::size_t i = 0; i < Count; ++i)
        if (frame.origins[i] && !task.await(frame.uploads[i]))
          return;
      if (!task.close(frame.group)) {
        hostArrayFailure(task);
        return;
      }
      frame.phase = 3;
    }
    if (frame.phase == 3) {
      if constexpr (Packed > 0) {
        if (!task.join(frame.group))
          return;
        const auto failure = task.failure(frame.group);
        if (failure.kind != TaskFailure::None) {
          task.releaseGroup(frame.group);
          task.fail(failure);
          return;
        }
        for (std::size_t i = 0; i < Count; ++i)
          if (frame.origins[i] && !readBuffer(task, frame.uploads[i], frame.inputs[i])) {
            hostArrayFailure(task);
            return;
          }
        if (!task.releaseGroup(frame.group)) {
          hostArrayFailure(task);
          return;
        }
        frame.snapshots = {};
        each([&]<std::size_t I>() {
          if constexpr (!resident<ArgumentAt<I>>())
            std::get<I>(frame.captures).retireWire();
        });
      }
      frame.group = task.openGroup();
      if (!frame.group) {
        hostArrayFailure(task);
        return;
      }
      frame.computation = runtime.executeNumericProgram(task, frame.group, frame.executable,
                                                        frame.inputs, frame.scalars);
      frame.phase = 4;
    }
    if (frame.phase == 4) {
      if (!task.await(frame.computation))
        return;
      if (!task.close(frame.group)) {
        hostArrayFailure(task);
        return;
      }
      frame.phase = 5;
    }
    if (frame.phase == 5) {
      if (!task.join(frame.group))
        return;
      const auto failure = task.failure(frame.group);
      if (failure.kind != TaskFailure::None) {
        task.releaseGroup(frame.group);
        task.fail(failure);
        return;
      }
      bool read;
      if constexpr (BufferResult)
        read = readBuffer(task, frame.computation, frame.value);
      else if constexpr (ArrayResult)
        read = readBuffer(task, frame.computation, frame.output);
      else
        read = task.read(frame.computation, resultLayout(), &frame.value);
      if (!read || !task.releaseGroup(frame.group)) {
        hostArrayFailure(task);
        return;
      }
      if constexpr (!ArrayResult) {
        task.complete(&frame.value);
        return;
      } else {
        frame.metadata = Runtime::arrayResult(frame.output);
        if (!frame.metadata ||
            frame.metadata->layout != DeviceArrayWireABI::layout(ResultElement, BooleanResult) ||
            (frame.metadata->origin > (Aggregate ? 0 : Packed) &&
             !(!Aggregate && (Arguments::graph || ...) &&
               frame.metadata->origin <= UINT32_MAX &&
               (frame.metadata->origin & DeviceArrayWireABI::GraphOrigin))) ||
            frame.metadata->length >
                DeviceArrayWireABI::MaxWords - DeviceArrayWireABI::HeaderWords) {
          task.fault(3);
          return;
        }
        const auto words =
            DeviceArrayWireABI::HeaderWords + (frame.metadata->origin ? 0 : frame.metadata->length);
        if (words > SIZE_MAX / 8) {
          task.fault(2);
          return;
        }
        frame.resultScope.emplace(task.context());
        if (!frame.resultScope->valid()) {
          hostArrayFailure(task);
          return;
        }
        Invocation::OwnershipSelection selection(*frame.resultScope);
        auto *data = static_cast<std::uint64_t *>(task.context().allocate(words * 8));
        if (!data) {
          hostArrayFailure(task);
          return;
        }
        frame.wire = {data, data, 0, {std::int64_t(words)}, {1}};
        data[0] = DeviceArrayWireABI::ResultMagic;
        data[1] = frame.metadata->layout;
        data[2] = frame.metadata->length;
        data[3] = frame.metadata->origin;
        frame.phase = 6;
        task.yield();
        return;
      }
    }
    if constexpr (ArrayResult) {
      if (frame.phase == 6) {
        const auto payload = frame.metadata->origin ? 0 : frame.metadata->length;
        if (frame.cursor < payload) {
          frame.pageWords =
              std::min<std::uint64_t>(DeviceArrayWireABI::PageWords, payload - frame.cursor);
          frame.group = task.openGroup();
          if (!frame.group) {
            hostArrayFailure(task);
            return;
          }
          frame.download = runtime.downloadArrayPage(task, frame.group, frame.output, frame.cursor,
                                                     frame.pageWords);
          frame.phase = 7;
        } else {
          if constexpr (Aggregate) {
            if (!frame.decoder.initialize(task.context(), frame.wire)) {
              hostArrayFailure(task);
              return;
            }
          } else {
            const auto resolve = [&](std::uint64_t origin) {
              std::optional<CpuTensor<ResultElement, 1>> descriptor;
              each([&]<std::size_t I>() {
                if constexpr (std::is_same_v<std::tuple_element_t<I, std::tuple<Arguments...>>,
                                             DeviceArrayArgument<ResultElement, BooleanResult>>) {
                  if (frame.origins[I] == origin)
                    descriptor = std::get<I>(frame.arguments);
                } else if constexpr (ArgumentAt<I>::graph) {
                  if (origin & DeviceArrayWireABI::GraphOrigin) {
                    const auto *captured = findDeviceGraphOrigin(std::get<I>(frame.captures).origins,
                        origin ^ DeviceArrayWireABI::GraphOrigin,
                        DeviceArrayWireABI::layout(ResultElement, BooleanResult), frame.metadata->length);
                    if (captured && captured->stride == sizeof(NumericValue<ResultElement>))
                      descriptor = CpuTensor<ResultElement, 1>{
                          const_cast<NumericValue<ResultElement> *>(static_cast<const NumericValue<ResultElement> *>(captured->owner)),
                          const_cast<NumericValue<ResultElement> *>(static_cast<const NumericValue<ResultElement> *>(captured->aligned)),
                          captured->offset, {static_cast<std::int64_t>(captured->elements)}, {1}};
                  }
                }
              });
              return descriptor;
            };
            if (!frame.decoder.initialize(task.context(), frame.wire, resolve)) {
              hostArrayFailure(task);
              return;
            }
          }
          frame.output = {};
          frame.phase = 9;
          task.yield();
          return;
        }
      }
      if (frame.phase == 7) {
        if (!task.await(frame.download))
          return;
        if (!task.close(frame.group)) {
          hostArrayFailure(task);
          return;
        }
        frame.phase = 8;
      }
      if (frame.phase == 8) {
        if (!task.join(frame.group))
          return;
        const auto failure = task.failure(frame.group);
        if (failure.kind != TaskFailure::None) {
          task.releaseGroup(frame.group);
          task.fail(failure);
          return;
        }
        if (!readBuffer(task, frame.download, frame.page) || !task.releaseGroup(frame.group)) {
          hostArrayFailure(task);
          return;
        }
        auto bytes = Runtime::arrayPageBytes(frame.page);
        if (!bytes || bytes->size() != frame.pageWords * 8) {
          task.fault(bytes ? 3 : std::int64_t(bytes.error()));
          return;
        }
        std::memcpy(frame.wire.aligned + DeviceArrayWireABI::HeaderWords + frame.cursor,
                    bytes->data(), bytes->size());
        frame.cursor += frame.pageWords;
        frame.page = {};
        frame.phase = 6;
        task.yield();
        return;
      }
      if (frame.phase == 9) {
        bool complete;
        if constexpr (GraphResult) {
          Invocation::OwnershipSelection selection(*frame.resultScope);
          std::span<const DeviceGraphOrigin> origins;
          each([&]<std::size_t I>() {
            using Argument = std::tuple_element_t<I, std::tuple<Arguments...>>;
            if constexpr (Argument::graph)
              origins = std::get<I>(frame.captures).origins;
          });
          complete = frame.decoder.advance(task.context(), frame.value, origins);
        } else if constexpr (Aggregate) {
          Invocation::OwnershipSelection selection(*frame.resultScope);
          const auto resolve = [&](std::uint64_t origin, NumericType element, bool boolean,
                                   std::uint64_t length, std::byte *out) {
            bool resolved = false;
            each([&]<std::size_t I>() {
              using Argument = std::tuple_element_t<I, std::tuple<Arguments...>>;
              if constexpr (!resident<ArgumentAt<I>>()) {
                const auto &source = std::get<I>(frame.arguments);
                if (!resolved && frame.origins[I] == origin && Argument::element == element &&
                    Argument::boolean == boolean && std::uint64_t(source.sizes[0]) == length) {
                  std::memcpy(out, &source, sizeof(source));
                  resolved = true;
                }
              }
            });
            return resolved;
          };
          complete = frame.decoder.advance(task.context(), frame.value, resolve);
        } else
          complete = frame.decoder.advance();
        if (!complete) {
          if (task.context().poll())
            hostArrayFailure(task);
          else
            task.yield();
          return;
        }
        if constexpr (!Aggregate)
          frame.value = *frame.decoder.result();
        task.complete(&frame.value);
      }
    }
  }

public:
  inline static const TaskEntry entry{1,
                                      Identity,
                                      sizeof(PacketData),
                                      alignof(PacketData),
                                      sizeof(Frame),
                                      alignof(Frame),
                                      construct,
                                      destroy,
                                      resume,
                                      &resultLayout()};
};
} // namespace kalandra::runtime

namespace kalandra::runtime {
template <class Runtime, class Buffer, class Device, class Executable, std::uint64_t Identity,
          NumericType ResultElement, bool BooleanResult, std::size_t ScalarCount,
          class... Arguments>
using DeviceArrayInputJob =
    DeviceArrayTask<Runtime, Buffer, Device, Executable, Identity, ResultElement, BooleanResult,
                    false, false, ScalarCount, void, Arguments...>;
template <class Runtime, class Buffer, class Device, class Executable, std::uint64_t Identity,
          NumericType ResultElement, bool BooleanResult, std::size_t ScalarCount,
          class... Arguments>
using DeviceArrayResultJob =
    DeviceArrayTask<Runtime, Buffer, Device, Executable, Identity, ResultElement, BooleanResult,
                    true, false, ScalarCount, void, Arguments...>;
template <class Runtime, class Buffer, class Device, class Executable, std::uint64_t Identity,
          NumericType ResultElement, bool BooleanResult, std::size_t ScalarCount,
          class... Arguments>
using DeviceTensorResultJob =
    DeviceArrayTask<Runtime, Buffer, Device, Executable, Identity, ResultElement, BooleanResult,
                    false, true, ScalarCount, void, Arguments...>;
template <class Runtime, class Buffer, class Device, class Executable, std::uint64_t Identity,
          class Policy, std::size_t ScalarCount, class... Arguments>
using DeviceAggregateResultJob =
    DeviceArrayTask<Runtime, Buffer, Device, Executable, Identity, NumericType::UInt64, false,
                    true, false, ScalarCount, Policy, Arguments...>;
} // namespace kalandra::runtime
