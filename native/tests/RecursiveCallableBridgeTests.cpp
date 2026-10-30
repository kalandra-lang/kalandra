#include "../runtime/String.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" const void *callable_box(const void *, const void *, std::int64_t, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    static const char key = 0, code = 0, otherCode = 0;
    for (unsigned mode = 0; mode < 10; ++mode) {
      Invocation context, foreign;
      StringDescriptor text;
      kalandra_string_literal_v1(&context, &text, "é", 2);
      const void *dependency = text.allocated;
      auto *environment = context.makeRecursiveValueFromPointers(std::as_bytes(std::span{&text, 1}),
                                                                 &key, {&dependency, 1});
      require(environment && kalandra_recursive_environment_bind_v1(&context, environment, &code),
              "recursive environment binding failed");
      auto envObserver = context.lease(environment), textObserver = context.lease(text.allocated);
      auto *selectedCode = static_cast<const void *>(&code);
      auto *selectedEnvironment = environment;
      if (mode == 1) {
        selectedCode = nullptr;
        selectedEnvironment = reinterpret_cast<void *>(0x1234);
      }
      if (mode == 2)
        selectedCode = nullptr;
      if (mode == 3)
        selectedCode = &otherCode;
      if (mode == 4)
        selectedEnvironment = reinterpret_cast<void *>(0x1230);
      if (mode == 5)
        selectedEnvironment = foreign.allocateEnvironment(sizeof(text), &code);
      if (mode == 6)
        selectedEnvironment = context.allocateEnvironment(sizeof(text), &code);
      if (mode == 7)
        selectedEnvironment = nullptr;
      if (mode >= 8)
        context.failAllocationAt(context.allocations() + mode - 7);
      auto *root = callable_box(selectedCode, selectedEnvironment, mode == 1 ? 1 : 2, &context);
      const bool invalid = mode >= 2 && mode <= 6;
      if (invalid || mode == 8) {
        require(!root && context.status() == (invalid ? 3 : 2),
                "invalid callable or failed box published");
        context.settle();
      } else {
        require(root && !context.status(), "recursive callable publication failed");
        auto retained = context.lease(root);
        require(bool(retained), "recursive callable root lease missing");
        context.settle();
        const bool owned = mode == 0 || mode == 9;
        require(envObserver->referenceCount() == (owned ? 2 : 1) &&
                    textObserver->referenceCount() == 2,
                "callable environment or captured String backing lost/retained incorrectly");
        if (owned) {
          auto env = retained->dependency(0);
          require(env && env->bytes().size() == sizeof(text), "retained environment bytes missing");
          auto captured = env->dependency(0);
          require(captured && captured->bytes().size() == 2 &&
                      !std::memcmp(captured->bytes().data(), "é", 2),
                  "captured String bytes lost");
        }
        retained.reset();
      }
      require(envObserver->referenceCount() == 1 && !context.live() && !context.callDepth() &&
                  context.allocations() == context.releases(),
              "callable owner cleanup leaked");
      envObserver.reset();
      require(textObserver->referenceCount() == 1,
              "captured String edge survived environment release");
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
      Invocation context;
      std::uint64_t payload = 42;
      auto *env = context.makeRecursiveValue(std::as_bytes(std::span{&payload, 1}), &key);
      require(env && context.bindRecursiveEnvironment(env, &code), "environment fixture failed");
      if (mode == 0)
        require(!context.bindRecursiveEnvironment(env, &code) && context.status() == 3,
                "published environment rebound");
      if (mode == 1)
        require(!context.readRecursiveEnvironment(env, 7, &code) && context.status() == 3,
                "wrong environment size read admitted");
      if (mode == 2)
        require(!context.readRecursiveEnvironment(env, 8, &otherCode) && context.status() == 3,
                "wrong callable environment read admitted");
      if (mode == 3)
        require(context.readRecursiveEnvironment(env, 8, &code) == env && !context.status(),
                "valid callable environment read rejected");
      if (mode == 4)
        require(!kalandra_recursive_function_dependency_v1(&context, nullptr, nullptr, 0) &&
                    !context.status() &&
                    !kalandra_recursive_function_dependency_v1(&context, &code, env, 2) &&
                    context.status() == 3,
                "callable active flag validation failed");
      if (mode == 5) {
        std::atomic<bool> cancelled{true};
        context.bindCancellation(&cancelled);
        require(!context.readRecursiveEnvironment(env, 8, &code) &&
                    !kalandra_recursive_function_dependency_v1(&context, &code, env, 1),
                "cancelled callable validation executed");
      }
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "environment read cleanup leaked");
    }
    std::cout << "recursive callable bridge passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
