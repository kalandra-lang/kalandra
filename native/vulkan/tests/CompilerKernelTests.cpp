#include "Vulkan.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
using namespace kalandra::runtime::vulkan;
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    throw std::runtime_error(message);
  }
}
template <class T> T take(std::expected<T, Error> result, const char *message) {
  if (!result) {
    std::fprintf(stderr, "FAIL: %s (error %d)\n", message, int(result.error()));
    throw std::runtime_error(message);
  }
  return std::move(*result);
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 4)
    return 2;
  auto runtime =
      take(Runtime::create(
               {.deviceIndex = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0}),
           "runtime");
  const bool software = argc == 4 && std::string_view(argv[3]) == "software";
  if (argc == 4 && !software && std::string_view(argv[3]) != "hardware")
    return 2;
  const auto flush = [software](float value) {
    auto bits = std::bit_cast<std::uint32_t>(value);
    if (!software && !(bits & 0x7f800000))
      bits &= 0x80000000;
    return std::bit_cast<float>(bits);
  };
  auto kernel = take(runtime->loadKernel(argv[1]), "compiler kernel");
  for (unsigned count : {0u, 1u, 63u, 64u, 65u, 4097u}) {
    std::vector<float> a(count), b(count);
    constexpr std::array<std::uint32_t, 12> edges{0,          0x80000000, 1,          0x80000001,
                                                  0x007fffff, 0x00800000, 0x7f7fffff, 0xff7fffff,
                                                  0x7f800000, 0xff800000, 0x7fc00001, 0x3f800001};
    for (unsigned i = 0; i < count; ++i) {
      a[i] = i < edges.size() ? std::bit_cast<float>(edges[i]) : float(int(i % 91) - 45) / 8;
      b[i] = float(i % 9) / 4;
    }
    auto left = take(runtime->upload(a), "left"), right = take(runtime->upload(b), "right");
    std::array inputs{left, right};
    std::array words{count, UINT32_MAX};
    auto result =
        take(runtime->execute(kernel, inputs, words, count * 4, std::max(1u, (count + 63) / 64)),
             "execute source kernel");
    check(result.parameters == std::vector<std::uint32_t>(words.begin(), words.end()),
          "parameter/fault carrier");
    auto values = take(runtime->download(result.output), "output");
    for (unsigned i = 0; i < count; ++i) {
      volatile float product = flush(a[i]) * flush(b[i]);
      float expected = flush(flush(product) + flush(a[i]));
      if (!((std::isnan(expected) && std::isnan(values[i])) ||
            std::bit_cast<std::uint32_t>(expected) == std::bit_cast<std::uint32_t>(values[i]))) {
        std::fprintf(stderr, "count=%u lane=%u a=%08x b=%08x actual=%08x expected=%08x\n", count, i,
                     std::bit_cast<unsigned>(a[i]), std::bit_cast<unsigned>(b[i]),
                     std::bit_cast<unsigned>(values[i]), std::bit_cast<unsigned>(expected));
        check(false, "exact source float32 result");
      }
    }
    auto original = take(runtime->download(left), "original");
    check(original.size() == a.size() &&
              (a.empty() || !std::memcmp(original.data(), a.data(), a.size() * sizeof(float))),
          "immutable input bits");
  }
  check(runtime->retainedBytes() == 0 && runtime->validationErrors() == 0,
        "clean resources and validation");
  std::printf("PASS: actual compiled Kalandra combine() on %s, including IEEE edges/empty/tails\n",
              runtime->device().name.c_str());
  return 0;
}

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
