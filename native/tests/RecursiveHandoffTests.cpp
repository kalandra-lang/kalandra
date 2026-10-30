#include "../runtime/String.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
static const char rootKey = 0, nodeKey = 0, environmentKey = 0, codeKey = 0;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(const Invocation &context) {
  require(!context.live() && context.allocations() == context.releases(), "handoff owners leaked");
}
struct Node {
  std::int64_t value;
  const void *next;
};
struct Array {
  void *allocated, *data;
  std::int64_t offset, size, stride;
};
struct Cell {
  const void *code, *environment;
};
struct Root {
  const void *node;
  Array callbacks;
};
template <class T> static std::span<const std::byte> bytes(const T &value) {
  return std::as_bytes(std::span{&value, 1});
}
static AllocationLease fixture(Invocation &producer) {
  StringDescriptor text;
  kalandra_string_literal_v1(&producer, &text, "é", 2);
  const void *dependency = text.allocated;
  auto *env =
      producer.makeRecursiveValueFromPointers(bytes(text), &environmentKey, {&dependency, 1});
  require(env && producer.bindRecursiveEnvironment(env, &codeKey), "environment fixture failed");
  const Cell cells[] = {{&codeKey, env}, {&codeKey, env}, {&codeKey, nullptr}};
  auto *storage = producer.allocateRecord(sizeof(cells));
  require(storage != nullptr, "array allocation failed");
  std::memcpy(storage, cells, sizeof(cells));
  Array array{storage, storage, 0, 3, 1};
  auto *owner = kalandra_recursive_function_array_dependency_v1(&producer, &array, 1);
  Node leaf{42, nullptr};
  auto *tail = producer.makeRecursiveValue(bytes(leaf), &nodeKey);
  Node parent{0, tail};
  auto *node = producer.makeRecursiveValueFromPointers(bytes(parent), &nodeKey, {&tail, 1});
  Root root{node, array};
  const void *dependencies[] = {node, owner};
  auto *result = producer.makeRecursiveValueFromPointers(bytes(root), &rootKey, dependencies);
  auto retained = producer.lease(result);
  require(bool(retained), "root fixture failed");
  return std::move(*retained);
}
static bool import(Invocation &destination, const AllocationLease &root) {
  return destination.retainRecursiveGraph(root, &rootKey, sizeof(Root), alignof(Root));
}
static void check(Invocation &destination, const AllocationLease &root) {
  Root payload;
  auto *view = kalandra_recursive_read_v1(&destination, root.bytes().data(), sizeof(Root),
                                          alignof(Root), &rootKey);
  require(view != nullptr, "imported root cannot be read");
  std::memcpy(&payload, view, sizeof(payload));
  Node node;
  view =
      kalandra_recursive_read_v1(&destination, payload.node, sizeof(Node), alignof(Node), &nodeKey);
  require(view != nullptr, "imported intermediate node cannot be read");
  std::memcpy(&node, view, sizeof(node));
  view = kalandra_recursive_read_v1(&destination, node.next, sizeof(Node), alignof(Node), &nodeKey);
  require(view != nullptr, "imported leaf cannot be read");
  std::memcpy(&node, view, sizeof(node));
  require(node.value == 42, "imported payload changed");
  kalandra_record_array_validate_v1(&destination, &payload.callbacks, sizeof(Cell), alignof(Cell));
  require(!destination.status(), "imported array backing is not registered");
  Cell cell;
  std::memcpy(&cell, payload.callbacks.data, sizeof(cell));
  auto *env =
      destination.readRecursiveEnvironment(cell.environment, sizeof(StringDescriptor), cell.code);
  require(env != nullptr, "imported closure environment cannot be read");
  StringDescriptor text;
  std::memcpy(&text, env, sizeof(text));
  require(kalandra_string_length_v1(&destination, &text) == 1 && !destination.status(),
          "imported nested String cannot be read");
}
struct Budget {
  std::uint64_t limit, used = 0, reservations = 0, refunds = 0;
  std::atomic<bool> *cancel = nullptr;
  bool valid = true;
  static bool reserve(void *state, std::uint64_t bytes) noexcept {
    auto &budget = *static_cast<Budget *>(state);
    ++budget.reservations;
    if (bytes > budget.limit - budget.used)
      return false;
    budget.used += bytes;
    if (budget.cancel)
      budget.cancel->store(true, std::memory_order_release);
    return true;
  }
  static void release(void *state, std::uint64_t bytes) noexcept {
    auto &budget = *static_cast<Budget *>(state);
    ++budget.refunds;
    if (bytes > budget.used) {
      budget.valid = false;
      return;
    }
    budget.used -= bytes;
  }
  void bind(Invocation &context) { context.bindRecursiveBudget(this, reserve, release); }
};
static void genericBudgetTests() {
  constexpr std::uint64_t size = 64;
  Budget probeBudget{1000000};
  Invocation probe;
  probeBudget.bind(probe);
  require(probe.allocate(size), "generic budget probe failed");
  const auto footprint = probe.recursiveBytes();
  require(footprint > size, "generic allocation omitted backing metadata");
  probe.settle();
  Invocation source;
  auto *original = source.allocateRecord(size);
  auto lease = source.lease(original);
  require(bool(lease), "generic retain source missing");
  for (unsigned operation = 0; operation < 3; ++operation)
    for (unsigned mode = 0; mode < 5; ++mode) {
      Budget budget{mode == 0 ? footprint - 1 : footprint};
      std::atomic<bool> cancelled{false};
      if (mode == 2)
        budget.cancel = &cancelled;
      Invocation context;
      budget.bind(context);
      context.bindCancellation(&cancelled);
      if (mode == 3)
        context.failAllocationAt(1);
      const auto references = lease->referenceCount();
      const bool ok = operation == 0   ? bool(context.allocate(size))
                      : operation == 1 ? bool(context.allocateRecord(size))
                                       : context.retainFrom(source, original);
      const bool expected = mode == 1 || mode == 4;
      require(ok == expected, "generic allocation/retain byte admission changed");
      if (expected) {
        require(budget.used == footprint && context.recursiveBytes() == footprint &&
                    context.live() == 1,
                "generic owner published without full reservation");
        if (mode == 4)
          require(!context.retainFrom(source, original) && context.status() == 2 &&
                      context.live() == 1 && budget.used == footprint,
                  "duplicate ordinary retain escaped exhausted byte budget");
        if (operation == 1) {
          context.releaseRecords();
          require(!context.live() && budget.used == footprint,
                  "record cleanup returned conservative budget before settlement");
        }
      } else {
        require(!context.live() && !context.allocations() && !budget.used &&
                    !context.recursiveBytes() && lease->referenceCount() == references &&
                    (mode == 2 ? context.poll() == -1 : context.status() == 2),
                "failed generic reservation published ownership or leaked bytes");
        require(budget.refunds == (mode == 2 ? 1u : 0u),
                "cancelled generic reservation was not refunded exactly once");
      }
      context.settle();
      context.settle();
      require(!budget.used && budget.valid && lease->referenceCount() == references,
              "generic settlement leaked charge or source reference");
      balanced(context);
    }
  // A newly allocated ordinary buffer becomes a recursive dependency without
  // charging that buffer again during the subsequent graph registration.
  {
    Budget budget{1000000};
    Invocation context;
    budget.bind(context);
    auto *buffer = context.allocateRecord(size);
    const void *dependency = buffer;
    auto *node =
        context.makeRecursiveValueFromPointers(bytes(dependency), &nodeKey, {&dependency, 1});
    auto root = context.lease(node);
    const auto used = budget.used;
    require(root &&
                context.retainRecursiveGraph(*root, &nodeKey, sizeof(dependency),
                                             alignof(decltype(dependency))) &&
                budget.used == used && budget.reservations == 2,
            "graph handoff recharged its task-created ordinary buffer");
    context.settle();
    require(!budget.used && budget.valid, "generic dependency reservation leaked");
    balanced(context);
  }
  source.settle();
}
static void constructionBudgetTests() {
  Node value{42, nullptr};
  Invocation source, probe;
  auto *pointer = source.makeRecursiveValue(bytes(value), &nodeKey);
  auto root = source.lease(pointer);
  require(root && probe.retainRecursiveGraph(*root, &nodeKey, sizeof(Node), alignof(Node)),
          "construction budget probe failed");
  const auto footprint = probe.recursiveBytes();
  probe.settle();
  for (unsigned mode = 0; mode < 5; ++mode) {
    Budget budget{mode == 0 ? footprint - 1 : footprint};
    std::atomic<bool> cancelled{false};
    if (mode == 2)
      budget.cancel = &cancelled;
    Invocation context;
    budget.bind(context);
    context.bindCancellation(&cancelled);
    const void *invalid = reinterpret_cast<void *>(0x1230);
    const auto references = root->referenceCount();
    const void *node =
        mode == 3 ? context.makeRecursiveValueFromPointers(bytes(value), &nodeKey, {&invalid, 1})
                  : context.makeRecursiveValue(bytes(value), &nodeKey);
    // The invalid-edge fixture needs table capacity in addition to a leaf.
    if (mode == 3) {
      require(!node && context.status() == 2 && !budget.used && !context.live(),
              "table-capacity budget rejection published a node");
    } else if (mode == 0 || mode == 2) {
      require(!node && !context.live() && !context.allocations() && !context.recursiveBytes() &&
                  !budget.used && (mode == 2 ? context.poll() : context.status() == 2),
              "construction budget failure published storage");
    } else {
      auto retained = context.lease(node);
      require(retained && context.recursiveBytes() == footprint && budget.used == footprint &&
                  context.retainRecursiveGraph(*retained, &nodeKey, sizeof(Node), alignof(Node)) &&
                  context.recursiveBytes() == footprint && budget.reservations == 1,
              "new node handoff double-charged construction");
      if (mode == 4) {
        require(!context.makeRecursiveValue(bytes(value), &nodeKey) && context.status() == 2 &&
                    context.live() == 1 && context.recursiveBytes() == footprint,
                "second node bypassed exhausted construction budget");
      }
    }
    context.settle();
    require(!budget.used && budget.valid && root->referenceCount() == references,
            "construction reservation leaked");
    balanced(context);
  }
  // A partial dependency retain must refund the full unpublished reservation.
  {
    Budget budget{footprint * 4};
    Invocation context;
    budget.bind(context);
    auto *child = context.makeRecursiveValue(bytes(value), &nodeKey);
    auto retained = context.lease(child);
    const auto used = budget.used, refs = retained->referenceCount();
    const void *dependencies[]{child, reinterpret_cast<void *>(0x1230)};
    require(!context.makeRecursiveValueFromPointers(bytes(value), &nodeKey, dependencies) &&
                context.status() == 3 && context.live() == 1 && budget.used == used &&
                context.recursiveBytes() == used && retained->referenceCount() == refs &&
                budget.refunds == 1,
            "failed child retention leaked reservation or dependency");
    context.settle();
    require(!budget.used && budget.valid && budget.refunds == 2,
            "partial construction refund changed");
    balanced(context);
  }
  source.settle();
}
static void budgetTests() {
  Invocation producer, probe;
  auto root = fixture(producer);
  require(import(probe, root), "budget probe failed");
  const auto footprint = probe.recursiveBytes();
  require(footprint > sizeof(Root) && probe.live() == 7, "empty graph reservation");
  probe.settle();
  require(!probe.recursiveBytes(), "settled local reservation remained");
  for (unsigned mode = 0; mode < 4; ++mode) {
    Budget budget{mode % 2 ? footprint : footprint - 1};
    Invocation receiver;
    if (mode >= 2) {
      for (unsigned duplicate = 0; duplicate < 2; ++duplicate)
        require(receiver.retainFrom(producer, const_cast<std::byte *>(root.bytes().data())),
                "pre-existing budget owner fixture failed");
    }
    budget.bind(receiver); // Qualify graph charging of owners predating budget binding.
    const auto live = receiver.live(), allocations = receiver.allocations();
    const auto references = root.referenceCount();
    const bool success = import(receiver, root);
    require(success == bool(mode % 2), "exact graph byte boundary changed");
    if (success) {
      require(receiver.recursiveBytes() == footprint && budget.used == footprint &&
                  import(receiver, root) && budget.reservations == 1 &&
                  receiver.live() == (mode >= 2 ? 8 : 7),
              "duplicate/pre-existing graph backing charged twice or omitted");
      check(receiver, root);
      receiver.release(const_cast<std::byte *>(root.bytes().data()));
      require(budget.used == footprint, "graph reservation refunded before settlement");
    } else {
      require(receiver.status() == 2 && !receiver.recursiveBytes() && !budget.used &&
                  receiver.live() == live && receiver.allocations() == allocations &&
                  root.referenceCount() == references,
              "budget rejection published partial ownership or retained backing");
    }
    receiver.settle();
    receiver.settle();
    require(!budget.used && budget.valid && budget.refunds == (success ? 1u : 0u),
            "graph settlement refund changed");
    balanced(receiver);
  }
  {
    Budget budget{footprint};
    std::atomic<bool> cancelled{false};
    budget.cancel = &cancelled;
    Invocation receiver;
    budget.bind(receiver);
    receiver.bindCancellation(&cancelled);
    const auto references = root.referenceCount();
    require(!import(receiver, root) && !receiver.live() && !receiver.allocations() &&
                !receiver.recursiveBytes() && !budget.used && budget.refunds == 1 &&
                root.referenceCount() == references,
            "post-reservation cancellation retained bytes or published ownership");
    receiver.settle();
    require(budget.valid && budget.refunds == 1, "cancelled reservation refunded twice");
  }
  // Null dependency slots have no edges, but their allocated table still costs bytes.
  {
    Invocation compact, padded;
    Node node{42, nullptr};
    auto *small = producer.makeRecursiveValue(bytes(node), &nodeKey);
    std::array<const void *, 1024> empty{};
    auto *large = producer.makeRecursiveValueFromPointers(bytes(node), &nodeKey, empty);
    auto first = producer.lease(small), second = producer.lease(large);
    require(first && second &&
                compact.retainRecursiveGraph(*first, &nodeKey, sizeof(Node), alignof(Node)) &&
                padded.retainRecursiveGraph(*second, &nodeKey, sizeof(Node), alignof(Node)) &&
                padded.recursiveBytes() - compact.recursiveBytes() == empty.size() * sizeof(void *),
            "unused dependency table capacity escaped accounting");
    compact.settle();
    padded.settle();
  }
  producer.settle();
  balanced(producer);
}
int main() {
  try {
    genericBudgetTests();
    constructionBudgetTests();
    budgetTests();
    {
      Invocation producer, receiver, onward;
      auto root = fixture(producer);
      producer.settle();
      balanced(producer);
      require(import(receiver, root), "handoff after producer settlement failed");
      check(receiver, root);
      const auto owners = receiver.live();
      require(owners == 7 && import(receiver, root) && receiver.live() == owners,
              "duplicate imports added redundant owners");
      auto packet = receiver.lease(root.bytes().data());
      require(packet && import(onward, *packet), "onward handoff failed");
      receiver.settle();
      balanced(receiver);
      root = {};
      check(onward, *packet);
      onward.settle();
      balanced(onward);
      require(packet->referenceCount() == 1, "handoff roots retained after final context cleanup");
    }
    // An existing root owner does not imply its transitive backing is registered.
    {
      Invocation producer, receiver;
      auto root = fixture(producer);
      require(receiver.retainFrom(producer, const_cast<std::byte *>(root.bytes().data())),
              "partial ledger fixture failed");
      require(import(receiver, root) && receiver.live() == 7, "partial ledger skipped descendants");
      producer.settle();
      check(receiver, root);
      receiver.settle();
      balanced(receiver);
    }
    for (std::uint64_t failAt = 1; failAt <= 9; ++failAt) {
      Invocation producer, receiver;
      auto root = fixture(producer);
      producer.settle();
      const auto references = root.referenceCount();
      auto *sentinel = receiver.allocateRecord(8);
      require(sentinel != nullptr, "sentinel fixture failed");
      auto original = receiver.lease(sentinel);
      receiver.failAllocationAt(receiver.allocations() + failAt);
      const bool success = import(receiver, root);
      require(success == (failAt == 9), "handoff allocation failure point changed");
      if (success)
        check(receiver, root);
      else
        require(receiver.status() == 2 && receiver.live() == 1 && receiver.allocations() == 1 &&
                    root.referenceCount() == references && original->referenceCount() == 2,
                "failed handoff changed pre-existing ledger or retained a partial graph");
      receiver.settle();
      balanced(receiver);
      require(root.referenceCount() == references, "handoff failed to release root edge");
    }
    for (unsigned invalid = 0; invalid < 9; ++invalid) {
      Invocation producer, receiver;
      auto root = fixture(producer);
      producer.settle();
      AllocationLease empty;
      bool accepted = false;
      if (invalid == 0)
        accepted = receiver.retainRecursiveGraph(empty, &rootKey, sizeof(Root), alignof(Root));
      if (invalid == 1)
        accepted = receiver.retainRecursiveGraph(root, &nodeKey, sizeof(Root), alignof(Root));
      if (invalid == 2)
        accepted = receiver.retainRecursiveGraph(root, &rootKey, sizeof(Root) - 1, alignof(Root));
      if (invalid == 3)
        accepted = receiver.retainRecursiveGraph(root, &rootKey, sizeof(Root), 3);
      if (invalid == 4) {
        receiver.fault(5);
        accepted = import(receiver, root);
      }
      if (invalid == 5) {
        receiver.expectedTaskError(1, 2, 0, 0);
        accepted = import(receiver, root);
      }
      if (invalid == 6) {
        receiver.expectedApplicationError("Problem", "Invalid");
        accepted = import(receiver, root);
      }
      if (invalid == 7) {
        receiver.settle();
        accepted = import(receiver, root);
      }
      if (invalid == 8) {
        auto *raw = receiver.allocateRecord(8);
        auto ordinary = receiver.lease(raw);
        accepted = receiver.retainRecursiveGraph(*ordinary, &rootKey, 8, 1);
      }
      require(!accepted && (invalid == 5 || invalid == 6 ? receiver.hasExpectedError()
                                                         : receiver.status() != 0),
              "invalid root/context accepted or original error lost");
      receiver.settle();
      balanced(receiver);
    }
    // Separate receivers read shared backing while the producer retires independently.
    for (unsigned trial = 0; trial < 16; ++trial) {
      Invocation producer;
      auto root = fixture(producer);
      auto other = producer.lease(root.bytes().data());
      std::atomic<bool> go{false}, good{true};
      auto run = [&](AllocationLease packet) {
        while (!go.load(std::memory_order_acquire))
          std::this_thread::yield();
        try {
          Invocation receiver;
          require(import(receiver, packet), "concurrent import failed");
          check(receiver, packet);
          receiver.settle();
          balanced(receiver);
        } catch (...) {
          good.store(false, std::memory_order_relaxed);
        }
      };
      std::thread first(run, std::move(root)), second(run, std::move(*other));
      go.store(true, std::memory_order_release);
      producer.settle();
      first.join();
      second.join();
      require(good.load() && !producer.live(), "concurrent handoff/read/retirement failed");
    }
    // A deep immutable chain is transferred and retired on a bounded native stack.
    {
      Invocation producer;
      Node leaf{0, nullptr};
      auto *pointer = producer.makeRecursiveValue(bytes(leaf), &nodeKey);
      auto root = producer.lease(pointer);
      auto terminal = producer.lease(pointer);
      for (std::int64_t i = 1; i <= 100000; ++i) {
        Node node{i, root->bytes().data()};
        auto *next = producer.makeRecursiveValue(bytes(node), &nodeKey, {&*root, 1});
        root = producer.lease(next);
        require(bool(root), "deep handoff fixture failed");
      }
      producer.settle();
      struct Transfer {
        AllocationLease root;
        bool good = false;
      } transfer{std::move(*root)};
      pthread_attr_t attributes;
      require(!pthread_attr_init(&attributes) &&
                  !pthread_attr_setstacksize(&attributes, 256 * 1024),
              "bounded stack setup");
      pthread_t worker;
      require(!pthread_create(
                  &worker, &attributes,
                  [](void *argument) -> void * {
                    auto &transfer = *static_cast<Transfer *>(argument);
                    Invocation receiver;
                    transfer.good = receiver.retainRecursiveGraph(transfer.root, &nodeKey,
                                                                  sizeof(Node), alignof(Node));
                    if (transfer.good) {
                      auto node = receiver.lease(transfer.root.bytes().data());
                      transfer.good = node && receiver.live() == 100001;
                      node.reset();
                    }
                    receiver.settle();
                    transfer.root = {};
                    transfer.good = transfer.good && !receiver.live() &&
                                    receiver.allocations() == receiver.releases();
                    return nullptr;
                  },
                  &transfer),
              "bounded stack handoff thread");
      pthread_attr_destroy(&attributes);
      require(!pthread_join(worker, nullptr) && transfer.good && terminal->referenceCount() == 1,
              "deep graph handoff or final retirement failed");
    }
    // Charge repeated dependency edges even when only two unique backings are visited.
    {
      Invocation producer, receiver, rejected;
      Node leaf{42, nullptr};
      auto *child = producer.makeRecursiveValue(bytes(leaf), &nodeKey);
      std::vector<const void *> edges(Invocation::maxRecursiveDependencies, child);
      auto *parent = producer.makeRecursiveValueFromPointers(bytes(leaf), &nodeKey, edges);
      auto accepted = producer.lease(parent);
      require(accepted &&
                  receiver.retainRecursiveGraph(*accepted, &nodeKey, sizeof(Node), alignof(Node)) &&
                  receiver.live() == 2,
              "exact graph edge limit rejected or duplicate owners added");
      auto *over = producer.makeRecursiveValueFromPointers(bytes(leaf), &nodeKey, {&parent, 1});
      auto root = producer.lease(over);
      require(root &&
                  !rejected.retainRecursiveGraph(*root, &nodeKey, sizeof(Node), alignof(Node)) &&
                  rejected.status() == 2 && !rejected.live() && !rejected.allocations(),
              "graph edge exhaustion published a partial ledger");
      receiver.settle();
      rejected.settle();
      producer.settle();
      balanced(receiver);
      balanced(rejected);
      balanced(producer);
    }
    {
      Invocation producer, receiver;
      auto root = fixture(producer);
      producer.settle();
      std::atomic<bool> cancelled{true};
      receiver.bindCancellation(&cancelled);
      require(!import(receiver, root) && !receiver.allocations() && receiver.poll(),
              "pre-cancelled handoff executed");
      receiver.settle();
      balanced(receiver);
    }
    {
      Invocation producer, receiver;
      auto root = fixture(producer);
      producer.settle();
      for (std::size_t i = 0; i < Invocation::maxRecursiveDependencies - 7; ++i)
        require(receiver.allocateRecord(1) != nullptr, "owner bound fixture failed");
      require(import(receiver, root) && receiver.live() == Invocation::maxRecursiveDependencies,
              "exact receiving owner bound rejected");
      require(import(receiver, root) && receiver.live() == Invocation::maxRecursiveDependencies,
              "idempotent import at owner bound rejected");
      // An unrelated allocation is allowed by the ordinary allocator, but a
      // subsequent graph transaction must reject its over-budget ledger.
      require(receiver.allocateRecord(1) != nullptr, "extra owner fixture failed");
      const auto live = receiver.live();
      require(!import(receiver, root) && receiver.status() == 2 && receiver.live() == live,
              "over-budget ledger changed during rejected import");
      receiver.settle();
      balanced(receiver);
    }
    for (unsigned trial = 0; trial < 8; ++trial) {
      Invocation producer, receiver;
      Node leaf{0, nullptr};
      auto *pointer = producer.makeRecursiveValue(bytes(leaf), &nodeKey);
      auto root = producer.lease(pointer);
      for (unsigned i = 0; i < 20000; ++i) {
        auto *next = producer.makeRecursiveValue(bytes(leaf), &nodeKey, {&*root, 1});
        root = producer.lease(next);
        require(bool(root), "cancellation graph fixture failed");
      }
      producer.settle();
      std::atomic<bool> cancelled{false};
      std::atomic<std::uint64_t> polls{0};
      receiver.bindCancellation(&cancelled);
      receiver.watchPolls(&polls);
      std::thread canceller([&] {
        while (polls.load(std::memory_order_relaxed) < 4096)
          std::this_thread::yield();
        cancelled.store(true, std::memory_order_release);
      });
      const bool success =
          receiver.retainRecursiveGraph(*root, &nodeKey, sizeof(Node), alignof(Node));
      canceller.join();
      require(success ? receiver.live() == 20001 : !receiver.live() && !receiver.allocations(),
              "cancelled handoff partially published owners");
      receiver.settle();
      balanced(receiver);
      require(root->referenceCount() == 1, "cancelled handoff retained staged root");
    }
    std::cout << "recursive handoff passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
