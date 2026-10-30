#include "../runtime/String.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" const void *function_array_box(const void *, std::int64_t, Invocation *);
struct Cell {
  const void *code, *environment;
};
struct Descriptor {
  void *allocated, *data;
  std::int64_t offset, size, stride;
};
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    static const char key = 0, code = 0, otherCode = 0;
    for (unsigned mode = 0; mode < 14; ++mode) {
      Invocation context, foreign;
      StringDescriptor text;
      kalandra_string_literal_v1(&context, &text, "é", 2);
      const void *dependency = text.allocated;
      auto *env = context.makeRecursiveValueFromPointers(std::as_bytes(std::span{&text, 1}), &key,
                                                         {&dependency, 1});
      require(env && context.bindRecursiveEnvironment(env, &code), "environment fixture failed");
      auto envObserver = context.lease(env), textObserver = context.lease(text.allocated);
      Cell cells[] = {{&code, env}, {&code, env}, {&otherCode, nullptr}};
      if (mode == 2)
        cells[1].code = nullptr;
      if (mode == 3)
        cells[1].code = &otherCode;
      if (mode == 4)
        cells[1].environment = reinterpret_cast<void *>(0x1230);
      if (mode == 5)
        cells[1].environment = foreign.allocateEnvironment(8, &code);
      if (mode == 6)
        cells[1].environment = context.allocateEnvironment(8, &code);
      auto *storage = context.allocateRecord(sizeof(cells));
      require(storage != nullptr, "array fixture allocation failed");
      std::memcpy(storage, cells, sizeof(cells));
      Descriptor descriptor{storage, storage, 0, 3, 1};
      auto arrayObserver = context.lease(storage);
      if (mode == 1)
        descriptor = {reinterpret_cast<void *>(0x1230), nullptr, -1, -2, 5};
      if (mode == 7)
        descriptor.offset = 1;
      if (mode == 8)
        descriptor.size = 4;
      if (mode == 9)
        descriptor = {};
      if (mode >= 10)
        context.failAllocationAt(context.allocations() + mode - 9);
      auto *root = function_array_box(&descriptor, mode == 1 ? 1 : 2, &context);
      const bool invalid = mode >= 2 && mode <= 8;
      const bool allocationFailure = mode >= 10 && mode <= 12;
      if (invalid || allocationFailure) {
        require(!root && context.status() == (invalid ? 3 : 2), "invalid function array published");
        context.settle();
      } else {
        require(root && !context.status(), "function array publication failed");
        auto retained = context.lease(root);
        context.settle();
        const bool owned = mode == 0 || mode == 13;
        require(envObserver->referenceCount() == (owned ? 3 : 1) &&
                    arrayObserver->referenceCount() == (owned ? 2 : 1),
                "duplicate closure or array edge lost/retained incorrectly");
        if (owned) {
          auto owner = retained->dependency(0);
          require(owner && owner->dependency(0) && owner->dependency(1) && owner->dependency(2),
                  "array ownership graph missing");
          auto captured = owner->dependency(1)->dependency(0);
          require(captured && captured->bytes().size() == 2 &&
                      !std::memcmp(captured->bytes().data(), "é", 2),
                  "closure captures lost after settlement");
        }
        retained.reset();
      }
      require(envObserver->referenceCount() == 1 && arrayObserver->referenceCount() == 1 &&
                  !context.live() && !context.callDepth() &&
                  context.allocations() == context.releases(),
              "function array cleanup leaked");
      envObserver.reset();
      require(textObserver->referenceCount() == 1, "captured String edge leaked");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
      Invocation context;
      if (mode == 0)
        require(!kalandra_recursive_function_array_dependency_v1(&context, nullptr, 0) &&
                    !context.status(),
                "inactive null descriptor read");
      if (mode == 1)
        require(!kalandra_recursive_function_array_dependency_v1(&context, nullptr, 2) &&
                    context.status() == 3,
                "invalid active flag admitted");
      if (mode == 2)
        require(!kalandra_recursive_function_array_dependency_v1(&context, nullptr, 1) &&
                    context.status() == 3,
                "active null descriptor admitted");
      if (mode == 3) {
        std::atomic<bool> cancelled{true};
        context.bindCancellation(&cancelled);
        require(!kalandra_recursive_function_array_dependency_v1(&context, nullptr, 1) &&
                    context.poll(),
                "cancelled descriptor validation executed");
      }
      context.settle();
      require(!context.live() && context.allocations() == context.releases(), "validation leaked");
    }
    // The bound counts ownership edges, including repeated environments, rather than array length.
    for (bool captured : {false, true}) {
      Invocation context;
      std::uint64_t payload = 42;
      auto *env = context.makeRecursiveValue(std::as_bytes(std::span{&payload, 1}), &key);
      require(env && context.bindRecursiveEnvironment(env, &code), "limit fixture failed");
      const auto count = Invocation::maxRecursiveDependencies;
      auto *cells = static_cast<Cell *>(context.allocateRecord(count * sizeof(Cell)));
      require(cells != nullptr, "limit array allocation failed");
      for (std::size_t i = 0; i < count; ++i)
        cells[i] = {&code, captured ? env : nullptr};
      Descriptor descriptor{cells, cells, 0, static_cast<std::int64_t>(count), 1};
      auto *owner = kalandra_recursive_function_array_dependency_v1(&context, &descriptor, 1);
      require(captured ? (!owner && context.status() == 2) : (owner && !context.status()),
              "function array dependency limit incorrect");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "limit cleanup leaked");
    }
    std::cout << "recursive function array bridge passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
