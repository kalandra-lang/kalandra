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
    for (auto source : {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
                        "f32", "f64"})
      for (auto destination : {"i8", "u8", "i16", "u16", "i32", "u32", "i64",
                               "u64", "f32", "f64"}) {
        auto typed = analyze("fn convert(value:" + std::string(source) + ") -> " + destination +
                             ":\n  return " + destination + "(value)\nfn main() -> i64:\n  return 0\n");
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
    for (auto source : {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
                        "f32", "f64"})
      for (auto destination : {"i8", "u8", "i16", "u16", "i32", "u32", "i64",
                               "u64", "f32", "f64"})
        for (auto shape : {"", ",?", ",2,?", ",2,0,?"}) {
          auto typed = analyze("fn convert(value:Tensor[" + std::string(source) + shape +
                               "]) -> Tensor[" + destination + shape + "]:\n  return " + destination +
                               "(value)\nfn main() -> i64:\n  return 0\n");
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
         {"fn main() -> f64:\n  count=3\n  return f64(count)/2.0\n",
          "fn main() -> u64:\n  return u64(-0.9)\n", "fn main() -> i32:\n  return i32(-2147483648.9)\n",
          "fn main() -> f32:\n  return f32(1.0/0.0)\n",
          "fn main() -> f32:\n  return f32(2.0e-100)\n"})
      require(analyze(source).ok(), "valid known conversion rejected");
    for (auto source :
         {"fn main() -> u64:\n  original:u64=18446744073709551615\n  "
          "return u64(f64(original))\n",
          "fn main() -> u32:\n  return u32(-1)\n", "fn main() -> i32:\n  return i32(2147483648)\n",
          "fn main() -> i64:\n  return i64(9223372036854775808.0)\n",
          "fn main() -> u64:\n  return u64(18446744073709551616.0)\n",
          "fn main() -> i32:\n  return i32(0.0/0.0)\n", "fn main() -> i32:\n  return i32(1.0/0.0)\n",
          "fn main() -> f32:\n  return f32(3.5e38)\n",
          "fn main() -> i64:\n  original:u64=18446744073709551615\n  return i64(original)\n",
          "fn main() -> f32:\n  original=3.5e38\n  return f32(original)\n"}) {
      auto typed = analyze(source);
      require(!typed.ok() && typed.diagnostics[0].code == "S055",
              "known invalid conversion not diagnosed");
    }
    for (auto source :
         {"fn f(x:Tensor[i64,?]) -> f64:\n  return f64(x)\n", "fn main() -> i64:\n  return i16(1)\n",
          "fn main() -> i64:\n  return i64(1,2)\n", "fn i32(x:i64) -> i64:\n  return x\n",
          "fn main() -> i64:\n  x=1\n  y:f64=2\n  return x+y\n"})
      require(!analyze(source).ok(), "invalid/implicit conversion admitted");
    auto tooLarge = analyze("fn f(value:Tensor[i32,1152921504606846976]) -> i64:\n"
                            "  converted=i64(value)\n  return reduceSum(converted)\n");
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
