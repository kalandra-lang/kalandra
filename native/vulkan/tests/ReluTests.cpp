#include "ConversionOracle.h"
#include "Vulkan.h"
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
std::uint64_t reference(NumericType type, std::uint64_t bits) {
  if (isUnsigned(type))
    return bits;
  if (isInteger(type))
    return llvm::APInt(describeNumeric(type)->bits, bits).isNegative() ? 0 : bits;
  const llvm::APFloat value(conversion_proof::semantics(type),
                            llvm::APInt(describeNumeric(type)->bits, bits));
  return value.isNegative() && !value.isNaN() ? 0 : bits;
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 4)
    return 2;
  // ReLU is a sign/selection operation. Even float64 storage must not require
  // float64 arithmetic capability or select an emulated arithmetic pipeline.
  const bool validation = argc < 4 || std::string_view(argv[3]) != "--no-validation";
  auto runtime = take(
      Runtime::create({.validation = validation,
                       .deviceIndex = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0,
                       .enableFloat64 = false}),
      "ReLU runtime");
  std::uint64_t checks = 0;
  for (const auto type : numericTypes) {
    auto kernel =
        take(runtime->loadKernel(std::filesystem::path(argv[1]) / type.name / "kernel0.spv"),
             "ReLU kernel without native float64");
    check(kernel.inputCount() == 1 && kernel.parameterWords() == 2, "ReLU typed ABI");
    auto values = conversion_proof::corpus(type.type);
    const auto execute = [&](std::span<const std::uint64_t> batch) {
      std::vector<std::byte> input(batch.size() * type.bytes());
      for (std::size_t i = 0; i < batch.size(); ++i)
        std::memcpy(input.data() + i * type.bytes(), &batch[i], type.bytes());
      const std::array buffers{take(runtime->uploadBytes(input), "ReLU input")};
      const std::array parameters{std::uint32_t(batch.size()), UINT32_MAX};
      auto result = take(runtime->execute(kernel, buffers, parameters, input.size(),
                                          std::max(1u, std::uint32_t((batch.size() + 63) / 64))),
                         "ReLU execution");
      check(result.parameters.back() == UINT32_MAX,
            "ReLU has no arithmetic fault, including signed minimum");
      auto output = take(runtime->downloadBytes(result.output), "ReLU output");
      check(output.size() == input.size(), "ReLU output byte extent");
      for (std::size_t i = 0; i < batch.size(); ++i) {
        std::uint64_t actual = 0;
        std::memcpy(&actual, output.data() + i * type.bytes(), type.bytes());
        if (actual != reference(type.type, batch[i])) {
          std::fprintf(stderr, "%.*s ReLU input=%016llx output=%016llx\n", int(type.name.size()),
                       type.name.data(), (unsigned long long)batch[i], (unsigned long long)actual);
          throw std::runtime_error("ReLU bitwise payload/sign contract");
        }
        ++checks;
      }
      check(take(runtime->downloadBytes(buffers[0]), "immutable ReLU input") == input,
            "ReLU input not mutated");
    };
    execute({});
    for (std::size_t at = 0; at < values.size(); at += 4096)
      execute(std::span(values).subspan(at, std::min(std::size_t(4096), values.size() - at)));
    for (std::size_t count : {1u, 63u, 65u, 129u})
      execute(std::span(values).first(count));
    check(!runtime->retainedBytes(), "ReLU ownership retired");
    std::printf("PASS: %.*s ReLU bitwise corpus/empty/tails\n", int(type.name.size()),
                type.name.data());
    std::fflush(stdout);
  }
  check(!runtime->validationErrors(), "zero ReLU validation errors");
  std::printf("PASS: %llu bitwise ReLU cells without Float64 feature on %s\n",
              (unsigned long long)checks, runtime->device().name.c_str());
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
