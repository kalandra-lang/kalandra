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
