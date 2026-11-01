#include "ConversionOracle.h"
#include "Vulkan.h"
#include "kalandra/Arithmetic.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::vulkan;
namespace {
void check(bool good, const char *message) {
  if (!good)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %lld\n", message, (long long)value.error());
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
void take(std::expected<void, Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %lld\n", message, (long long)value.error());
    throw std::runtime_error(message);
  }
}
bool nan(NumericType type, std::uint64_t bits) {
  if (isInteger(type))
    return false;
  const auto magnitude =
      bits & (type == NumericType::Float32 ? UINT64_C(0x7fffffff) : UINT64_C(0x7fffffffffffffff));
  return magnitude >
         (type == NumericType::Float32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000));
}
bool subnormal(NumericType type, std::uint64_t bits) {
  if (isInteger(type))
    return false;
  const auto magnitude =
      bits & (type == NumericType::Float32 ? UINT64_C(0x7fffffff) : UINT64_C(0x7fffffffffffffff));
  return magnitude && magnitude < (type == NumericType::Float32 ? UINT64_C(0x800000)
                                                                : UINT64_C(0x10000000000000));
}
int run(int argc, char **argv) {
  if (argc < 3 || argc > 5)
    return 2;
  const auto policy = parseArithmeticMode(argv[2]);
  check(bool(policy), "known arithmetic policy");
  const bool hardware = *policy == ArithmeticMode::Hardware;
  const bool validation = argc < 5 || std::string_view(argv[4]) != "--no-validation";
  auto runtime = take(
      Runtime::create({.validation = validation,
                       .deviceIndex = argc >= 4 ? unsigned(std::strtoul(argv[3], nullptr, 10)) : 0,
                       .enableFloat64 = hardware}),
      "conversion runtime");
  constexpr std::size_t capacity = 4096;
  constexpr std::uint32_t sentinel = 0xdeadbeef;
  std::uint64_t checks = 0, submissions = 0, deviceEnvironmentResults = 0;
  for (const auto source : numericTypes) {
    auto kernel = take(runtime->loadKernel(std::filesystem::path(argv[1]) / argv[2] /
                                           (std::string(source.name) + ".spv")),
                       "conversion kernel");
    check(kernel.inputCount() == 2 && kernel.parameterWords() == 2, "conversion proof ABI");
    const auto corpus = conversion_proof::corpus(source.type);
    for (unsigned destinationIndex = 0; destinationIndex < numericTypes.size();
         ++destinationIndex) {
      const auto destination = numericTypes[destinationIndex];
      const auto execute = [&](std::span<const std::uint64_t> batch) {
        check(batch.size() <= capacity, "bounded batch");
        std::array<std::uint64_t, capacity> inputs;
        inputs.fill(UINT64_C(0x4b12345689abcdef));
        std::copy(batch.begin(), batch.end(), inputs.begin());
        std::array<std::uint32_t, capacity> flags;
        flags.fill(sentinel);
        const std::array buffers{
            take(runtime->uploadBytes(std::as_bytes(std::span(inputs))), "source bits"),
            take(runtime->uploadBytes(std::as_bytes(std::span(flags))), "private flag scratch")};
        const std::array parameters{std::uint32_t(batch.size()), destinationIndex};
        auto result =
            take(runtime->execute(kernel, buffers, parameters, capacity * 8, capacity / 64),
                 "conversion execution");
        auto actual = take(runtime->downloadBytes(result.output), "converted bits");
        take(runtime->readBytes(buffers[1], std::as_writable_bytes(std::span(flags))),
             "validity flags");
        std::array<std::uint64_t, capacity> preserved;
        take(runtime->readBytes(buffers[0], std::as_writable_bytes(std::span(preserved))),
             "input readback");
        check(inputs == preserved, "source payload immutable");
        for (std::size_t i = 0; i < capacity; ++i) {
          std::uint64_t bits;
          std::memcpy(&bits, actual.data() + i * 8, 8);
          if (i >= batch.size()) {
            check(flags[i] == sentinel && bits == 0, "inactive lanes preserve guards");
            continue;
          }
          const auto expected = conversion_proof::oracle(source.type, destination.type, batch[i]);
          bool valid = flags[i] == std::uint32_t(expected.valid);
          if (expected.valid && bits != expected.bits) {
            bool environment = false;
            if (*policy != ArithmeticMode::Software && source.type != destination.type) {
              // Hardware mode explicitly permits device NaN/underflow behavior.
              // Fallback keeps native binary32, but binary64 casts use the exact
              // integer implementation and must pass the strict oracle.
              const bool native = hardware || (source.type != NumericType::Float64 &&
                                               destination.type != NumericType::Float64);
              if (native) {
                environment = nan(destination.type, bits) && nan(destination.type, expected.bits);
                const auto sign =
                    destination.bits == 32 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
                environment |=
                    subnormal(destination.type, expected.bits) && bits == (expected.bits & sign);
                environment |= source.type == NumericType::Float32 &&
                               destination.type == NumericType::Float64 &&
                               subnormal(source.type, batch[i]) &&
                               bits == ((batch[i] & UINT64_C(0x80000000)) << 32);
              }
            }
            valid &= environment;
            if (environment)
              ++deviceEnvironmentResults;
          }
          if (!valid) {
            std::fprintf(
                stderr, "%s %.*s -> %.*s input=%016llx actual=%016llx/%u expected=%016llx/%u\n",
                argv[2], int(source.name.size()), source.name.data(), int(destination.name.size()),
                destination.name.data(), (unsigned long long)batch[i], (unsigned long long)bits,
                flags[i], (unsigned long long)expected.bits, unsigned(expected.valid));
            throw std::runtime_error("independent APFloat conversion mismatch");
          }
          ++checks;
        }
        ++submissions;
      };
      execute({});
      for (std::size_t at = 0; at < corpus.size(); at += capacity)
        execute(std::span(corpus).subspan(at, std::min(capacity, corpus.size() - at)));
      execute(std::span(corpus).first(std::min(std::size_t(65), corpus.size())));
    }
    check(!runtime->retainedBytes(), "conversion backing retired");
    std::printf("PASS: %.*s -> all ten types, %s\n", int(source.name.size()), source.name.data(),
                argv[2]);
    std::fflush(stdout);
  }
  check(!runtime->validationErrors(), "zero conversion validation errors");
  std::printf("PASS: %llu independent APFloat cells, %llu submissions, %llu permitted device "
              "NaN/FTZ results on %s\n",
              (unsigned long long)checks, (unsigned long long)submissions,
              (unsigned long long)deviceEnvironmentResults, runtime->device().name.c_str());
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
