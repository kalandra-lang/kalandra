#include "../runtime/Runtime.h"
#include "kalandra/NumericType.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" const void *collection_box(const void *, const void *, std::int64_t, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Descriptor {
  void *allocated, *data;
  std::int64_t offset, length, stride;
};
int main() {
  try {
    for (unsigned mode = 0; mode < 15; ++mode) {
      Invocation context, foreign;
      auto *numbers = static_cast<std::int64_t *>(context.allocateRecord(16));
      auto *booleans = static_cast<unsigned char *>(context.allocateRecord(2));
      require(numbers && booleans, "collection fixture allocation failed");
      numbers[0] = 42;
      numbers[1] = 99;
      booleans[0] = 1;
      booleans[1] = 0;
      auto numericBuffer = context.lease(numbers), boolBuffer = context.lease(booleans);
      Descriptor numeric{numbers, numbers, 0, 2, 1}, flags{booleans, booleans, 0, 2, 1};
      switch (mode) {
      case 1:
        numeric = flags = {reinterpret_cast<void *>(0x1234), nullptr, -1, -1, -1};
        break;
      case 2:
        numeric.allocated = numeric.data = reinterpret_cast<void *>(0x1230);
        break;
      case 3:
        numeric.length = -1;
        break;
      case 4:
        numeric.length = INT64_MAX;
        break;
      case 5:
        numeric.offset = 1;
        break;
      case 6:
        numeric.stride = 2;
        break;
      case 7:
        numeric.data = numbers + 1;
        break;
      case 8:
        numeric.allocated = numeric.data = foreign.allocateRecord(16);
        break;
      case 9:
        booleans[1] = 2;
        break;
      case 10:
        flags.length = -1;
        break;
      case 11:
        numeric = flags = {};
        break;
      default:
        break;
      }
      if (mode >= 12)
        context.failAllocationAt(context.allocations() + mode - 11);
      auto *root = collection_box(&numeric, &flags, mode == 1 ? 1 : 2, &context);
      if ((mode >= 2 && mode <= 10) || mode == 12 || mode == 13) {
        require(!root && context.status() == (mode >= 12 ? 2 : 3),
                "invalid descriptor or failed box was published");
        context.settle();
      } else {
        require(root && !context.status(), "collection bridge construction failed");
        auto retained = context.lease(root);
        require(bool(retained), "collection root lease missing");
        context.settle();
        const bool owned = mode == 0 || mode == 14;
        require(numericBuffer->referenceCount() == (owned ? 2 : 1) &&
                    boolBuffer->referenceCount() == (owned ? 2 : 1),
                "active collection dependency lost or inactive storage retained");
        if (owned) {
          auto a = retained->dependency(0), b = retained->dependency(1);
          const std::array<std::int64_t, 2> expected{42, 99};
          require(a && b && a->bytes().size() == 16 && b->bytes().size() == 2 &&
                      !std::memcmp(a->bytes().data(), expected.data(), 16) &&
                      b->bytes()[0] == std::byte{1} && b->bytes()[1] == std::byte{0},
                  "retained collection bytes changed after settlement");
        }
        retained.reset();
      }
      require(numericBuffer->referenceCount() == 1 && boolBuffer->referenceCount() == 1 &&
                  !context.live() && context.allocations() == context.releases() &&
                  context.callDepth() == 0,
              "collection edges or checked call slots leaked");
    }
    for (unsigned mode = 0; mode < 5; ++mode) {
      Invocation context;
      if (mode == 0) {
        require(!kalandra_recursive_tensor_dependency_v1(&context, nullptr, 999, 999, 0) &&
                    !kalandra_recursive_bool_dependency_v1(&context, nullptr, 0) &&
                    !context.status(),
                "inactive collection metadata inspected");
      } else if (mode == 1) {
        require(!kalandra_recursive_tensor_dependency_v1(&context, nullptr, 0, 0, 2) &&
                    context.status() == 3,
                "bad tensor active flag accepted");
      } else if (mode == 2) {
        require(!kalandra_recursive_bool_dependency_v1(&context, nullptr, 2) &&
                    context.status() == 3,
                "bad Bool active flag accepted");
      } else if (mode == 3) {
        Descriptor empty{};
        require(!kalandra_recursive_tensor_dependency_v1(&context, &empty, 129, 0, 1) &&
                    context.status() == 3,
                "bad tensor rank accepted");
      } else {
        std::atomic<bool> cancelled{true};
        context.bindCancellation(&cancelled);
        require(!kalandra_recursive_tensor_dependency_v1(&context, nullptr, 1, 0, 1) &&
                    !kalandra_recursive_bool_dependency_v1(&context, nullptr, 1) && !context.live(),
                "cancelled collection validation executed");
      }
    }
    std::cout << "recursive collection bridge passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
