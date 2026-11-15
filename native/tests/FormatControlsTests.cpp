#include "../runtime/Format.h"
#include "kalandra/FormatTemplate.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace {
using namespace kalandra;
using namespace kalandra::runtime;
using K = FormatScalarKind;
static void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(Invocation &context) {
  context.settle();
  check(!context.live() && context.allocations() == context.releases(),
        "format controls leaked owned backing");
}
static std::string text(const StringDescriptor &value) {
  return value.byteLength ? std::string(value.data, value.byteLength) : std::string{};
}
static std::string number(K kind, std::uint64_t bits, std::uint64_t precision,
                          FormatPresentation mode = FormatPresentation::Fixed) {
  Invocation context;
  StringDescriptor output;
  kalandra_format_number_v1(&context, &output, unsigned(kind), bits, unsigned(mode), precision);
  check(!context.status() && validateString(context, output), "invalid number control output");
  auto result = text(output);
  balanced(context);
  return result;
}
struct FixedReference {
  K kind;
  std::uint64_t bits, precision;
  std::string_view expected;
};
#include "FormatFixedReferences.h"
struct Budget {
  std::uint64_t used = 0, maximum = 4096;
  std::atomic<bool> *cancel = nullptr;
  static bool reserve(void *state, std::uint64_t bytes) noexcept {
    auto &self = *static_cast<Budget *>(state);
    if (bytes > self.maximum - self.used)
      return false;
    self.used += bytes;
    if (self.cancel && bytes >= 4096)
      self.cancel->store(true, std::memory_order_release);
    return true;
  }
  static void release(void *state, std::uint64_t bytes) noexcept {
    static_cast<Budget *>(state)->used -= bytes;
  }
};
} // namespace

void testFormatControls() {
  using namespace kalandra;
  using namespace kalandra::runtime;
  const auto parsed = parseFormatTemplate("{:>8.2} {:<3x} {:.0} {:>0}", 4);
  check(parsed && (*parsed)[0].spec.width == 8 && (*parsed)[0].spec.precision == 2 &&
            (*parsed)[0].spec.alignment == FormatAlignment::Right &&
            (*parsed)[0].spec.presentation == FormatPresentation::Fixed &&
            (*parsed)[2].spec.presentation == FormatPresentation::HexLower,
        "format controls parsed incorrectly");
  for (auto invalid : {"{:}", "{:>}", "{:.}", "{:>-1}", "{:q}", "{:.2x}", "{:>9223372036854775808}",
                       "{:.9223372036854775808}"})
    check(!parseFormatTemplate(invalid, 1), "invalid format control accepted");

  for (const auto &reference : fixedReferences)
    check(number(reference.kind, reference.bits, reference.precision) == reference.expected,
          "fixed formatting disagrees with independent Python reference");
  for (const auto &[value, precision, expected] : {std::tuple{1.125, 2ULL, "1.12"},
                                                   {1.375, 2ULL, "1.38"},
                                                   {-1.125, 2ULL, "-1.12"},
                                                   {2.5, 0ULL, "2"},
                                                   {3.5, 0ULL, "4"},
                                                   {-0.0, 2ULL, "-0.00"},
                                                   {2.675, 2ULL, "2.67"}})
    check(number(K::F64, std::bit_cast<std::uint64_t>(value), precision) == expected,
          "fixed formatting rounding rule changed");
  check(number(K::I64, 0x8000000000000000ULL, 0, FormatPresentation::HexLower) ==
                "-8000000000000000" &&
            number(K::U64, UINT64_MAX, 0, FormatPresentation::HexLower) == "ffffffffffffffff",
        "signed/unsigned hexadecimal limit mismatch");

  for (const auto &[input, width, alignment, expected] :
       {std::tuple{"é", 4ULL, FormatAlignment::Right, "   é"},
        {"é", 4ULL, FormatAlignment::Left, "é  "},
        {"👩‍💻", 4ULL, FormatAlignment::Right, " 👩‍💻"},
        {"long", 1ULL, FormatAlignment::Left, "long"},
        {"", 3ULL, FormatAlignment::Right, "   "},
        {"", 0ULL, FormatAlignment::Left, ""}}) {
    for (bool alias : {false, true}) {
      Invocation context;
      StringDescriptor source, separate;
      kalandra_string_literal_v1(&context, &source, input, std::strlen(input));
      auto *output = alias ? &source : &separate;
      kalandra_format_align_v1(&context, output, &source, width, unsigned(alignment));
      check(!context.status() && validateString(context, *output) && text(*output) == expected,
            "Unicode scalar alignment or alias handling failed");
      balanced(context);
    }
  }
  for (const auto &[kind, bits, mode, precision] :
       {std::tuple{0u, 0ULL, unsigned(FormatPresentation::Fixed), 2ULL},
        {unsigned(K::I64), 0ULL, unsigned(FormatPresentation::Fixed), 2ULL},
        {unsigned(K::Bool), 1ULL, unsigned(FormatPresentation::HexLower), 0ULL},
        {unsigned(K::I8), 256ULL, unsigned(FormatPresentation::HexLower), 0ULL},
        {unsigned(K::F32), 0x100000000ULL, unsigned(FormatPresentation::Fixed), 2ULL},
        {unsigned(K::F64), 0ULL, 99u, 0ULL},
        {unsigned(K::U64), 0ULL, unsigned(FormatPresentation::HexLower), 1ULL},
        {unsigned(K::F64), 0ULL, unsigned(FormatPresentation::Fixed), 18446744073709551615ULL}}) {
    Invocation context;
    StringDescriptor output;
    kalandra_format_number_v1(&context, &output, kind, bits, mode, precision);
    check(context.status() == 3 && !output.data && !context.allocations(),
          "malformed numeric format ABI was accepted");
    balanced(context);
  }
  for (bool align : {false, true}) {
    for (unsigned failure = 1; failure <= 6; ++failure) {
      Invocation context;
      context.failAllocationAt(failure);
      StringDescriptor input, output;
      if (align) {
        kalandra_string_literal_v1(&context, &input, "é", 2);
        kalandra_format_align_v1(&context, &output, &input, 8, unsigned(FormatAlignment::Right));
      } else {
        kalandra_format_number_v1(&context, &output, unsigned(K::F64),
                                  std::bit_cast<std::uint64_t>(0.125),
                                  unsigned(FormatPresentation::Fixed), 1100);
      }
      check(context.status() ? !output.data : validateString(context, output),
            "failed controlled formatting published backing");
      balanced(context);
    }
    Budget budget;
    Invocation limited;
    limited.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
    StringDescriptor input, output;
    if (align) {
      kalandra_string_literal_v1(&limited, &input, "é", 2);
      kalandra_format_align_v1(&limited, &output, &input, 1000000,
                               unsigned(FormatAlignment::Right));
    } else {
      kalandra_format_number_v1(&limited, &output, unsigned(K::F64),
                                std::bit_cast<std::uint64_t>(0.125),
                                unsigned(FormatPresentation::Fixed), 1000000);
    }
    check(limited.status() == 2 && !output.data, "formatting escaped allocation budget");
    balanced(limited);
    check(!budget.used, "format resource reservation leaked");

    std::atomic<bool> cancelled{false};
    Budget cancellable{0, 2 << 20, &cancelled};
    Invocation stopped;
    stopped.bindCancellation(&cancelled);
    stopped.bindRecursiveBudget(&cancellable, Budget::reserve, Budget::release);
    if (align) {
      kalandra_string_literal_v1(&stopped, &input, "é", 2);
      kalandra_format_align_v1(&stopped, &output, &input, 1000000, unsigned(FormatAlignment::Left));
    } else {
      kalandra_format_number_v1(&stopped, &output, unsigned(K::F64),
                                std::bit_cast<std::uint64_t>(0.125),
                                unsigned(FormatPresentation::Fixed), 1000000);
    }
    check(cancelled.load() && !output.data, "cancellation published a partial formatted value");
    balanced(stopped);
    check(!cancellable.used, "cancelled formatting leaked budget");
  }
  {
    Invocation context;
    std::atomic<std::uint64_t> polls{0};
    context.watchPolls(&polls);
    StringDescriptor output;
    kalandra_format_number_v1(&context, &output, unsigned(K::F64),
                              std::bit_cast<std::uint64_t>(0.125),
                              unsigned(FormatPresentation::Fixed), 1000000);
    check(!context.status() && output.byteLength == 1000002 && polls.load() > 100 &&
              std::string_view(output.data, 5) == "0.125" &&
              std::all_of(output.data + 5, output.data + output.byteLength,
                          [](char ch) { return ch == '0'; }),
          "large precision was inaccurate or lacked incremental safepoints");
    balanced(context);
  }
  {
    std::atomic<bool> cancellation{true};
    Invocation context;
    context.fault(7);
    context.bindCancellation(&cancellation);
    StringDescriptor output;
    kalandra_format_number_v1(&context, &output, 999, 0, 999, UINT64_MAX);
    kalandra_format_align_v1(&context, &output, nullptr, UINT64_MAX, 999);
    check(context.status() == 7 && !output.data && !context.allocations(),
          "format controls changed existing fault precedence");
    balanced(context);
  }
}
