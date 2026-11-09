#include "kalandra/Backend.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("int32.k", source)));
}
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (const auto *value : {"2147483647", "-2147483648", "+17", "-5 % 2", "5 % -2",
                              "-2147483648 % -1", "(2147483647 - 2) + 1"}) {
      auto result = analyze(std::string("fn main() -> i32:\n  return ") + value + "\n");
      require(result.ok(), value);
#ifndef KALANDRA_INT32_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), "int32 failed to lower");
      require(lowered->llvm.find("_mlir_ciface_kalandra_int32_entry") != std::string::npos,
              "integer CLI lost typed ABI");
#endif
    }
    for (const auto *value : {"2147483648", "-2147483649", "1.0", "1e0", "18446744073709551616",
                              "2147483647 + 1", "-2147483648 - 1", "1073741824 * 2",
                              "-(-2147483648)", "-2147483648 / -1", "3 / 0", "3 % 0"}) {
      auto result = analyze(std::string("fn main() -> i32:\n  return ") + value + "\n");
      require(!result.ok(), value);
      require(result.diagnostics.front().code == "S051" ||
                  result.diagnostics.front().code == "S052",
              "wrong literal/arithmetic diagnostic");
    }
    auto known = analyze("fn main() -> i32:\n  x:i32 = 2147483647\n  return x + 1\n");
    require(!known.ok() && known.diagnostics.front().code == "S052",
            "known binding overflow escaped");
    known = analyze("fn divide(x:i32) -> i32:\n  z:i32 = 0\n  return x / z\n"
                    "fn main() -> i32:\n  return divide(1)\n");
    require(!known.ok() && known.diagnostics.front().code == "S052", "known zero divisor escaped");
    for (const auto *source : {"fn main() -> i32:\n  x = 1\n  return x\n",
                               "fn f(x:f32) -> i32:\n  return x + 1\nfn main() -> i32:\n  return f(1)\n",
                               "fn f(x:i32) -> f32:\n  return x\nfn main() -> f32:\n  return f(1)\n"})
      require(!analyze(source).ok(), "implicit conversion/default/tensor accepted");
    for (const auto *source :
         {"fn values() -> Tensor[i32,2]:\n  return tensor([2147483647,-2147483648])\n",
          "fn values() -> Tensor[i32]:\n  return tensor(16777217)\n",
          "fn values(a:Tensor[i32,?]) -> Tensor[i32,?]:\n  return 2 % (a + 1)\n",
          "fn values() -> Tensor[i32,2,0]:\n  return tensor([], [2,0])\n",
          "fn values(x:Tensor[i32,?]) -> i32:\n  return reduceSum(x)\n",
          "fn values(x:Tensor[i32,?,?]) -> Tensor[i32,?,?]:\n  return matmul(x,x)\n"}) {
      auto tensor = analyze(std::string(source) + "fn main() -> i32:\n  return 0\n");
      require(tensor.ok(), "int32 tensor source rejected");
#ifndef KALANDRA_INT32_FRONTEND_ONLY
      require(bool(lowerNative(*tensor.module)), "int32 tensor lowering rejected");
#endif
    }
    for (const auto *source :
         {"fn main() -> i32:\n  x:Tensor[i32,1] = tensor([2147483648])\n  return 0\n",
          "fn main() -> i32:\n  x:Tensor[i32,1] = tensor([1.5])\n  return 0\n",
          "fn f(x:Tensor[i32,?],y:Tensor[f32,?]) -> Tensor[i32,?]:\n  return x + y\n",
          "fn f(x:Tensor[i32,?,?],y:Tensor[f32,?,?]) -> Tensor[i32,?,?]:\n  return matmul(x,y)\n",
          "fn f(x:Tensor[i32,2,3],y:Tensor[i32,4,2]) -> Tensor[i32,2,2]:\n  return matmul(x,y)\n",
          "fn f(x:Tensor[i32,2]) -> Tensor[i32,3]:\n  return x\n",
          "fn f() -> Tensor[i32,2,2]:\n  return tensor([[1],[2,3]])\n"})
      require(!analyze(source).ok(), "unsupported/malformed integer tensor operation accepted");
#ifndef KALANDRA_INT32_FRONTEND_ONLY
    auto tensorTask = analyze("fn f(x:Tensor[i32,?]) -> Tensor[i32,?]:\n  return x + 1\n"
                              "fn main() -> i32 ! TaskError:\n  return try parallel:\n"
                              "    x:Tensor[i32,2] = tensor([1,2])\n"
                              "    work = spawn f(x)\n    try await work\n    0\n");
    require(tensorTask.ok(), "typed int32 tensor task IR rejected");
    auto loweredTensorTask = lowerNative(*tensorTask.module);
    require(bool(loweredTensorTask), "int32 tensor task handoff rejected");
    auto invalidExport = lowerNative(*tensorTask.module, "f");
    require(!invalidExport && invalidExport.error().find("parameterless") != std::string::npos,
            "private task export accepted unsupported parameter ABI");
#endif
    auto functions = analyze("fn inc(x:i32) -> i32:\n  return x + 1\n"
                             "fn main() -> i32:\n  a:i32 = 16777217\n  return inc(a)\n");
    require(functions.ok(), "typed function/literal contexts failed");
#ifndef KALANDRA_INT32_FRONTEND_ONLY
    require(bool(lowerNative(*functions.module)), "typed functions failed to lower");
#endif
    auto task =
        analyze("fn f(x:i32) -> i32:\n  return x + 1\n"
                "fn main() -> i32 ! TaskError:\n  return try parallel:\n    try await spawn f(1)\n");
    require(task.ok(), "int32 task frontend regressed");
#ifndef KALANDRA_INT32_FRONTEND_ONLY
    auto loweredTask = lowerNative(*task.module);
    require(bool(loweredTask), "int32 task backend rejected typed captures/results");
#endif
    require(!numericCapabilityError(*functions.module, NumericTarget::AppleGpu),
            "int32 GPU arithmetic capability rejected");
    std::cout << "Checked int32 literals, constants, calls and unsupported target gates\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
