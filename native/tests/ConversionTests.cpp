#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("conversion.k", text)));
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (auto source : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64",
                        "float32", "float64"})
      for (auto destination : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "int64",
                               "uint64", "float32", "float64"}) {
        auto typed = analyze("fn convert(value:" + std::string(source) + ") -> " + destination +
                             ":\n  " + destination + "(value)\nfn main() -> int64:\n  0\n");
        require(typed.ok(), "scalar conversion rejected");
        auto lowered = lowerNative(*typed.module, {}, {}, ArithmeticMode::Software);
        require(bool(lowered), "conversion lowering rejected");
        require(lowered->llvm.find("fptosi") == std::string::npos &&
                    lowered->llvm.find("fptoui") == std::string::npos &&
                    lowered->llvm.find("sitofp") == std::string::npos &&
                    lowered->llvm.find("uitofp") == std::string::npos &&
                    lowered->llvm.find("fpext") == std::string::npos &&
                    lowered->llvm.find("fptrunc") == std::string::npos,
                "conversion bypassed integer lowering");
        require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                "GPU conversion admitted");
        auto planned = continuation::plan(*typed.module);
        require(bool(planned), "conversion planner rejected");
        auto forged = *typed.module;
        for (auto &expr : forged.expressions)
          if (std::holds_alternative<NumericConvert>(expr.operation))
            expr.type.tensor = true;
        require(!lowerNative(forged), "scalar-to-tensor conversion IR admitted");
      }
    for (auto source : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64",
                        "float32", "float64"})
      for (auto destination : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "int64",
                               "uint64", "float32", "float64"})
        for (auto shape : {"", ",?", ",2,?", ",2,0,?"}) {
          auto typed = analyze("fn convert(value:Tensor[" + std::string(source) + shape +
                               "]) -> Tensor[" + destination + shape + "]:\n  " + destination +
                               "(value)\nfn main() -> int64:\n  0\n");
          require(typed.ok(), "tensor conversion rejected");
          require(bool(lowerNative(*typed.module)), "tensor conversion lowering rejected");
          require(bool(continuation::plan(*typed.module)), "tensor conversion planner rejected");
          require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                  "GPU tensor conversion admitted");
          auto forged = *typed.module;
          for (auto &expr : forged.expressions)
            if (std::holds_alternative<NumericConvert>(expr.operation))
              expr.type.shape.push_back(Extent{2});
          require(!lowerNative(forged), "shape-changing conversion IR admitted");
        }
    for (auto source :
         {"fn main() -> float:\n  count=3\n  float(count)/2.0\n",
          "fn main() -> uint64:\n  uint(-0.9)\n", "fn main() -> int32:\n  int32(-2147483648.9)\n",
          "fn main() -> float32:\n  float32(1.0/0.0)\n",
          "fn main() -> float32:\n  float32(2.0e-100)\n"})
      require(analyze(source).ok(), "valid known conversion rejected");
    for (auto source :
         {"fn main() -> uint64:\n  original:uint64=18446744073709551615\n  "
          "uint64(float64(original))\n",
          "fn main() -> uint32:\n  uint32(-1)\n", "fn main() -> int32:\n  int32(2147483648)\n",
          "fn main() -> int64:\n  int64(9223372036854775808.0)\n",
          "fn main() -> uint64:\n  uint64(18446744073709551616.0)\n",
          "fn main() -> int32:\n  int32(0.0/0.0)\n", "fn main() -> int32:\n  int32(1.0/0.0)\n",
          "fn main() -> float32:\n  float32(3.5e38)\n",
          "fn main() -> int64:\n  original:uint64=18446744073709551615\n  int64(original)\n",
          "fn main() -> float32:\n  original=3.5e38\n  float32(original)\n"}) {
      auto typed = analyze(source);
      require(!typed.ok() && typed.diagnostics[0].code == "S055",
              "known invalid conversion not diagnosed");
    }
    for (auto source :
         {"fn f(x:Tensor[int64,?]) -> float:\n  float(x)\n", "fn main() -> int64:\n  int16(1)\n",
          "fn main() -> int64:\n  int64(1,2)\n", "fn int32(x:int64) -> int64:\n  x\n",
          "fn main() -> int64:\n  x=1\n  y:float=2\n  x+y\n"})
      require(!analyze(source).ok(), "invalid/implicit conversion admitted");
    auto tooLarge = analyze("fn f(value:Tensor[int32,1152921504606846976]) -> int64:\n"
                            "  converted=int64(value)\n  reduceSum(converted)\n");
    require(!tooLarge.ok() && tooLarge.diagnostics[0].code == "S002",
            "conversion destination byte overflow admitted");
    std::cout << "36 CPU scalar conversion pairs: admission, aliases, constant checks, strict "
                 "integer lowering, planner and GPU gates; 144 shape-preserving tensor signatures "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
