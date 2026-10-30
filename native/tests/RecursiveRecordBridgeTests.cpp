#include "../runtime/String.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" std::int64_t probe(Invocation *);
extern "C" std::int64_t wrong_key(Invocation *);
extern "C" std::int64_t forged(Invocation *);
extern "C" std::int64_t bad_reference(Invocation *);
extern "C" const void *task_error_box(std::uint64_t, std::uint64_t, Invocation *);
extern "C" const void *text_box(const StringDescriptor *, std::int64_t, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (std::uint64_t tag : {1u, 2u})
      for (std::uint64_t kind : {0u, 1u, 2u, 3u, 4u, 5u, UINT32_MAX}) {
        Invocation context;
        auto *root = task_error_box(kind, tag, &context);
        const bool valid = tag == 1 || (kind >= 1 && kind <= 4);
        require(bool(root) == valid && context.status() == (valid ? 0 : 3),
                "TaskError field validation ignored active kind or rejected inactive data");
        if (!valid)
          require(!context.live() && !context.allocations(),
                  "invalid TaskError field published backing");
        context.settle();
        require(!context.live() && context.allocations() == context.releases(),
                "TaskError field cleanup leaked");
      }
    for (unsigned fail = 0; fail <= 5; ++fail) {
      Invocation context;
      if (fail)
        context.failAllocationAt(fail);
      auto result = probe(&context);
      require(fail && fail <= 4 ? result == 0 && context.status() == 2
                                : result == 184 && !context.status(),
              "compiled recursive result/failure");
      require(context.callDepth() == 0, "checked call depth leaked");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "compiled recursive cleanup leaked");
    }
    for (auto run : {wrong_key, forged, bad_reference}) {
      Invocation context;
      require(run(&context) == 0 && context.status() == 3 && context.callDepth() == 0,
              "bad recursive read reached an unchecked load");
      context.settle();
      require(!context.live(), "bad-read cleanup leaked");
    }
    {
      Invocation context;
      std::atomic<bool> cancelled{true};
      context.bindCancellation(&cancelled);
      require(probe(&context) == 0 && !context.live() && context.callDepth() == 0,
              "cancelled compiled constructor executed");
    }
    for (unsigned mode = 0; mode < 12; ++mode) {
      Invocation context, foreign;
      StringDescriptor value;
      const std::string bytes("é\0👩", 7);
      kalandra_string_literal_v1(&context, &value, bytes.data(), bytes.size());
      require(!context.status(), "String fixture allocation failed");
      auto buffer = context.lease(value.allocated);
      auto invalid = value;
      if (mode == 1 || mode == 2)
        invalid = {reinterpret_cast<void *>(0x1234), reinterpret_cast<char *>(0x1234), -1};
      if (mode == 3)
        invalid.byteLength = 1000;
      if (mode == 4)
        invalid.data++;
      if (mode == 5) {
        kalandra_string_literal_v1(&foreign, &invalid, bytes.data(), bytes.size());
      }
      if (mode == 6)
        invalid = {};
      if (mode >= 7 && mode <= 9)
        context.failAllocationAt(context.allocations() + mode - 6);
      if (mode == 10) {
        auto *bad = static_cast<char *>(context.allocateRecord(1));
        require(bad, "invalid UTF-8 fixture allocation failed");
        *bad = char(0xff);
        invalid = {bad, bad, 1};
      }
      if (mode == 11)
        invalid.byteLength = 0;
      auto *root = text_box(&invalid, mode == 1 ? 1 : 2, &context);
      if (mode == 2 || mode == 3 || mode == 4 || mode == 5 || mode == 7 || mode == 8 ||
          mode >= 10) {
        require(!root && context.status() == (mode == 7 || mode == 8 ? 2 : 3),
                "invalid String or failed box was published");
        context.settle();
        require(buffer->referenceCount() == 1, "failed box retained String backing");
      } else {
        require(root && !context.status(), "String bridge box failed");
        auto retained = context.lease(root);
        require(bool(retained), "String root lease missing");
        context.settle();
        const bool owned = mode == 0 || mode == 9;
        require(buffer->referenceCount() == (owned ? 2 : 1),
                "active String dependency was lost or inactive String retained");
        if (owned) {
          auto child = retained->dependency(0);
          require(child && child->bytes().size() == bytes.size() &&
                      std::memcmp(child->bytes().data(), bytes.data(), bytes.size()) == 0,
                  "String bytes did not survive invocation settlement");
        }
        retained.reset();
        require(buffer->referenceCount() == 1, "String dependency survived final root release");
      }
      require(!context.live() && context.allocations() == context.releases() &&
                  context.callDepth() == 0,
              "String bridge cleanup leaked");
    }
    {
      Invocation context;
      require(!kalandra_recursive_string_dependency_v1(&context, nullptr, 0) && !context.status(),
              "inactive String descriptor was inspected");
      require(!kalandra_recursive_string_dependency_v1(&context, nullptr, 2) &&
                  context.status() == 3,
              "invalid active flag accepted");
    }
    static const char key = 0;
    for (unsigned mode = 0; mode < 6; ++mode) {
      Invocation context, foreign;
      std::uint64_t payload = 42;
      auto *child = kalandra_recursive_make_v1(&context, &payload, sizeof(payload),
                                               alignof(decltype(payload)), &key, nullptr, 0);
      require(child, "raw bridge child construction");
      auto observer = context.lease(child);
      std::array<const void *, 3> dependencies{child, nullptr, child};
      if (mode == 0)
        dependencies[2] = reinterpret_cast<void *>(0x1234);
      if (mode == 1)
        dependencies[2] = foreign.allocateRecord(8);
      auto *parent = kalandra_recursive_make_v1(
          &context, &payload, sizeof(payload), alignof(decltype(payload)), &key,
          mode == 2 ? nullptr : dependencies.data(),
          mode == 3 ? Invocation::maxRecursiveDependencies + 1 : 3);
      require(mode < 4 ? !parent && context.status() == 3 && observer->referenceCount() == 2
                       : parent && !context.status() && observer->referenceCount() == 4,
              "dependency table validation or rollback failed");
      if (mode == 4)
        require(!kalandra_recursive_read_v1(&context, parent, 8, 3, &key) && context.status() == 3,
                "bad projection alignment accepted");
      if (mode == 5)
        require(!kalandra_recursive_read_v1(&context, parent, 7, 1, &key) && context.status() == 3,
                "bad projection size accepted");
      context.settle();
      foreign.settle();
      require(observer->referenceCount() == 1 && !context.live(),
              "raw bridge retained partial child edges");
    }
    std::cout << "compiled recursive record bridge passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
