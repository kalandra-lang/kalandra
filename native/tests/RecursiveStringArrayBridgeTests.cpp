#include "../runtime/StringArray.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace kalandra::runtime;
extern "C" const void *string_array_box(const StringArrayDescriptor *, std::int64_t, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (unsigned mode = 0; mode < 17; ++mode) {
      Invocation context, foreign;
      const std::string bytes("é\0👩", 7);
      StringDescriptor shared, other;
      kalandra_string_literal_v1(&context, &shared, bytes.data(), bytes.size());
      kalandra_string_literal_v1(&context, &other, "x", 1);
      auto *cells =
          static_cast<StringDescriptor *>(context.allocateRecord(4 * sizeof(StringDescriptor)));
      require(cells && !context.status(), "String array fixture allocation failed");
      const std::array<StringDescriptor, 4> values{shared, {}, shared, other};
      std::memcpy(cells, values.data(), sizeof(values));
      auto a = context.lease(cells), b = context.lease(shared.allocated),
           c = context.lease(other.allocated);
      StringArrayDescriptor input{cells, cells, 0, 4, 1};
      switch (mode) {
      case 1:
        input = {reinterpret_cast<void *>(0x1234), nullptr, -1, -1, -1};
        break;
      case 2:
        input.allocated = input.data = reinterpret_cast<void *>(0x1230);
        break;
      case 3:
        input.size = -1;
        break;
      case 4:
        input.size = INT64_MAX;
        break;
      case 5:
        input.offset = 1;
        break;
      case 6:
        input.stride = 2;
        break;
      case 7:
        input.data = cells + 1;
        break;
      case 8:
        kalandra_string_literal_v1(&foreign, &cells[3], "foreign", 7);
        break;
      case 9:
        cells[3].byteLength = 100;
        break;
      case 10:
        cells[3].data = static_cast<const char *>(shared.data) + 1;
        break;
      case 11:
        input = {};
        break;
      default:
        break;
      }
      if (mode >= 12)
        context.failAllocationAt(context.allocations() + mode - 11);
      auto *root = string_array_box(&input, mode == 1 ? 1 : 2, &context);
      const bool invalid = mode >= 2 && mode <= 10;
      const bool allocationFailure = mode >= 12 && mode <= 15;
      if (invalid || allocationFailure) {
        require(!root && context.status() == (allocationFailure ? 2 : 3),
                "invalid String array or failed publication accepted");
        context.settle();
      } else {
        require(root && !context.status(), "String array bridge construction failed");
        auto retained = context.lease(root);
        require(bool(retained), "String array root lease missing");
        context.settle();
        const bool owned = mode == 0 || mode == 16;
        require(a->referenceCount() == (owned ? 2 : 1) && b->referenceCount() == (owned ? 3 : 1) &&
                    c->referenceCount() == (owned ? 2 : 1),
                "String array storage or duplicate String edges lost/retained incorrectly");
        if (owned) {
          auto carrier = retained->dependency(0);
          require(bool(carrier), "String array dependency owner missing");
          auto array = carrier->dependency(0), text = carrier->dependency(1),
               duplicate = carrier->dependency(2);
          require(array && text && duplicate && array->bytes().size() == sizeof(values) &&
                      text->bytes().size() == bytes.size() &&
                      !std::memcmp(text->bytes().data(), bytes.data(), bytes.size()) &&
                      text->bytes().data() == duplicate->bytes().data(),
                  "String array bytes did not survive invocation settlement");
        }
        retained.reset();
      }
      require(a->referenceCount() == 1 && b->referenceCount() == 1 && c->referenceCount() == 1 &&
                  !context.live() && context.allocations() == context.releases() &&
                  !context.callDepth(),
              "String array dependencies, temporary table or call depth leaked");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
      Invocation context;
      if (mode == 0) {
        require(!kalandra_recursive_string_array_dependency_v1(&context, nullptr, 0) &&
                    !context.status(),
                "inactive String array descriptor inspected");
      } else if (mode == 1) {
        require(!kalandra_recursive_string_array_dependency_v1(&context, nullptr, 2) &&
                    context.status() == 3,
                "invalid String array active flag accepted");
      } else if (mode == 2) {
        require(!kalandra_recursive_string_array_dependency_v1(&context, nullptr, 1) &&
                    context.status() == 3,
                "null active String array accepted");
      } else {
        std::atomic<bool> cancelled{true};
        context.bindCancellation(&cancelled);
        require(!kalandra_recursive_string_array_dependency_v1(&context, nullptr, 1) &&
                    !context.live(),
                "cancelled String array validation executed");
      }
    }
    for (bool overLimit : {false, true}) {
      Invocation context;
      StringDescriptor shared;
      kalandra_string_literal_v1(&context, &shared, "x", 1);
      auto observer = context.lease(shared.allocated);
      auto *cells = static_cast<StringDescriptor *>(
          context.allocateRecord(Invocation::maxRecursiveDependencies * sizeof(StringDescriptor)));
      require(cells, "dependency limit fixture allocation failed");
      for (std::size_t i = 0; i < Invocation::maxRecursiveDependencies; ++i)
        std::memcpy(cells + i, &shared, sizeof(shared));
      StringArrayDescriptor input{
          cells, cells, 0,
          static_cast<std::int64_t>(Invocation::maxRecursiveDependencies - (overLimit ? 0 : 1)), 1};
      auto *owner = kalandra_recursive_string_array_dependency_v1(&context, &input, 1);
      require(overLimit ? !owner && context.status() == 2 : owner && !context.status(),
              "String array dependency boundary changed");
      context.settle();
      require(observer->referenceCount() == 1 && !context.live(), "dependency limit leaked edges");
    }
    std::cout << "recursive String array bridge passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
