#include "../runtime/String.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
using namespace kalandra::runtime;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static StringDescriptor make(Invocation &ctx, const std::string &text) {
  StringDescriptor value;
  kalandra_string_literal_v1(&ctx, &value, text.data(), text.size());
  return value;
}
static StringDescriptor concat(Invocation &ctx, const StringDescriptor &a,
                               const StringDescriptor &b) {
  StringDescriptor value;
  kalandra_string_concat_v1(&ctx, &value, &a, &b);
  return value;
}
int main() {
  try {
    require(combinedStringBytes(0, 0) == 0 && combinedStringBytes(INT64_MAX, 0) == INT64_MAX &&
                combinedStringBytes(INT64_MAX - 1, 1) == INT64_MAX &&
                !combinedStringBytes(INT64_MAX, 1) && !combinedStringBytes(UINT64_MAX, 0) &&
                !combinedStringBytes(0, UINT64_MAX),
            "combined length arithmetic overflow");
    Invocation ctx;
    auto empty = make(ctx, "");
    auto left = make(ctx, "سلام");
    auto right = make(ctx, std::string("\0👩‍💻", 12));
    const std::string expected("سلام\0👩‍💻", 20);
    auto result = concat(ctx, left, right);
    require(validateString(ctx, result) && result.byteLength == 20 &&
                !std::memcmp(result.data, expected.data(), 20),
            "concat changed bytes");
    require(left.byteLength == 8 && right.byteLength == 12, "concat mutated source descriptors");
    auto plain = make(ctx, expected);
    auto allocations = ctx.allocations();
    require(kalandra_string_equal_v1(&ctx, &result, &plain) == 1 &&
                ctx.allocations() == allocations,
            "equality changed storage or value");
    require(kalandra_string_equal_v1(&ctx, &left, &right) == 0, "unequal lengths equal");
    auto composed = make(ctx, "é"), decomposed = make(ctx, "é");
    require(kalandra_string_equal_v1(&ctx, &composed, &decomposed) == 0,
            "equality normalized Unicode");
    auto zero = concat(ctx, empty, empty);
    require(!zero.data && !zero.byteLength, "empty concat not canonical");
    auto prefix = concat(ctx, empty, result), suffix = concat(ctx, result, empty);
    require(kalandra_string_equal_v1(&ctx, &prefix, &suffix) == 1, "empty concat changed value");
    kalandra_string_concat_v1(&ctx, &left, &left, &right);
    require(kalandra_string_equal_v1(&ctx, &left, &result) == 1, "aliased output concat failed");
    auto held = ctx.lease(result.allocated);
    ctx.settle();
    require(held && !std::memcmp(held->bytes().data(), expected.data(), 20) && !ctx.live() &&
                ctx.allocations() == ctx.releases(),
            "result lifetime/cleanup failed");
    for (const auto &[first, second, expectedOrder] :
         {std::tuple<std::string, std::string, int>{"", "", 0},
          {"a", "aa", -1},
          {"aa", "a", 1},
          {"a", "b", -1},
          {"é", "e\xcc\x81", 1},
          {"😀", "é", 1},
          {std::string("a\0z", 3), std::string("a\0b", 3), 1},
          {std::string(4096, 'x') + "a", std::string(4096, 'x') + "b", -1}}) {
      Invocation ordered;
      auto a = make(ordered, first), b = make(ordered, second);
      require(kalandra_string_compare_v1(&ordered, &a, &b) == expectedOrder &&
                  kalandra_string_compare_v1(&ordered, &b, &a) == -expectedOrder &&
                  !ordered.status(),
              "String lexicographic ordering differs from reference");
      ordered.settle();
      require(!ordered.live() && ordered.allocations() == ordered.releases(),
              "String ordering leaked storage");
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
      Invocation bad;
      auto a = make(bad, "a"), b = make(bad, std::string(4097, 'z'));
      static_cast<char *>(b.allocated)[4096] = char(0x80);
      if (mode == 1)
        a = b;
      if (mode == 2)
        a.byteLength = 0;
      require(kalandra_string_compare_v1(&bad, &a, &b) == 0 && bad.status() == 3,
              "ordering shortcut hid invalid String backing");
      bad.settle();
      require(!bad.live(), "invalid ordering leaked storage");
    }
    // Full validation precedes unequal-length, unequal-prefix and same-pointer shortcuts.
    for (unsigned mode = 0; mode < 6; ++mode) {
      Invocation bad;
      auto a = make(bad, "a"), b = make(bad, std::string(4097, 'b'));
      auto *raw = static_cast<char *>(b.allocated);
      raw[4096] = char(0x80);
      if (mode == 1)
        a = b;
      if (mode == 2)
        a.byteLength = 0;
      if (mode == 3)
        b.allocated = reinterpret_cast<void *>(0x1000);
      if (mode == 4)
        b.data++;
      if (mode == 5)
        b.byteLength = -1;
      require(kalandra_string_equal_v1(&bad, &a, &b) == 0 && bad.status() == 3,
              "invalid String hidden by equality shortcut");
      bad.settle();
      require(!bad.live(), "invalid comparison leaked");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
      Invocation bad;
      auto a = make(bad, "a"), b = make(bad, "b");
      if (mode == 0)
        b.data++;
      if (mode == 1)
        b.byteLength = INT64_MAX;
      if (mode == 2)
        static_cast<char *>(b.allocated)[0] = char(0x80);
      if (mode == 3)
        bad.settle();
      auto out = concat(bad, a, b);
      require(bad.status() == 3 && !out.allocated && !out.byteLength,
              "bad concat published a result");
      bad.settle();
      require(!bad.live() && bad.allocations() == bad.releases(), "bad concat leaked");
    }
    for (unsigned failure = 1; failure <= 6; ++failure) {
      Invocation failed;
      failed.failAllocationAt(failure);
      auto a = make(failed, "a"), b = make(failed, "b");
      auto out = concat(failed, a, b);
      if (failed.status())
        require(!out.data && !out.byteLength, "failed concat published storage");
      else
        require(validateString(failed, out) && out.byteLength == 2,
                "successful concat sweep invalid");
      failed.settle();
      require(!failed.live() && failed.allocations() == failed.releases(), "failed concat leaked");
    }
    for (unsigned kind = 0; kind < 3; ++kind) {
      Invocation work;
      std::string bytes(16 * 1024 * 1024, 'a');
      auto a = make(work, bytes), b = make(work, bytes);
      std::atomic<bool> token = false;
      std::atomic<std::uint64_t> polls = 0;
      work.bindCancellation(&token);
      work.watchPolls(&polls);
      // Two operand-validation scans, then interrupt comparison/copy after its first chunks.
      std::thread cancel([&] {
        while (polls.load(std::memory_order_acquire) < 131080)
          std::this_thread::yield();
        token = true;
      });
      // The watcher may run after completion. A concurrent cancellation permits
      // either a complete correct result or no published result, never a partial one.
      auto equal = kind == 0 ? kalandra_string_equal_v1(&work, &a, &b) : 0;
      StringDescriptor out{};
      if (kind == 1)
        out = concat(work, a, b);
      if (kind == 2)
        require(kalandra_string_compare_v1(&work, &a, &b) == 0,
                "cancelled or equal String ordering changed its result");
      cancel.join();
      if (kind == 0)
        require(equal == 0 || equal == 1, "invalid equality result");
      else if (kind == 1)
        require((!out.data && !out.byteLength) ||
                    (out.byteLength == 2 * static_cast<std::int64_t>(bytes.size()) &&
                     !std::memcmp(out.data, bytes.data(), bytes.size()) &&
                     !std::memcmp(out.data + bytes.size(), bytes.data(), bytes.size())),
                "concurrent cancellation published partial concat");
      // Once cancellation is visible, every subsequent operation must reject it.
      require(kalandra_string_equal_v1(&work, &a, &b) == 0,
              "visible cancellation ignored by equality");
      require(kalandra_string_compare_v1(&work, &a, &b) == 0,
              "visible cancellation ignored by ordering");
      auto stopped = concat(work, a, b);
      require(!stopped.data && !stopped.byteLength,
              "visible cancellation ignored by concat");
      require(work.poll() != 0, "operation ignored cancellation");
      work.settle();
      require(!work.live() && work.allocations() == work.releases(), "cancelled operation leaked");
    }
    std::cout << "String equality/concat: exact bytes, immutable inputs, checked counts, retained "
                 "results, full validation, failure and in-operation cancellation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
