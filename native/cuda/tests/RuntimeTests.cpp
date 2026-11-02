#include "CudaRuntime.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <type_traits>
using namespace kalandra::runtime::cuda;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected artifact directory");
    auto runtime = Runtime::create();
    if (!runtime)
      throw std::runtime_error("create runtime: " + std::to_string(int(runtime.error())));
    auto other = Runtime::create();
    require(bool(other), "create independent runtime");
    unsigned launches = 0;
    for (auto chip : {"sm_80", "sm_120"})
      for (auto policy : {"hardware", "software", "hardware-with-software-fallback"}) {
        for (auto type : {"f32", "f64", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"}) {
          std::string element(type);
          if (element[0] == 'u')
            element[0] = 'i';
          auto path = std::filesystem::path(argv[1]) /
                      (std::string(chip) + "-" + element + "-" + policy + ".ptx");
          auto kernel = (*runtime)->loadKernel(path);
          require(bool(kernel), "load kernel");
          for (unsigned count : {1, 3, 63, 64, 65, 257, 131073}) {
            auto run = [&]<class T>() {
              std::vector<T> a(count), b(count), expected(count);
              if constexpr (std::is_floating_point_v<T>) {
                for (unsigned i = 0; i < count; ++i) {
                  a[i] = T(int(i % 31) - 15) / 8;
                  b[i] = T(int(i % 19) - 9) / 4;
                }
                a[0] = 1 + std::numeric_limits<T>::epsilon();
                b[0] = -1 + std::numeric_limits<T>::epsilon();
                for (unsigned i = 0; i < count; ++i) {
                  volatile T product = a[i] * b[i];
                  expected[i] = product + a[i];
                }
              } else {
                // Unsigned uint64 arithmetic defines modulo 2^64. Truncation
                // gives the smaller-width oracle without signed C++ overflow.
                for (unsigned i = 0; i < count; ++i) {
                  a[i] = T(~std::uint64_t(i * 19));
                  b[i] = T(i * 31 + 7);
                  expected[i] = T(std::uint64_t(a[i]) * std::uint64_t(b[i]) + std::uint64_t(a[i]));
                }
              }
              const std::array<unsigned, 2> words{count, 0};
              auto left = (*runtime)->uploadBytes(std::as_bytes(std::span(a)));
              auto right = (*runtime)->uploadBytes(std::as_bytes(std::span(b)));
              auto output = (*runtime)->blankBytes(count * sizeof(T));
              auto parameters = (*runtime)->uploadBytes(std::as_bytes(std::span(words)));
              require(bool(left) && bool(right) && bool(output) && bool(parameters),
                      "allocate buffers");
              std::array<Buffer, 4> bindings{*left, *right, *output, *parameters};
              require(bool((*runtime)->execute(*kernel, bindings, (count + 63) / 64)), "execute");
              auto actual = (*runtime)->downloadBytes(*output);
              auto oracle = std::as_bytes(std::span(expected));
              require(actual &&
                          std::equal(actual->begin(), actual->end(), oracle.begin(), oracle.end()),
                      "exact multiply-then-add");
              auto immutable = (*runtime)->downloadBytes(*left);
              auto original = std::as_bytes(std::span(a));
              require(immutable && std::equal(immutable->begin(), immutable->end(),
                                              original.begin(), original.end()),
                      "input immutable");
              require(!(*other)->downloadBytes(*left), "cross-context buffer rejected");
              require(!(*other)->execute(*kernel, bindings, 1), "cross-context kernel rejected");
              require(!(*runtime)->execute(*kernel, bindings, 0), "zero groups rejected");
              auto invalid = bindings;
              invalid[3] = Buffer{};
              require(!(*runtime)->execute(*kernel, invalid, 1),
                      "missing parameter buffer rejected");
              ++launches;
            };
            if (std::string(type) == "f32")
              run.template operator()<float>();
            else if (std::string(type) == "f64")
              run.template operator()<double>();
            else if (std::string(type) == "i8")
              run.template operator()<std::int8_t>();
            else if (std::string(type) == "u8")
              run.template operator()<std::uint8_t>();
            else if (std::string(type) == "i16")
              run.template operator()<std::int16_t>();
            else if (std::string(type) == "u16")
              run.template operator()<std::uint16_t>();
            else if (std::string(type) == "i32")
              run.template operator()<std::int32_t>();
            else if (std::string(type) == "u32")
              run.template operator()<std::uint32_t>();
            else if (std::string(type) == "i64")
              run.template operator()<std::int64_t>();
            else
              run.template operator()<std::uint64_t>();
            require((*runtime)->retainedBytes() == 0, "buffers retired after completion");
          }
        }
      }
    {
      auto kernel = (*runtime)->loadKernel(std::filesystem::path(argv[1]) / "index-width.ptx");
      require(bool(kernel), "load unsigned domain witness");
      std::array<std::uint32_t, 2> words{0x80000001u, 0xffffffffu};
      auto parameters = (*runtime)->uploadBytes(std::as_bytes(std::span(words)));
      auto output = (*runtime)->blankBytes(16);
      require(parameters && output, "index witness allocations");
      require(bool((*runtime)->execute(*kernel, std::array{*output, *parameters}, 1)),
              "index witness launch");
      auto result = (*runtime)->downloadBytes(*output);
      std::array<std::uint64_t, 2> expected{0x80000001ull, 0xffffffffffffffffull};
      auto reference = std::as_bytes(std::span(expected));
      require(result &&
                  std::equal(result->begin(), result->end(), reference.begin(), reference.end()),
              "unsigned count above 2^31 and signed scalar preserved");
    }
    auto limited = Runtime::create({.maxModules = 1});
    require(bool(limited), "module-limited runtime");
    auto path = std::filesystem::path(argv[1]) / "sm_80-f32-hardware.ptx";
    auto first = (*limited)->loadKernel(path);
    require(bool(first), "first module");
    require(bool((*limited)->loadKernel(path)), "identical module shares bounded cache");
    require(!(*limited)->loadKernel(std::filesystem::path(argv[1]) / "sm_80-f64-hardware.ptx"),
            "different module quota rejected");
    first = Kernel{};
    require(bool((*limited)->loadKernel(path)), "module quota retired");
    auto small = Runtime::create({.maxBytes = 1024});
    require(bool(small), "small-budget runtime");
    require(!(*small)->blankBytes(2048), "budget rejected");
    auto empty = (*runtime)->blankBytes(0);
    require(empty && (*runtime)->owns(*empty) && empty->bytes() == 0,
            "owned empty tensor without allocation");
    require(bool((*runtime)->downloadBytes(*empty)), "empty readback");
    // A retained buffer safely outlives its Runtime facade.
    auto survivor = (*runtime)->blankBytes(32);
    require(bool(survivor), "survivor");
    runtime->reset();
    survivor = Buffer{};
    std::cout << "PASS: " << launches
              << " exact CUDA launches; private contexts, immutable input, descriptor admission, "
                 "budget and retained ownership\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
