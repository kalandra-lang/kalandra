#include "../runtime/Format.h"
#include "../runtime/Parse.h"
#include <atomic>
#include <bit>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "parse assertion %d: %s\n", __LINE__, #x);                              \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
static ParseScalarResult parse(Invocation &ctx, unsigned kind, const std::string &text) {
  StringDescriptor input;
  kalandra_string_literal_v1(&ctx, &input, text.data(), text.size());
  ParseScalarResult result;
  kalandra_parse_scalar_v1(&ctx, &result, kind, &input);
  return result;
}
static ParseScalarResult value(unsigned kind, const std::string &text) {
  Invocation ctx;
  auto result = parse(ctx, kind, text);
  CHECK(!ctx.status());
  ctx.settle();
  CHECK(!ctx.live());
  return result;
}
static void success(unsigned kind, const std::string &text, std::uint64_t bits) {
  auto result = value(kind, text);
  if (result.status != ParseStatus::Success || result.bits != bits) {
    std::fprintf(stderr, "kind %u input %s got %u/%llx expected %llx\n", kind, text.c_str(),
                 unsigned(result.status), (unsigned long long)result.bits,
                 (unsigned long long)bits);
    std::abort();
  }
}
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--batch") {
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto split = line.find('\t');
      CHECK(split != std::string::npos);
      auto r = value(unsigned(std::stoul(line.substr(0, split))), line.substr(split + 1));
      std::printf("%u %llu %016llx\n", unsigned(r.status), (unsigned long long)r.offset,
                  (unsigned long long)r.bits);
    }
    return 0;
  }
  for (unsigned kind = 1; kind <= 8; ++kind) {
    success(kind, "+00012", 12);
    CHECK(value(kind, "").status == ParseStatus::Invalid);
    CHECK(value(kind, " 12").offset == 0);
    CHECK(value(kind, "12 ").offset == 2);
    CHECK(value(kind, "999999999999999999999999999x").status == ParseStatus::Invalid);
    CHECK(value(kind, "999999999999999999999999999").status == ParseStatus::OutOfRange);
  }
  success(1, "-128", 128);
  success(1, "127", 127);
  CHECK(value(1, "128").status == ParseStatus::OutOfRange);
  success(2, "-32768", 32768);
  success(2, "32767", 32767);
  success(3, "-2147483648", 2147483648ULL);
  success(3, "2147483647", 2147483647);
  success(4, "-9223372036854775808", 1ULL << 63);
  success(4, "9223372036854775807", INT64_MAX);
  success(5, "255", 255);
  success(6, "65535", 65535);
  success(7, "4294967295", UINT32_MAX);
  success(8, "18446744073709551615", UINT64_MAX);
  for (unsigned kind = 5; kind <= 8; ++kind)
    CHECK(value(kind, "-0").status == ParseStatus::Invalid);
  success(9, "0.1", 0x3dcccccd);
  success(10, "0.1", 0x3fb999999999999aULL);
  success(9, "16777217", 0x4b800000);
  success(10, "9007199254740993", 0x4340000000000000ULL);
  success(9, ".5", 0x3f000000);
  success(10, "1.", 0x3ff0000000000000ULL);
  success(9, "-0e999999999999999999999", 1ULL << 31);
  success(10, "-0.00e-999999999999999999", 1ULL << 63);
  success(9, "inf", 0x7f800000);
  success(10, "-inf", 0xfff0000000000000ULL);
  success(9, "+nan", 0x7fc00000);
  success(10, "-nan", 0xfff8000000000000ULL);
  success(9, "1.401298464324817e-45", 1);
  success(10, "4.9406564584124654e-324", 1);
  CHECK(value(9, "1e-99").status == ParseStatus::OutOfRange);
  CHECK(value(10, "1e999").status == ParseStatus::OutOfRange);
  CHECK(value(10, "1e999junk").status == ParseStatus::Invalid);
  CHECK(value(10, "1e+").offset == 3);
  CHECK(value(10, ".x").offset == 1);
  CHECK(value(10, "nan(payload)").offset == 3);
  success(11, "true", 1);
  success(11, "false", 0);
  CHECK(value(11, "True").status == ParseStatus::Invalid);
  CHECK(value(11, "falsex").offset == 5);
  // Exponent cancellation must not depend on source length or integer overflow.
  success(10, "0." + std::string(100000, '0') + "1e100001", 0x3ff0000000000000ULL);
  success(10, "1" + std::string(100000, '0') + "e-100000", 0x3ff0000000000000ULL);
  // Hardware rounding state and exception flags must be untouched.
  const auto original = std::fegetround();
  for (auto mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    CHECK(std::fesetround(mode) == 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_INVALID);
    const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
    success(9, "0.1", 0x3dcccccd);
    success(10, "0.1", 0x3fb999999999999aULL);
    CHECK(std::fegetround() == mode && std::fetestexcept(FE_ALL_EXCEPT) == flags);
  }
  CHECK(std::fesetround(original) == 0);
  std::feclearexcept(FE_ALL_EXCEPT);
  {
    Invocation ctx;
    StringDescriptor input;
    const std::string text(4 * 1024 * 1024, '0');
    kalandra_string_literal_v1(&ctx, &input, text.data(), text.size());
    std::atomic<std::uint64_t> polls{0};
    ctx.watchPolls(&polls);
    ParseScalarResult result;
    kalandra_parse_scalar_v1(&ctx, &result, 10, &input);
    CHECK(result.status == ParseStatus::Success && !result.bits && polls > text.size() / 1024);
    std::atomic<bool> cancelled{true};
    ctx.bindCancellation(&cancelled);
    result.bits = 123;
    kalandra_parse_scalar_v1(&ctx, &result, 10, &input);
    CHECK(!result.bits && ctx.poll());
    ctx.fault(1);
    kalandra_parse_scalar_v1(&ctx, &result, 10, &input);
    CHECK(ctx.status() == 1);
    ctx.settle();
    CHECK(!ctx.live());
  }
  // Exercise the public formatter/parser pair on finite bit patterns. The
  // independent rational oracle separately proves these are not matching bugs.
  std::uint64_t random = 0x56fb73d610a9c81eULL;
  for (unsigned n = 0; n < 2000; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    for (unsigned kind : {9u, 10u}) {
      const auto bits = kind == 9 ? std::uint64_t(std::uint32_t(random)) : random;
      const auto exponent = kind == 9 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
      if ((bits & exponent) == exponent)
        continue;
      Invocation ctx;
      StringDescriptor text;
      kalandra_format_scalar_v1(&ctx, &text, kind, bits);
      ParseScalarResult result;
      kalandra_parse_scalar_v1(&ctx, &result, kind, &text);
      CHECK(!ctx.status() && result.status == ParseStatus::Success && result.bits == bits);
      ctx.settle();
      CHECK(!ctx.live());
    }
  }
  // Cancel after scanning has begun, rather than testing only a cancelled entry.
  // The counter is an existing runtime probe, not a wall-clock sleep heuristic.
  {
    Invocation ctx;
    const std::string text(16 * 1024 * 1024, '0');
    StringDescriptor input;
    kalandra_string_literal_v1(&ctx, &input, text.data(), text.size());
    std::atomic<std::uint64_t> polls{0};
    std::atomic<bool> cancelled{false}, finished{false};
    ctx.watchPolls(&polls);
    ctx.bindCancellation(&cancelled);
    std::thread cancel([&] {
      while (polls.load(std::memory_order_acquire) < 64 &&
             !finished.load(std::memory_order_acquire))
        std::this_thread::yield();
      cancelled.store(true, std::memory_order_release);
    });
    ParseScalarResult result;
    kalandra_parse_scalar_v1(&ctx, &result, 10, &input);
    finished.store(true, std::memory_order_release);
    cancel.join();
    CHECK(ctx.poll() && !ctx.status() && !result.bits);
    CHECK(polls < text.size() / 1024);
    ctx.settle();
    CHECK(!ctx.live());
  }
  for (auto kind : {0u, 12u, UINT32_MAX}) {
    Invocation ctx;
    auto r = parse(ctx, kind, "12");
    CHECK(ctx.status() == 3 && !r.bits);
    ctx.settle();
    CHECK(!ctx.live());
  }
  std::puts("Typed parsing: integer/float boundaries, grammar, offsets, long input, fenv and "
            "cancellation passed");
}
