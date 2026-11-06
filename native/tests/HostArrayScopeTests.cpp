#include "../runtime/HostArrays.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
using kalandra::NumericType;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <NumericType Element, bool Boolean = false> static void exercise() {
  using Descriptor = CpuTensor<Element, 1>;
  using Cell = NumericValue<Element>;
  const auto &layout = hostArrayLayout<Element, Boolean>();
  const std::array layouts{&layout, &layout};
  for (unsigned failure = 0; failure < 65; ++failure) {
    Invocation source, context;
    auto *data = static_cast<Cell *>(source.allocate(3 * sizeof(Cell)));
    require(data != nullptr, "source allocation failed");
    data[0] = 1;
    data[1] = 0;
    data[2] = 1;
    Descriptor input{data, data, 0, {3}, {1}}, current{}, saved{};
    context.failAllocationAt(failure);
    bool initialized = false;
    {
      HostArrayScopes scopes;
      initialized = scopes.initialize(context, layouts);
      if (initialized) {
        initialized = scopes.capture(context, 0, layout, &current, source, &input);
        if (initialized)
          initialized = scopes.capture(context, 1, layout, &saved, source, &input);
      }
      source.settle();
      if (initialized) {
        const auto rounds = failure ? 8 : 1000;
        for (int i = 0; i < rounds; ++i) {
          HostArrayWork work(scopes);
          if (!work)
            break;
          const auto count = i % 4;
          auto *fresh =
              count ? static_cast<Cell *>(context.allocate(count * sizeof(Cell))) : nullptr;
          if (count && !fresh)
            break;
          for (int j = 0; j < count; ++j)
            fresh[j] = Cell(j % 2);
          current = count ? Descriptor{fresh, fresh, 0, {count}, {1}} : Descriptor{};
          if (!scopes.commit(0, &current))
            break;
          require(saved.sizes[0] == 3 && saved.aligned[0] == 1 && saved.aligned[1] == 0,
                  "capture lost its original backing");
          require(context.live() <= 3, "array replacement retained discarded buffers");
        }
      }
      require(!context.status() || context.status() == 2, "array scope allocation fault changed");
    }
    require(!context.live(), "array scope destruction retained owners");
    context.settle();
    require(context.allocations() == context.releases(), "array scope leaked allocations");
  }
  // A failed aggregate result has no success descriptor to retain. Discard must
  // release just that slot and preserve a second alias without reading payload.
  {
    Invocation owner, destination;
    auto *bytes = static_cast<Cell *>(owner.allocate(sizeof(Cell)));
    require(bytes != nullptr, "discard source allocation failed");
    *bytes = Cell(1);
    Descriptor input{bytes, bytes, 0, {1}, {1}}, first{}, second{};
    HostArrayScopes scopes;
    require(scopes.initialize(destination, layouts), "discard scope initialization failed");
    const auto fixed = destination.live();
    require(scopes.capture(destination, 0, layout, &first, owner, &input) &&
                scopes.capture(destination, 1, layout, &second, owner, &input),
            "discard capture failed");
    owner.settle();
    const auto before = destination.live();
    {
      HostArrayWork work(scopes);
      require(bool(work) && scopes.discard(0), "valid payload discard failed");
    }
    require(second.aligned[0] == Cell(1) && destination.live() < before,
            "discard retired a live alias or retained an abandoned slot");
    {
      HostArrayWork work(scopes);
      require(bool(work) && scopes.discard(1), "last payload discard failed");
    }
    require(destination.live() == fixed, "discard kept native backing alive");
  }
  // Replacing an active outcome arm must retain aliases before clearing its
  // old arm. Repeat to detect backing accumulating across observations.
  {
    Invocation context;
    HostArrayScopes scopes;
    require(scopes.initialize(context, layouts), "outcome scope initialization failed");
    const auto fixed = context.live();
    Descriptor arms[2]{};
    {
      HostArrayWork work(scopes);
      auto *cell = static_cast<Cell *>(context.allocate(sizeof(Cell)));
      require(bool(work) && cell, "outcome source allocation failed");
      *cell = Cell(1);
      arms[0] = Descriptor{cell, cell, 0, {1}, {1}};
      require(scopes.commit(0, &arms[0]), "initial outcome commit failed");
    }
    for (unsigned i = 0; i < 1000; ++i) {
      const auto old = i % 2, next = 1 - old;
      HostArrayWork work(scopes);
      arms[next] = arms[old];
      require(bool(work) && scopes.commit(next, &arms[next]) && scopes.clear(old),
              "outcome arm replacement failed");
      require(arms[next].aligned[0] == Cell(1) && context.live() == fixed + 1,
              "outcome replacement lost or retained backing");
    }
    require(scopes.clear(0) && context.live() == fixed, "outcome arm retirement leaked");
  }
  Invocation context;
  std::atomic<bool> cancelled{false};
  context.bindCancellation(&cancelled);
  {
    HostArrayScopes scopes;
    require(scopes.initialize(context, layouts), "cancel scope initialization failed");
    cancelled = true;
    HostArrayWork work(scopes);
    require(!work && !context.status() && context.poll() == -1,
            "cancelled ownership work became an invalid-value fault");
  }
  context.settle();
}
int main() {
  try {
    exercise<NumericType::Int8>();
    exercise<NumericType::Int16>();
    exercise<NumericType::Int32>();
    exercise<NumericType::Int64>();
    exercise<NumericType::UInt8>();
    exercise<NumericType::UInt16>();
    exercise<NumericType::UInt32>();
    exercise<NumericType::UInt64>();
    exercise<NumericType::Float32>();
    exercise<NumericType::Float64>();
    exercise<NumericType::UInt8, true>();
    for (unsigned mode = 0; mode < 4; ++mode) {
      Invocation context;
      std::atomic<bool> cancelled{false};
      context.bindCancellation(&cancelled);
      const auto &layout = hostArrayLayout<NumericType::Int64>();
      const std::array layouts{&layout};
      {
        HostArrayScopes scopes;
        require(scopes.initialize(context, layouts), "discard guard initialization failed");
        if (mode == 0)
          require(!scopes.discard(0) && context.status() == 3,
                  "discard without an active ownership interval was admitted");
        else {
          HostArrayWork work(scopes);
          require(bool(work), "discard guard interval failed");
          if (mode == 1)
            require(!scopes.discard(1) && context.status() == 3,
                    "discard outside the checked slot range was admitted");
          else {
            if (mode == 3)
              context.fault(5);
            cancelled = true;
            require(!scopes.discard(0) && context.status() == (mode == 3 ? 5 : 0),
                    "discard changed cancellation or earlier-fault precedence");
          }
        }
      }
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "failed/cancelled discard retained ownership");
    }
    Invocation source, destination;
    auto *data = static_cast<unsigned char *>(source.allocate(1));
    require(data != nullptr, "invalid Bool source allocation failed");
    *data = 2;
    CpuTensor<NumericType::UInt8, 1> input{data, data, 0, {1}, {1}}, output{};
    require(
        !hostArrayLayout<NumericType::UInt8, true>().copy(destination, &output, source, &input) &&
            destination.status() == 3 && !output.allocated,
        "noncanonical Bool byte crossed the host frame boundary");
    std::cout << "Host array scopes: eleven element types, bounded replacement, captured aliases, "
                 "64 allocation faults, cancellation and canonical Boolean bytes passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
