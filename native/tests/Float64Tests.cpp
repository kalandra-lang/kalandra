#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("float64.k", text)));
}
#include "Float64TensorChecks.h"
int main() {
  try {
    checkFloat64Tensors();
    struct Literal {
      const char *text;
      std::uint64_t bits;
    };
    for (auto test :
         {Literal{"0", 0}, Literal{"1.0", 0x3ff0000000000000},
          Literal{"9007199254740993", 0x4340000000000000},
          Literal{"1.00000000000000011102230246251565404236316680908203125", 0x3ff0000000000000},
          Literal{"1.00000000000000033306690738754696212708950042724609375", 0x3ff0000000000002},
          Literal{"1.7976931348623157e308", 0x7fefffffffffffff},
          Literal{"2.2250738585072014e-308", 0x0010000000000000},
          Literal{"4.9406564584124654e-324", 1}, Literal{"1e-4000", 0}}) {
      auto typed = analyze(std::string("fn main() -> float:\n  ") + test.text + "\n");
      require(typed.ok(), test.text);
      auto value = typed.module->functions[0].result;
      require(typed.module->expressions[value].type.element == NumericType::Float64 &&
                  std::get<Float64Literal>(typed.module->expressions[value].operation).bits ==
                      test.bits,
              "literal did not round directly to binary64");
    }
    for (auto expr : {"1.5", "-0.0", "+1e0", "1 + 1.5", "1.5 + 1", "-(1 + 1.5)", "(1 + 2) * 1.5"}) {
      auto typed = analyze(std::string("fn main():\n  x = ") + expr + "\n  x\n");
      require(typed.ok() && typed.module->functions[0].resultType.element == NumericType::Float64,
              "default floating literal/expression inference failed");
    }
    struct Invalid {
      const char *text;
      const char *code;
    };
    for (auto test : {Invalid{"fn main() -> float64:\n  1.8e308\n", "S015"},
                      Invalid{"fn main():\n  -1e9999\n", "S015"},
                      Invalid{"fn main() -> float:\n  1 % 2\n", "S019"},
                      Invalid{"fn f(x:float64,y:float32) -> float:\n  x+y\n", "S010"},
                      Invalid{"fn f(x:float64) -> float32:\n  x\n", "S010"},
                      Invalid{"fn f(x:float32) -> float64:\n  x\n", "S010"},
                      Invalid{"fn f(x:int64) -> float:\n  x\n", "S010"},
                      Invalid{"fn main() -> float:\n  x=1\n  x\n", "S010"},
                      Invalid{"fn f(x:Tensor[float16,2]) -> float:\n  0\n", "S003"},
                      Invalid{"fn main() -> Tensor[float16,2]:\n  tensor([1,2])\n", "S003"}}) {
      auto typed = analyze(test.text);
      require(!typed.ok() && typed.diagnostics[0].code == test.code, test.text);
    }
    auto typed = analyze("fn math(a:float64,b:float64) -> float64:\n  -(a+b)*b/a\nfn main() -> "
                         "float:\n  math(1.5,2)\n");
    require(typed.ok(), "ordinary binary64 functions rejected");
    auto dump = dumpTypedIR(*typed.module);
    require(dump.find("f64.bits") != std::string::npos &&
                dump.find("add.f64 strict") != std::string::npos,
            "typed IR lost literal/arithmetic precision");
#ifndef KALANDRA_FLOAT64_FRONTEND_ONLY
    auto native = lowerNative(*typed.module, {}, {}, ArithmeticMode::Software);
    require(bool(native), "binary64 lowering failed");
    require(native->llvm.find("_mlir_ciface_kalandra_float64_entry") != std::string::npos &&
                native->llvm.find("fadd double") == std::string::npos &&
                native->llvm.find("i128") != std::string::npos,
            "binary64 entry or strict arithmetic lost");
    // Unsupported numeric identities fail before reaching scalar or buffer lowering.
    for (unsigned surface = 0; surface < 4; ++surface) {
      auto forged = *typed.module;
      ValueType *type = surface == 0   ? &forged.functions[0].resultType
                        : surface == 1 ? &*forged.functions[0].declaredResult
                        : surface == 2 ? &forged.symbols[0].type
                                       : &forged.expressions[0].type;
      type->element = static_cast<NumericType>(255);
      type->tensor = true;
      type->shape = {{2, 0}};
      require(!lowerNative(forged), "forged unsupported tensor reached backend lowering");
    }
#endif
    require(!numericCapabilityError(*typed.module, NumericTarget::AppleGpu),
            "binary64 GPU numerical capability rejected");
    auto task = analyze("fn identity(x:float) -> float:\n  x\nfn main() -> float ! TaskError:\n  "
                        "try parallel:\n    try await spawn identity(1.0000000000000002)\n");
    require(task.ok(), "float64 worker call rejected");
    auto planned = continuation::plan(*task.module);
    require(bool(planned), "float64 continuation rejected");
#ifndef KALANDRA_FLOAT64_FRONTEND_ONLY
    auto nativeTask = lowerNative(*task.module);
    require(bool(nativeTask) &&
                nativeTask->llvm.find("value.layout.scalar.float64") != std::string::npos,
            "distinct float64 worker layout lost");
#endif
    std::cout << "float64 literals/default float, direct rounding, strict scalar typing, worker "
                 "identity and tensor/GPU gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
