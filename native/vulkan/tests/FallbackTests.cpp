#include "Language.h"
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
using namespace kalandra;
namespace v = kalandra::runtime::vulkan;
namespace m = v::language;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, v::Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %lld\n", message, (long long)value.error());
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 5)
    return 2;
  const unsigned device = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0;
  const auto path = std::filesystem::path(argv[1]);
  const std::string_view operation = argc >= 4 ? argv[3] : "mul";
  const bool validation = argc < 5 || std::string_view(argv[4]) != "--no-validation";
  check(operation == "add" || operation == "sub" || operation == "mul" || operation == "div",
        "known independent binary oracle");
  for (bool native : {false, true})
    for (bool fallback : {false, true}) {
      auto runtime = take(m::Runtime::create({.validation = validation,
                                              .defaultDevice = device,
                                              .allowSoftwareFallback = fallback,
                                              .enableFloat64 = native}),
                          "language runtime");
      auto selected = take(runtime->device(), "selected device");
      auto loaded = runtime->loadNumericGraphKernel(
          path.string(), selected, NumericType::Float64, {2, 0},
          std::array{NumericType::Float64, NumericType::Float64}, {}, std::array{0u, 1u},
          std::array<std::uint8_t, 2>{0, 0});
      if (!native && !fallback)
        check(!loaded && loaded.error() == v::Error::Unsupported,
              "hardware-only rejects disabled native feature");
      else
        check(loaded && loaded->usedSoftwareFallback() == !native,
              "fallback is explicit; supported hardware is always preferred");
      auto missing = runtime->loadNumericGraphKernel(
          path.string() + ".missing", selected, NumericType::Float64, {2, 0},
          std::array{NumericType::Float64, NumericType::Float64}, {}, std::array{0u, 1u},
          std::array<std::uint8_t, 2>{0, 0});
      check(!missing && missing.error() == v::Error::Unavailable,
            "missing native module is not a fallback trigger");
      check(!runtime->stats().bytes, "load-only registry owns no tensor backing");
    }
  {
    struct Scratch {
      std::filesystem::path root;
      Scratch()
          : root(std::filesystem::temp_directory_path() /
                 ("kalandra-vulkan-fallback-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        check(std::filesystem::create_directory(root), "exclusive test directory");
      }
      ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
      }
    } scratch;
    auto runtime = take(m::Runtime::create({.validation = validation,
                                            .defaultDevice = device,
                                            .allowSoftwareFallback = true,
                                            .enableFloat64 = false}),
                        "error-policy runtime");
    const auto selected = take(runtime->device(), "error-policy device");
    const auto load = [&](std::string_view name) {
      return runtime->loadNumericGraphKernel(
          (scratch.root / name).string(), selected, NumericType::Float64, {2, 0},
          std::array{NumericType::Float64, NumericType::Float64}, {}, std::array{0u, 1u},
          std::array<std::uint8_t, 2>{0, 0});
    };
    const auto corrupt = [&](std::string_view name) {
      std::ofstream file(scratch.root / name, std::ios::binary);
      const std::array<std::uint32_t, 5> invalid{};
      file.write(reinterpret_cast<const char *>(invalid.data()), sizeof(invalid));
      check(bool(file), "invalid module fixture written");
    };
    const auto copy = [&](const std::filesystem::path &source, std::string_view name) {
      check(std::filesystem::copy_file(source, scratch.root / name),
            "private module fixture copied");
    };
    copy(path.string() + ".software.spv", "missing.spv.software.spv");
    {
      std::ifstream input(path, std::ios::binary | std::ios::ate);
      const auto length = input.tellg();
      check(length >= 20 && length % 4 == 0, "valid native fixture size");
      std::vector<std::uint32_t> words(static_cast<std::size_t>(length) / 4);
      input.seekg(0);
      input.read(reinterpret_cast<char *>(words.data()), length);
      bool changed = false;
      for (std::size_t index = 5; index < words.size();) {
        const auto count = words[index] >> 16;
        check(count && count <= words.size() - index, "valid native instruction");
        if ((words[index] & 65535) == 15) {
          check(count >= 4, "entry point string");
          words[index + 3] = (words[index + 3] & 0xffffff00u) | unsigned('x');
          changed = true;
          break;
        }
        index += count;
      }
      check(changed, "entry point renamed without changing physical ABI/capabilities");
      std::ofstream output(scratch.root / "wrong-entry.spv", std::ios::binary);
      output.write(reinterpret_cast<const char *>(words.data()), length);
      output.close();
      check(bool(output), "wrong entry fixture written");
      copy(path.string() + ".software.spv", "wrong-entry.spv.software.spv");
      auto raw = take(v::Runtime::create({.validation = validation, .deviceIndex = device,
                                         .enableFloat64 = false}), "wrong-entry raw control");
      auto physical = raw->loadKernel(scratch.root / "wrong-entry.spv");
      check(!physical && physical.error() == v::Error::Unsupported,
            "raw fixture has valid reflected ABI and unsupported capability");
      auto logical = load("wrong-entry.spv");
      check(!logical && logical.error() == v::Error::Invalid,
            "wrong logical primary ABI is never masked by a valid fallback");
    }
    auto loaded = load("missing.spv");
    check(!loaded && loaded.error() == v::Error::Unavailable,
          "missing primary never selects a valid backup");
    corrupt("invalid.spv");
    copy(path.string() + ".software.spv", "invalid.spv.software.spv");
    loaded = load("invalid.spv");
    check(!loaded && loaded.error() == v::Error::Invalid,
          "invalid primary never selects a valid backup");
    copy(path, "unsupported.spv");
    loaded = load("unsupported.spv");
    check(!loaded && loaded.error() == v::Error::Unsupported,
          "missing backup retains capability error");
    copy(path, "broken-backup.spv");
    corrupt("broken-backup.spv.software.spv");
    loaded = load("broken-backup.spv");
    check(!loaded && loaded.error() == v::Error::Invalid,
          "malformed fallback remains an artifact error");
  }
  auto runtime = take(
      v::Runtime::create({.validation = validation, .deviceIndex = device, .enableFloat64 = false}),
      "feature-limited runtime");
  auto rejected = runtime->loadKernel(path);
  check(!rejected && rejected.error() == v::Error::Unsupported,
        "native capability rejected before pipeline creation");
  const v::KernelContract wrongEntry{"wrong-entry", 2, 2};
  auto malformed = runtime->loadKernel(path, &wrongEntry);
  check(!malformed && malformed.error() == v::Error::Invalid,
        "wrong primary entry is invalid before unsupported capability can trigger fallback");
  const auto inspect = take(v::Runtime::create({.validation = validation, .deviceIndex = device}),
                            "contract inspection context");
  const auto nativeKernel = take(inspect->loadKernel(path), "native contract module");
  const v::KernelContract wrongWords{std::string(nativeKernel.entryName()), 2, 3};
  malformed = runtime->loadKernel(path, &wrongWords);
  check(!malformed && malformed.error() == v::Error::Invalid,
        "wrong primary carrier count is invalid before capability fallback");
  auto software = take(runtime->loadKernel(path.string() + ".software.spv"), "software module");
  check(software.inputCount() == 2 && software.parameterWords() == 2, "shared product ABI");
  std::fesetround(FE_TONEAREST);
  const std::array<std::uint64_t, 22> edges{0,
                                            0x8000000000000000ULL,
                                            1,
                                            2,
                                            0xfffffffffffffULL,
                                            0x10000000000000ULL,
                                            0x10000000000001ULL,
                                            0x3fefffffffffffffULL,
                                            0x3ff0000000000000ULL,
                                            0x3ff0000000000001ULL,
                                            0x4000000000000000ULL,
                                            0x7fefffffffffffffULL,
                                            0x7ff0000000000000ULL,
                                            0xfff0000000000000ULL,
                                            0x7ff8000000000000ULL,
                                            0x7ff0000000000001ULL,
                                            0x8000000000000001ULL,
                                            0x8010000000000000ULL,
                                            0xbff0000000000000ULL,
                                            0xbff0000000000001ULL,
                                            0xffefffffffffffffULL,
                                            0xfff8000000001234ULL};
  std::vector<double> left, right;
  for (auto a : edges)
    for (auto b : edges) {
      left.push_back(std::bit_cast<double>(a));
      right.push_back(std::bit_cast<double>(b));
    }
  std::uint64_t state = 0x2834a5b6e7c8d9f1ULL;
  const auto random = [&]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (unsigned i = 0; i < 1024; ++i) {
    left.push_back(std::bit_cast<double>(random()));
    right.push_back(std::bit_cast<double>(random()));
  }
  {
    const std::array buffers{take(runtime->uploadBytes(std::as_bytes(std::span(left))), "left"),
                             take(runtime->uploadBytes(std::as_bytes(std::span(right))), "right")};
    const std::array parameters{std::uint32_t(left.size()), UINT32_MAX};
    auto result =
        take(runtime->execute(software, buffers, parameters, left.size() * 8,
                              (left.size() + software.localSize() - 1) / software.localSize()),
             "software execution");
    check(result.parameters.back() == UINT32_MAX, "floating arithmetic is not an integer fault");
    const auto output = take(runtime->downloadBytes(result.output), "software output");
    check(output.size() == left.size() * 8, "output size");
    for (std::size_t i = 0; i < left.size(); ++i) {
      volatile double product = operation == "add"   ? left[i] + right[i]
                                : operation == "sub" ? left[i] - right[i]
                                : operation == "div" ? left[i] / right[i]
                                                     : left[i] * right[i];
      double actual;
      std::memcpy(&actual, output.data() + 8 * i, 8);
      if (std::isnan(product))
        check(std::bit_cast<std::uint64_t>(actual) == UINT64_C(0x7ff8000000000000),
              "software arithmetic canonical NaN contract");
      else
        check(std::bit_cast<std::uint64_t>(actual) == std::bit_cast<std::uint64_t>(double(product)),
              "software IEEE arithmetic including gradual underflow/signed zero/overflow");
    }
  }
  check(!runtime->retainedBytes() && !runtime->validationErrors(), "fallback cleanup/validation");
  std::printf("PASS: native-first explicit fallback, disabled VkDevice float64 feature, %zu IEEE "
              "%.*s checks on %s\n",
              left.size(), int(operation.size()), operation.data(), runtime->device().name.c_str());
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
