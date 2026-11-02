#include "../include/kalandra/NumericType.h"
#include "../runtime/Runtime.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
struct Descriptor {
  void *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0, count = 0, stride = 0;
};
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (unsigned type = 0; type < 10; ++type) {
      const auto width = describeNumeric(static_cast<NumericType>(type))->bytes();
      for (std::int64_t stride : {-1, 1, 2}) {
        alignas(16) std::array<std::byte, 256> raw{};
        for (unsigned i = 0; i < raw.size(); ++i)
          raw[i] = std::byte(i * 37 + 13);
        Invocation context;
        Descriptor source{raw.data(), raw.data(), 9, 8, stride}, output;
        kalandra_array_import_v1(&context, &output, &source, type);
        require(!context.status() && output.allocated != source.allocated && output.offset == 0 &&
                    output.count == 8 && output.stride == 1,
                "foreign buffer was not imported to independent managed storage");
        for (int i = 0; i < 8; ++i)
          require(std::memcmp(static_cast<std::byte *>(output.aligned) + i * width,
                              raw.data() + (9 + i * stride) * width, width) == 0,
                  "import changed raw bits or strided ordering");
        const auto allocations = context.allocations();
        Descriptor alias;
        kalandra_array_import_v1(&context, &alias, &output, type);
        require(!context.status() && alias.allocated == output.allocated &&
                    context.allocations() == allocations,
                "managed import copied backing or lost identity");
        kalandra_array_import_v1(&context, &alias, &alias, type);
        require(alias.allocated == output.allocated, "overlapping descriptor import lost input");
        kalandra_array_validate_v1(&context, &alias, type);
        require(!context.status(), "valid reference rejected");
        context.settle();
        require(context.live() == 0, "import owner leaked");
      }
    }
    std::array<std::int64_t, 2> raw{1, 2};
    Descriptor foreign{raw.data(), raw.data(), 0, 2, 1};
    {
      Invocation context;
      auto *storage = context.allocate(16);
      auto *ordinary = context.allocate(16);
      require(storage && ordinary && kalandra_array_hold_v1(&context, storage),
              "array escape ownership failed");
      context.release(storage);
      require(bool(context.lease(storage)), "ordinary free retired an escaped alias");
      context.releaseRecords();
      require(!context.lease(storage) && bool(context.lease(ordinary)) && context.live() == 1,
              "escape cleanup missed its owner or hid an ordinary allocation leak");
      context.release(ordinary);
      require(!context.status() && !context.live() && context.allocations() == context.releases(),
              "array escape cleanup leaked storage");
    }
    {
      Invocation context;
      kalandra_array_validate_v1(&context, &foreign, 3);
      require(context.status() == 3 && context.allocations() == 0,
              "opaque reference validation imported unowned memory");
    }
    {
      Invocation context;
      auto *storage = context.allocate(8);
      Descriptor bad{storage, storage, 0, 2, 1}, output;
      kalandra_array_import_v1(&context, &output, &bad, 3);
      require(context.status() == 3 && !output.allocated, "managed bounds were not checked");
    }
    for (const auto descriptor : {
             Descriptor{raw.data(), raw.data(), 0, -1, 1},
             Descriptor{raw.data(), raw.data(), 0, INT64_MAX, 1},
             Descriptor{raw.data(), raw.data(), INT64_MAX, 2, 1},
             Descriptor{raw.data(), raw.data(), 0, 3, INT64_MAX},
             Descriptor{raw.data(), nullptr, 0, 2, 1},
         }) {
      Invocation context;
      Descriptor output{raw.data(), raw.data(), 1, 1, 1};
      kalandra_array_import_v1(&context, &output, &descriptor, 3);
      const auto expected = descriptor.count < 0 ? 1 : descriptor.count == INT64_MAX ? 2 : 3;
      require(context.status() == expected && !output.allocated && !context.allocations(),
              "invalid descriptor read or allocated payload before failing");
    }
    for (const auto invalidType : {10u, UINT32_MAX}) {
      Invocation context;
      Descriptor output;
      kalandra_array_import_v1(&context, &output, &foreign, invalidType);
      require(context.status() == 3 && !output.allocated && !context.allocations(),
              "invalid numeric identity admitted");
    }
    {
      Invocation context;
      Descriptor empty{raw.data(), nullptr, INT64_MAX, 0, INT64_MIN}, output;
      kalandra_array_import_v1(&context, &output, &empty, 3);
      require(!context.status() && !output.allocated && !output.aligned && !output.offset &&
                  !output.count && !output.stride && !context.allocations(),
              "empty import examined payload or lost canonical descriptor");
      kalandra_array_validate_v1(&context, &output, 3);
      require(!context.status(), "canonical empty reference rejected");
    }
    {
      Invocation context;
      context.failAllocationAt(1);
      Descriptor output;
      kalandra_array_import_v1(&context, &output, &foreign, 3);
      require(context.status() == 2 && !output.allocated && context.live() == 0,
              "failed import published backing");
    }
    for (const auto fault : {false, true}) {
      Invocation context;
      std::atomic<bool> stopped{true};
      context.bindCancellation(&stopped);
      if (fault)
        context.fault(4);
      Descriptor output;
      kalandra_array_import_v1(&context, &output, &foreign, 3);
      require(context.status() == (fault ? 4 : 0) && !output.allocated &&
                  context.allocations() == 0,
              "import changed cancellation/fault precedence");
    }
    std::cout << "Array imports: ten types, bits/strides, identity, bounds, faults and ownership "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
