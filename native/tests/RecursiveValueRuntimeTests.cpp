#include "../runtime/Runtime.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
static const char nodeKey = 0, otherKey = 0;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Node {
  std::int64_t value;
  const void *next;
};
static std::span<const std::byte> bytes(const Node &node) {
  return std::as_bytes(std::span(&node, 1));
}
static AllocationLease make(Invocation &context, std::int64_t value,
                            std::span<const AllocationLease> children = {}) {
  Node payload{value, children.empty() ? nullptr : children.front().bytes().data()};
  const auto *pointer = context.makeRecursiveValue(bytes(payload), &nodeKey, children);
  require(pointer, "recursive construction failed");
  auto lease = context.lease(pointer);
  require(bool(lease), "published node is not invocation owned");
  return std::move(*lease);
}
static Node read(const AllocationLease &value) {
  auto view = value.recursiveBytes(&nodeKey, sizeof(Node), alignof(Node));
  require(bool(view), "typed recursive projection failed");
  Node result;
  std::memcpy(&result, view->data(), sizeof(result));
  return result;
}
static void balanced(const Invocation &context) {
  require(!context.live() && context.allocations() == context.releases(),
          "invocation owners leaked");
}
int main() {
  try {
    {
      Invocation context;
      Node payload{42, nullptr};
      auto *pointer = context.makeRecursiveValue(bytes(payload), &nodeKey);
      require(pointer, "leaf construction");
      payload.value = -1;
      auto root = context.lease(pointer);
      require(root && read(*root).value == 42, "payload aliases writable constructor input");
      require(!root->recursiveBytes(nullptr, sizeof(Node), alignof(Node)) &&
                  !root->recursiveBytes(&otherKey, sizeof(Node), alignof(Node)) &&
                  !root->recursiveBytes(&nodeKey, sizeof(Node) - 1, alignof(Node)) &&
                  !root->recursiveBytes(&nodeKey, sizeof(Node), 0) &&
                  !root->recursiveBytes(&nodeKey, sizeof(Node), 3) &&
                  !root->recursiveBytes(&nodeKey, sizeof(Node), 128),
              "forged projection accepted");
      require(!root->dependency(0), "leaf has dependency");
      context.releaseRecords();
      balanced(context);
      require(read(*root).value == 42, "lease did not preserve leaf after cleanup");
      context.settle();
    }
    {
      Invocation context;
      auto leaf = make(context, 7);
      std::array<AllocationLease, 2> duplicate;
      duplicate[0] = std::move(leaf);
      duplicate[1] = std::move(*context.lease(duplicate[0].bytes().data()));
      auto parent = make(context, 42, duplicate);
      duplicate = {};
      auto child = parent.dependency(1);
      require(child && read(*child).value == 7 && !parent.dependency(2),
              "duplicate dependency projection");
      context.settle();
      balanced(context);
      parent = {};
      require(read(*child).value == 7 && child->referenceCount() == 1,
              "projected child did not outlive parent or duplicate edge leaked");
    }
    {
      // Two parents retain one tail; updating creates a new parent without mutation.
      Invocation context;
      std::array<AllocationLease, 1> tail{make(context, 10)};
      auto observer = std::move(*context.lease(tail[0].bytes().data()));
      std::array<AllocationLease, 2> branches;
      branches[0] = make(context, 20, tail);
      branches[1] = make(context, 30, tail);
      auto root = make(context, 40, branches);
      auto updated = make(context, 50, branches);
      tail = {};
      branches = {};
      context.settle();
      balanced(context);
      require(read(root).value == 40 && read(updated).value == 50,
              "immutable update changed old value");
      auto left = root.dependency(0), right = updated.dependency(1);
      root = {};
      updated = {};
      auto a = left->dependency(0), b = right->dependency(0);
      require(a && b && a->bytes().data() == b->bytes().data() && read(*a).value == 10,
              "shared diamond tail lost identity");
      left.reset();
      right.reset();
      a.reset();
      b.reset();
      require(observer.referenceCount() == 1,
              "shared graph left retained edges after final release");
    }
    {
      // Nonrecursive backing (Strings, collections, closure environments) can be retained too.
      Invocation context;
      auto *storage = context.allocateRecord(8);
      require(storage, "ordinary dependency allocation");
      std::memset(storage, 0x5a, 8);
      std::array<AllocationLease, 1> dependencies{std::move(*context.lease(storage))};
      require(!dependencies[0].recursiveBytes(&nodeKey, 8, 1), "ordinary storage forged as node");
      auto root = make(context, 1, dependencies);
      dependencies = {};
      context.settle();
      auto data = root.dependency(0);
      root = {};
      require(data && data->bytes()[7] == std::byte{0x5a},
              "ordinary backing lost on recursive cleanup");
    }
    for (unsigned invalid = 0; invalid < 4; ++invalid) {
      Invocation context;
      Node payload{1, nullptr};
      AllocationLease empty;
      auto result =
          invalid == 0   ? context.makeRecursiveValue(bytes(payload), nullptr)
          : invalid == 1 ? context.makeRecursiveValue({}, &nodeKey)
          : invalid == 2
              ? context.makeRecursiveValue(bytes(payload), &nodeKey, std::span(&empty, 1))
              : [&] {
                  std::vector<AllocationLease> tooMany(Invocation::maxRecursiveDependencies + 1);
                  return context.makeRecursiveValue(bytes(payload), &nodeKey, tooMany);
                }();
      require(!result && context.status() == 3 && !context.allocations(),
              "invalid constructor published node");
      context.settle();
      balanced(context);
    }
    for (std::uint64_t failAt = 1; failAt <= 3; ++failAt) {
      Invocation context;
      context.failAllocationAt(failAt);
      Node payload{1, nullptr};
      const auto *first = context.makeRecursiveValue(bytes(payload), &nodeKey);
      if (first) {
        std::array<AllocationLease, 1> child{std::move(*context.lease(first))};
        const auto *second = context.makeRecursiveValue(bytes(payload), &nodeKey, child);
        require((second != nullptr) == (failAt == 3), "allocation failure point ignored");
        require(read(child[0]).value == 1, "failed parent damaged existing child");
      } else
        require(failAt == 1, "unexpected allocation failure");
      require(context.status() == (failAt < 3 ? 2 : 0), "allocation fault lost");
      context.settle();
      balanced(context);
    }
    {
      Invocation context;
      Node payload{1, nullptr};
      std::atomic<bool> cancel{true};
      context.bindCancellation(&cancel);
      require(!context.makeRecursiveValue(bytes(payload), &nodeKey) && !context.allocations(),
              "pre-cancelled constructor published a node");
      context.settle();
      balanced(context);
    }
    {
      // Cancellation may win during edge retention or payload copying, or after publication.
      Invocation context;
      std::array<AllocationLease, 1> child{make(context, 7)};
      std::vector<std::byte> payload(32 * 1024 * 1024, std::byte{0x41});
      std::atomic<bool> cancel{false}, started{false};
      std::atomic<std::uint64_t> polls{0};
      context.bindCancellation(&cancel);
      context.watchPolls(&polls);
      std::thread canceller([&] {
        started.store(true, std::memory_order_release);
        while (polls.load(std::memory_order_relaxed) < 4)
          std::this_thread::yield();
        cancel.store(true, std::memory_order_release);
      });
      while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
      auto *result = context.makeRecursiveValue(payload, &nodeKey, child);
      canceller.join();
      if (result) {
        auto complete = context.lease(result);
        require(complete && complete->bytes().size() == payload.size() &&
                    complete->bytes().back() == std::byte{0x41},
                "partially published cancelled node");
      }
      require(read(child[0]).value == 7 && child[0].referenceCount() == (result ? 3 : 2),
              "cancelled constructor leaked a partial dependency");
      context.settle();
      balanced(context);
      require(child[0].referenceCount() == 1, "cancelled/completed parent dependency leaked");
    }
    {
      Invocation context;
      AllocationLease root = make(context, 0);
      auto terminal = std::move(*context.lease(root.bytes().data()));
      constexpr std::int64_t depth = 100000;
      for (std::int64_t value = 1; value <= depth; ++value) {
        std::array<AllocationLease, 1> child{std::move(root)};
        root = make(context, value, child);
      }
      context.settle();
      balanced(context);
      require(read(root).value == depth, "deep root did not survive invocation");
      auto projected = root.dependency(0);
      require(projected && read(*projected).value == depth - 1, "deep child projection");
      projected.reset();
      pthread_attr_t attributes;
      require(pthread_attr_init(&attributes) == 0, "stack attributes");
      require(pthread_attr_setstacksize(&attributes, 256 * 1024) == 0, "bounded release stack");
      pthread_t thread;
      require(pthread_create(
                  &thread, &attributes,
                  [](void *value) -> void * {
                    *static_cast<AllocationLease *>(value) = {};
                    return nullptr;
                  },
                  &root) == 0,
              "release thread creation");
      pthread_attr_destroy(&attributes);
      require(pthread_join(thread, nullptr) == 0 && !root, "deep graph cleanup failed");
      require(terminal.referenceCount() == 1 && read(terminal).value == 0,
              "deep graph cleanup retained a child edge");
    }
    // Independent retained roots can be retired concurrently, including shared tails.
    for (unsigned trial = 0; trial < 64; ++trial) {
      Invocation context;
      std::array<AllocationLease, 1> tail{make(context, 7)};
      auto left = make(context, 10, tail), right = make(context, 20, tail);
      tail = {};
      std::atomic<bool> go{false};
      std::thread a([root = std::move(left), &go]() mutable {
        while (!go.load(std::memory_order_acquire))
          std::this_thread::yield();
        root = {};
      });
      std::thread b([root = std::move(right), &go]() mutable {
        while (!go.load(std::memory_order_acquire))
          std::this_thread::yield();
        root = {};
      });
      context.settle();
      balanced(context);
      go.store(true, std::memory_order_release);
      a.join();
      b.join();
    }
    for (unsigned failure = 0; failure < 3; ++failure) {
      Invocation context;
      if (failure == 0)
        context.fault(5);
      else if (failure == 1)
        context.expectedTaskError(1, 2, 0, 0);
      else
        context.expectedApplicationError("Problem", "Invalid");
      Node payload{1, nullptr};
      require(!context.makeRecursiveValue(bytes(payload), &nodeKey) && !context.allocations() &&
                  (failure == 0 ? context.status() == 5
                                : context.hasExpectedError() && context.status() == 0),
              "failed invocation accepted construction or lost error channel");
      context.settle();
      balanced(context);
    }
    {
      Invocation context;
      context.settle();
      Node payload{1, nullptr};
      require(!context.makeRecursiveValue(bytes(payload), &nodeKey) && context.status() == 3,
              "settled invocation accepted construction");
    }
    std::cout << "recursive value runtime passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
