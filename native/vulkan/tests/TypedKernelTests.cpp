#include "Vulkan.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
using namespace kalandra::runtime::vulkan;
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s error %d\n", message, int(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
struct Type {
  const char *name;
  unsigned bytes;
  bool floating;
  bool unsignedInteger;
};
std::uint64_t bits(Type type, unsigned value) {
  if (type.floating)
    return type.bytes == 4 ? std::bit_cast<std::uint32_t>(float(value))
                           : std::bit_cast<std::uint64_t>(double(value));
  return value;
}
int run(int argc, char **argv) {
  if (argc != 2 && argc != 3)
    return 2;
  auto runtime = take(
      Runtime::create({.deviceIndex = argc == 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0,
                       .maxBytes = 128 * 1024 * 1024}),
      "runtime");
  for (Type type : std::array<Type, 10>{{{"int8", 1, false, false},
                                         {"uint8", 1, false, true},
                                         {"int16", 2, false, false},
                                         {"uint16", 2, false, true},
                                         {"int32", 4, false, false},
                                         {"uint32", 4, false, true},
                                         {"int64", 8, false, false},
                                         {"uint64", 8, false, true},
                                         {"float32", 4, true, false},
                                         {"float64", 8, true, false}}}) {
    std::vector<Kernel> kernels;
    for (unsigned stage = 0;; ++stage) {
      auto path =
          std::filesystem::path(argv[1]) / type.name / ("kernel" + std::to_string(stage) + ".spv");
      if (!std::filesystem::exists(path))
        break;
      kernels.push_back(take(runtime->loadKernel(path), "load typed source stage"));
    }
    check(!kernels.empty(), "source map stages exist");
    {
      auto buffer = take(runtime->uploadBytes({}), "oversized grid input");
      std::array inputs{buffer};
      std::array parameters{0u, UINT32_MAX};
      auto rejected = runtime->execute(kernels.front(), inputs, parameters, 0, UINT32_MAX);
      check(!rejected && rejected.error() == Error::Unsupported,
            "padded dispatch index overflow rejected before allocation");
    }
    const auto execute = [&](Buffer input, unsigned count) {
      KernelResult result;
      for (const auto &kernel : kernels) {
        std::array inputs{input};
        std::array parameters{count, UINT32_MAX};
        result = take(runtime->execute(kernel, inputs, parameters, count * type.bytes,
                                       std::max(1u, (count + 63) / 64)),
                      "typed map stage");
        if (result.parameters.back() != UINT32_MAX)
          break;
        input = result.output;
      }
      return result;
    };
    std::vector<unsigned> counts{0u, 1u, 3u, 63u, 64u, 65u, 129u};
    // Cross the portable X-axis dispatch limit, including a partial final
    // workgroup and padded physical groups. Every lane is checked independently.
    if (std::string_view(type.name) == "float32")
      counts.push_back(65535u * 64 + 17);
    for (unsigned count : counts) {
      std::vector<std::byte> raw(count * type.bytes), expected(raw.size());
      for (unsigned i = 0; i < count; ++i) {
        auto input = bits(type, i % 20), output = bits(type, (i % 20) * 2 + 3);
        std::memcpy(raw.data() + i * type.bytes, &input, type.bytes);
        std::memcpy(expected.data() + i * type.bytes, &output, type.bytes);
      }
      auto buffer = take(runtime->uploadBytes(raw), "typed upload");
      auto result = execute(buffer, count);
      check(result.parameters.back() == UINT32_MAX, "unexpected checked arithmetic fault");
      check(take(runtime->downloadBytes(result.output), "typed result") == expected,
            "exact typed arithmetic");
      check(take(runtime->downloadBytes(buffer), "input result") == raw, "immutable typed input");
    }
    if (!type.floating) {
      auto maximum = type.bytes == 8
                         ? (type.unsignedInteger ? UINT64_MAX : std::uint64_t(INT64_MAX))
                         : (UINT64_C(1) << (type.bytes * 8 - unsigned(!type.unsignedInteger))) - 1;
      std::vector<std::byte> raw(type.bytes);
      std::memcpy(raw.data(), &maximum, type.bytes);
      auto buffer = take(runtime->uploadBytes(raw), "overflow input");
      auto result = execute(buffer, 1);
      check(result.parameters.back() == 0, "first failing arithmetic lane retained");
    }
    std::printf("PASS: %s exact source arithmetic/empty/tails/overflow\n", type.name);
  }
  check(runtime->retainedBytes() == 0 && runtime->validationErrors() == 0,
        "typed cleanup/validation");
  std::printf("PASS: ten numeric source kernels on %s\n", runtime->device().name.c_str());
  return 0;
}
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
