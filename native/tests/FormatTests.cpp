#include "kalandra/FormatTemplate.h"
#include "../runtime/Format.h"
#include <atomic>
#include <bit>
#include <charconv>
#include <iostream>
#include <limits>
#include <tuple>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
static std::string render(FormatScalarKind kind, std::uint64_t bits) {
  Invocation ctx;
  StringDescriptor out;
  kalandra_format_scalar_v1(&ctx, &out, std::uint32_t(kind), bits);
  require(!ctx.status() && validateString(ctx, out), "invalid formatted String");
  const std::string result(out.data, out.byteLength);
  ctx.settle();
  require(!ctx.live() && ctx.allocations() == ctx.releases(), "format backing leaked");
  return result;
}
int main() {
  try {
    const auto parsed = parseFormatTemplate("é {{literal}}: {} / {}", 2);
    require(parsed && parsed->size() == 4, "format segmentation failed");
    require((*parsed)[0].text == "é {literal}: " && (*parsed)[1].argument &&
            (*parsed)[2].text == " / " && (*parsed)[3].argument, "format changed text or argument order");
    require(parseFormatTemplate("", 0) && parseFormatTemplate("{{}}", 0), "empty/escaped format rejected");
    for (const auto text : {"{", "}", "{0}", "{name}", "{:.2f}", "{{}", "}{"})
      require(!parseFormatTemplate(text, 0), "unsupported format syntax accepted");
    require(!parseFormatTemplate("{}", 0) && !parseFormatTemplate("", 1), "format arity mismatch accepted");
    using K = FormatScalarKind;
    for (const auto &[kind, bits, expected] : {
      std::tuple{K::I8, 128ULL, "-128"}, {K::I16, 32768ULL, "-32768"},
      {K::I32, 2147483648ULL, "-2147483648"}, {K::I64, 0x8000000000000000ULL, "-9223372036854775808"},
      {K::U8, 255ULL, "255"}, {K::U16, 65535ULL, "65535"},
      {K::U32, 4294967295ULL, "4294967295"}, {K::U64, 18446744073709551615ULL, "18446744073709551615"},
      {K::F32, 0x80000000ULL, "-0"}, {K::F64, 0x8000000000000000ULL, "-0"},
      {K::F32, 0x7f800000ULL, "inf"}, {K::F64, 0xfff0000000000000ULL, "-inf"},
      {K::F32, 0xffc12345ULL, "nan"}, {K::F64, 0xfff8000000000001ULL, "nan"},
      {K::Bool, 0ULL, "false"}, {K::Bool, 1ULL, "true"}})
      require(render(kind, bits) == expected, "format edge value mismatch");
    // Independent parser round-trips finite hardware representations, including
    // subnormals and sign bits. Formatting never rounds through f64 for f32.
    std::uint64_t random = 0x719fbc310e5274a9ULL;
    for (unsigned i = 0; i < 2000; ++i) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      const auto bits = std::uint32_t(random);
      if ((bits & 0x7f800000) != 0x7f800000) {
        const auto text = render(K::F32, bits);
        float value = 0;
        const auto result = std::from_chars(text.data(), text.data()+text.size(), value);
        require(result.ec == std::errc{} && result.ptr == text.data()+text.size() &&
                std::bit_cast<std::uint32_t>(value) == bits, "f32 did not round-trip exactly");
      }
      if ((random & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL) {
        const auto text = render(K::F64, random);
        double value = 0;
        const auto result = std::from_chars(text.data(), text.data()+text.size(), value);
        require(result.ec == std::errc{} && result.ptr == text.data()+text.size() &&
                std::bit_cast<std::uint64_t>(value) == random, "f64 did not round-trip exactly");
      }
    }
    for (const auto &[kind, bits] : {std::pair{0u, 0ULL}, {99u, 0ULL},
                                    {unsigned(K::Bool), 2ULL}, {unsigned(K::I8), 256ULL},
                                    {unsigned(K::F32), 0x100000000ULL}}) {
      Invocation ctx; StringDescriptor out;
      kalandra_format_scalar_v1(&ctx, &out, kind, bits);
      require(ctx.status() == 3 && !out.data && !ctx.allocations(), "malformed scalar ABI accepted");
    }
    for (unsigned ordinal = 1; ordinal <= 4; ++ordinal) {
      Invocation ctx; StringDescriptor out;
      ctx.failAllocationAt(ordinal);
      kalandra_format_scalar_v1(&ctx, &out, unsigned(K::U64), 42);
      require(ctx.status() ? !out.data : validateString(ctx, out), "failed format published backing");
      ctx.settle();
      require(!ctx.live() && ctx.allocations() == ctx.releases(), "format allocation failure leaked");
    }
    std::atomic<bool> cancelled{true};
    Invocation ctx; StringDescriptor out;
    ctx.bindCancellation(&cancelled);
    kalandra_format_scalar_v1(&ctx, &out, unsigned(K::U64), 42);
    require(!out.data && !ctx.allocations(), "cancelled format published backing");
    Invocation fault;
    fault.fault(7); fault.bindCancellation(&cancelled);
    kalandra_format_scalar_v1(&fault, &out, unsigned(K::U64), 42);
    require(fault.status() == 7 && !out.data, "format changed fault precedence");
    std::cout << "PASS: literal format grammar, all scalar types, exact float round-trips and owned failure cleanup\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
