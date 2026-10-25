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
      auto result = analyze(std::string("fn main() -> int32:\n  ") + value + "\n");
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
      auto result = analyze(std::string("fn main() -> int32:\n  ") + value + "\n");
      require(!result.ok(), value);
      require(result.diagnostics.front().code == "S051" ||
                  result.diagnostics.front().code == "S052",
              "wrong literal/arithmetic diagnostic");
    }
    auto known = analyze("fn main() -> int32:\n  x:int32 = 2147483647\n  x + 1\n");
    require(!known.ok() && known.diagnostics.front().code == "S052",
            "known binding overflow escaped");
    known = analyze("fn divide(x:int32) -> int32:\n  z:int32 = 0\n  x / z\n"
                    "fn main() -> int32:\n  divide(1)\n");
    require(!known.ok() && known.diagnostics.front().code == "S052", "known zero divisor escaped");
    for (const auto *source : {"fn main() -> int32:\n  x = 1\n  x\n",
                               "fn f(x:float32) -> int32:\n  x + 1\nfn main() -> int32:\n  f(1)\n",
                               "fn f(x:int32) -> float32:\n  x\nfn main() -> float32:\n  f(1)\n"})
      require(!analyze(source).ok(), "implicit conversion/default/tensor accepted");
    for (const auto *source :
         {"fn values() -> Tensor[int32,2]:\n  tensor([2147483647,-2147483648])\n",
          "fn values() -> Tensor[int32]:\n  tensor(16777217)\n",
          "fn values(a:Tensor[int32,?]) -> Tensor[int32,?]:\n  2 % (a + 1)\n",
          "fn values() -> Tensor[int32,2,0]:\n  tensor([], [2,0])\n",
          "fn values(x:Tensor[int32,?]) -> int32:\n  reduceSum(x)\n",
          "fn values(x:Tensor[int32,?,?]) -> Tensor[int32,?,?]:\n  matmul(x,x)\n"}) {
      auto tensor = analyze(std::string(source) + "fn main() -> int32:\n  0\n");
      require(tensor.ok(), "int32 tensor source rejected");
#ifndef KALANDRA_INT32_FRONTEND_ONLY
      require(bool(lowerNative(*tensor.module)), "int32 tensor lowering rejected");
#endif
    }
    for (const auto *source :
         {"fn main() -> int32:\n  x:Tensor[int32,1] = tensor([2147483648])\n  0\n",
          "fn main() -> int32:\n  x:Tensor[int32,1] = tensor([1.5])\n  0\n",
          "fn f(x:Tensor[int32,?],y:Tensor[float32,?]) -> Tensor[int32,?]:\n  x + y\n",
          "fn f(x:Tensor[int32,?,?],y:Tensor[float32,?,?]) -> Tensor[int32,?,?]:\n  matmul(x,y)\n",
          "fn f(x:Tensor[int32,2,3],y:Tensor[int32,4,2]) -> Tensor[int32,2,2]:\n  matmul(x,y)\n",
          "fn f(x:Tensor[int32,2]) -> Tensor[int32,3]:\n  x\n",
          "fn f() -> Tensor[int32,2,2]:\n  tensor([[1],[2,3]])\n"})
      require(!analyze(source).ok(), "unsupported/malformed integer tensor operation accepted");
#ifndef KALANDRA_INT32_FRONTEND_ONLY
    auto tensorTask = analyze("fn f(x:Tensor[int32,?]) -> Tensor[int32,?]:\n  x + 1\n"
                              "fn main() -> int32 ! TaskError:\n  try parallel:\n"
                              "    x:Tensor[int32,2] = tensor([1,2])\n"
                              "    work = spawn f(x)\n    try await work\n    0\n");
    require(tensorTask.ok(), "typed int32 tensor task IR rejected");
    auto loweredTensorTask = lowerNative(*tensorTask.module);
    require(bool(loweredTensorTask), "int32 tensor task handoff rejected");
    auto invalidExport = lowerNative(*tensorTask.module, "f");
    require(!invalidExport && invalidExport.error().find("parameterless") != std::string::npos,
            "private task export accepted unsupported parameter ABI");
#endif
    auto functions = analyze("fn inc(x:int32) -> int32:\n  x + 1\n"
                             "fn main() -> int32:\n  a:int32 = 16777217\n  inc(a)\n");
    require(functions.ok(), "typed function/literal contexts failed");
#ifndef KALANDRA_INT32_FRONTEND_ONLY
    require(bool(lowerNative(*functions.module)), "typed functions failed to lower");
#endif
    auto task =
        analyze("fn f(x:int32) -> int32:\n  x + 1\n"
                "fn main() -> int32 ! TaskError:\n  try parallel:\n    try await spawn f(1)\n");
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
