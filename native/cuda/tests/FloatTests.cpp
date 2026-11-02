#include "CudaRuntime.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime::cuda;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> value) {
  if (!value)
    throw std::runtime_error("CUDA error " + std::to_string(int(value.error())));
  return std::move(*value);
}
template <class Bits> void qualify(Runtime &runtime, const std::filesystem::path &directory) {
  constexpr unsigned width = sizeof(Bits) * 8, fraction = width == 32 ? 23 : 52,
                     exponent = width == 32 ? 8 : 11;
  constexpr Bits sign = Bits(1) << (width - 1), infinity = ((Bits(1) << exponent) - 1) << fraction,
                 nan = infinity | (Bits(1) << (fraction - 1));
  std::vector<Bits> edges{0,
                          sign,
                          1,
                          2,
                          sign | 1,
                          (Bits(1) << fraction) - 1,
                          Bits(1) << fraction,
                          (Bits(1) << fraction) + 1,
                          infinity - 1,
                          sign | (infinity - 1),
                          infinity,
                          sign | infinity,
                          nan,
                          nan | 1,
                          infinity | 1,
                          sign | nan};
  const unsigned bias = (1u << (exponent - 1)) - 1;
  for (unsigned exp : {1u, 2u, bias - 1, bias, bias + 1, 2 * bias - 1, 2 * bias})
    for (Bits mantissa :
         {Bits(0), Bits(1), (Bits(1) << (fraction - 1)) - 1, (Bits(1) << fraction) - 1})
      for (Bits s : {Bits(0), sign})
        edges.push_back(s | (Bits(exp) << fraction) | mantissa);
  std::vector<Bits> left, right;
  for (Bits a : edges)
    for (Bits b : edges) {
      left.push_back(a);
      right.push_back(b);
    }
  for (unsigned bit = 0; bit < fraction; ++bit)
    for (Bits s : {Bits(0), sign})
      for (Bits scale :
           {Bits(bias - 1) << fraction, Bits(bias) << fraction, Bits(bias + 1) << fraction}) {
        left.push_back(s | (Bits(1) << bit));
        right.push_back(scale);
      }
  std::uint64_t seed = 0xa3b41f2856c7d9e1ULL;
  const auto next = [&]() {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return Bits(seed);
  };
  for (unsigned i = 0; i < 32769; ++i) {
    left.push_back(next());
    right.push_back(next());
  }
  auto a = take(runtime.uploadBytes(std::as_bytes(std::span(left)))),
       b = take(runtime.uploadBytes(std::as_bytes(std::span(right))));
  const auto &semantics = width == 32 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
  for (const std::string operation : {"add", "sub", "mul", "div"}) {
    std::vector<Bits> expected;
    for (size_t i = 0; i < left.size(); ++i) {
      llvm::APFloat x(semantics, llvm::APInt(width, left[i])),
          y(semantics, llvm::APInt(width, right[i]));
      constexpr auto mode = llvm::APFloat::rmNearestTiesToEven;
      if (operation == "add")
        x.add(y, mode);
      else if (operation == "sub")
        x.subtract(y, mode);
      else if (operation == "mul")
        x.multiply(y, mode);
      else
        x.divide(y, mode);
      expected.push_back(Bits(x.bitcastToAPInt().getZExtValue()));
    }
    for (const std::string policy : {"hardware", "software", "hardware-with-software-fallback"}) {
      auto kernel = take(runtime.loadKernel(
          directory / ("f" + std::to_string(width) + "-" + operation + "-" + policy + ".ptx")));
      const std::array inputs{a, b};
      const std::array parameters{std::uint32_t(left.size()), UINT32_MAX};
      auto result =
          take(runtime.execute(kernel, inputs, parameters, left.size() * sizeof(Bits),
                               (left.size() + kernel.localSize() - 1) / kernel.localSize()));
      auto bytes = take(runtime.downloadBytes(result.output));
      check(bytes.size() == left.size() * sizeof(Bits), "float result extent");
      for (size_t i = 0; i < left.size(); ++i) {
        Bits actual;
        std::memcpy(&actual, bytes.data() + i * sizeof(Bits), sizeof(Bits));
        const bool isNan = (expected[i] & ~sign) > infinity;
        const bool canonical =
            policy == "software" || (width == 64 && policy == "hardware-with-software-fallback");
        const bool correct =
            isNan ? canonical ? actual == nan : (actual & ~sign) > infinity : actual == expected[i];
        if (!correct) {
          std::cerr << "f" << width << " " << operation << " " << policy << " case " << i
                    << " a=" << std::hex << left[i] << " b=" << right[i] << " actual=" << actual
                    << " expected=" << expected[i] << '\n';
          throw std::runtime_error("independent APFloat arithmetic mismatch");
        }
      }
      check(take(runtime.downloadBytes(a)) ==
                std::vector<std::byte>(std::as_bytes(std::span(left)).begin(),
                                       std::as_bytes(std::span(left)).end()),
            "left unchanged");
      check(take(runtime.downloadBytes(b)) ==
                std::vector<std::byte>(std::as_bytes(std::span(right)).begin(),
                                       std::as_bytes(std::span(right)).end()),
            "right unchanged");
      std::cout << "PASS f" << width << " " << operation << " " << policy << " " << left.size()
                << " APFloat results\n";
    }
  }
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected explicit float artifact directory");
    auto runtime = take(Runtime::create());
    qualify<std::uint32_t>(*runtime, argv[1]);
    qualify<std::uint64_t>(*runtime, argv[1]);
    check(!runtime->retainedBytes(), "float corpus ownership retired");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
