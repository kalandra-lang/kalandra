#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct Mixed {
  std::int64_t tag;
  std::array<Failure, 4> failures;
};
static_assert(sizeof(Mixed) == 136 && sizeof(Failure) == 32);
extern "C" void kalandra_narrow_probe(Failure *, const Mixed *, const Failure *, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static bool same(Failure a, Failure b) {
  return a.kind == b.kind && a.code == b.code && a.line == b.line && a.column == b.column;
}
static void check(const Mixed &input, bool valid, Failure expected) {
  const Failure fallback{TaskFailure::Cancelled, 91, 23, 29};
  Invocation invocation;
  alignas(Failure) std::array<std::byte, sizeof(Failure)> storage;
  storage.fill(std::byte{0x5a});
  const auto before = storage;
  kalandra_narrow_probe(reinterpret_cast<Failure *>(storage.data()), &input, &fallback,
                        &invocation);
  if (valid) {
    Failure actual;
    std::memcpy(&actual, storage.data(), sizeof(actual));
    require(invocation.status() == 0 && same(actual, expected),
            "narrowing changed TaskError kind/code/source metadata");
  } else
    require(invocation.status() == 3 && storage == before,
            "invalid union metadata escaped a checked projection");
  invocation.settle();
  require(!invocation.live() && invocation.allocations() == invocation.releases(),
          "narrowing retained unexpected storage");
}
int main() {
  try {
    for (unsigned kind = 1; kind <= 4; ++kind) {
      Mixed input{};
      input.tag = kind + 1;
      const Failure expected{static_cast<TaskFailure>(kind), 42, 17, 9};
      input.failures[kind - 1] = expected;
      check(input, true, expected);
      for (auto bad : {0U, 99U, kind == 1 ? 2U : 1U}) {
        input.failures[kind - 1].kind = static_cast<TaskFailure>(bad);
        check(input, false, {});
      }
    }
    for (auto tag : {0, 99}) {
      Mixed input{};
      input.tag = tag;
      check(input, false, {});
    }
    Mixed application{};
    application.tag = 1;
    for (auto &field : application.failures)
      field.kind = static_cast<TaskFailure>(99);
    check(application, true, {TaskFailure::Cancelled, 91, 23, 29});
    std::cout << "Checked narrowing: four TaskError kinds, original metadata, inactive payloads "
                 "and 14 malformed active cases passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
