#include "kalandra/Backend.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("uint32.k", source)));
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (const auto *value :
         {"0", "4294967295", "2147483648", "16777217", "+17", "-0", "4294967295 / 2",
          "4294967295 % 2", "4294967295 * 0", "(4294967295 - 2) + 1"}) {
      auto checked = analyze(std::string("fn main() -> u32:\n  return ") + value + "\n");
      require(checked.ok(), value);
#ifndef KALANDRA_UINT32_FRONTEND_ONLY
      auto lowered = lowerNative(*checked.module);
      require(bool(lowered), "uint32 lowering rejected");
      require(lowered->llvm.find("_mlir_ciface_kalandra_uint32_entry") != std::string::npos,
              "uint32 CLI entry lost");
#endif
    }
    for (const auto *value :
         {"4294967296", "-1", "-4294967295", "1.0", "1e0", "18446744073709551616", "4294967295 + 1",
          "0 - 1", "65536 * 65536", "4294967295 * 4294967295", "-4294967295", "1 / 0", "1 % 0"}) {
      auto checked = analyze(std::string("fn main() -> u32:\n  return ") + value + "\n");
      require(!checked.ok(), value);
      require(checked.diagnostics.front().code == "S051" ||
                  checked.diagnostics.front().code == "S052",
              "wrong constant diagnostic");
    }
    for (const auto *source :
         {"fn main() -> u32:\n  x:u32 = 4294967295\n  return x * x\n",
          "fn main() -> u32:\n  x:u32 = 1\n  return -x\n", "fn main() -> u32:\n  x = 1\n  return x\n",
          "fn f(x:i32) -> u32:\n  return x\nfn main() -> u32:\n  return f(1)\n",
          "fn f(x:u32) -> i32:\n  return x\nfn main() -> i32:\n  return f(1)\n",
          "fn f(x:u32,y:i32) -> u32:\n  return x + y\nfn main() -> u32:\n  return f(1,1)\n",
          "fn f(x:u32,y:f32) -> u32:\n  return x + y\nfn main() -> u32:\n  return f(1,1)\n",
          "fn f(x:Tensor[u32,?,?],y:Tensor[i32,?,?]) -> Tensor[u32,?,?]:\n  return matmul(x,y)\n",
          "fn f(x:Tensor[u32,?,?],y:Tensor[f32,?,?]) -> Tensor[u32,?,?]:\n  return "
          "matmul(x,y)\n",
          "fn f(x:Tensor[u32,2,3],y:Tensor[u32,4,2]) -> Tensor[u32,2,2]:\n  return matmul(x,y)\n",
          "fn f(x:Tensor[u32,?]) -> Tensor[u32,?,?]:\n  return matmul(x,x)\n",
          "fn f() -> Tensor[u32,1]:\n  return tensor([4294967296])\n",
          "fn f() -> Tensor[u32,1]:\n  return tensor([-1])\n",
          "fn f() -> Tensor[u32,2,2]:\n  return tensor([[1],[2,3]])\n",
          "fn f(x:Tensor[u32,?],y:Tensor[i32,?]) -> Tensor[u32,?]:\n  return x + y\n",
          "fn f(x:Tensor[u32,?],y:Tensor[f32,?]) -> Tensor[u32,?]:\n  return x + y\n"})
      require(!analyze(source).ok(), "unsupported conversion/default/tensor accepted");
    for (const auto *source :
         {"fn f(x:Tensor[u32,?]) -> u32:\n  return reduceSum(x)\n",
          "fn f(x:Tensor[u32,?,?]) -> Tensor[u32,?,?]:\n  return matmul(x,x)\n",
          "fn f() -> Tensor[u32]:\n  return tensor(4294967295)\n",
          "fn f() -> Tensor[u32,2,0]:\n  return tensor([],[2,0])\n",
          "fn f(x:Tensor[u32,?],s:u32) -> Tensor[u32,?]:\n  return s % (x + 1)\n",
          "fn f() -> Tensor[u32,2,2]:\n  return tensor([[0,4294967295],[2147483648,16777217]])\n"}) {
      auto tensor = analyze(std::string(source) + "fn main() -> u32:\n  return 0\n");
      require(tensor.ok(), "uint32 tensor source rejected");
#ifndef KALANDRA_UINT32_FRONTEND_ONLY
      require(bool(lowerNative(*tensor.module)), "uint32 synchronous tensor lowering rejected");
#endif
    }
    auto knownZero = analyze(
        "fn f(x:u32) -> u32:\n  z:u32 = 0\n  return x / z\nfn main() -> u32:\n  return f(1)\n");
    require(!knownZero.ok() && knownZero.diagnostics.front().code == "S052",
            "known zero divisor escaped");
    auto task = analyze("fn inc(x:u32) -> u32:\n  return x + 1\nfn main() -> u32 ! TaskError:\n  return "
                        "try parallel:\n    try await spawn inc(2147483648)\n");
    require(task.ok(), "uint32 task rejected");
#ifndef KALANDRA_UINT32_FRONTEND_ONLY
    auto loweredTask = lowerNative(*task.module);
    require(bool(loweredTask), "uint32 task lowering rejected");
    require(loweredTask->llvm.find("value.layout.scalar.uint32") != std::string::npos,
            "unsigned task identity lost");
    auto tensorTask =
        analyze("fn f(x:Tensor[u32,?]) -> Tensor[u32,?]:\n  return x + 1\n"
                "fn main() -> u32 ! TaskError:\n  return try parallel:\n"
                "    x:Tensor[u32,2] = tensor([1,2])\n    try await spawn f(x)\n    0\n");
    require(tensorTask.ok(), "uint32 tensor task typed IR rejected");
    auto loweredTensorTask = lowerNative(*tensorTask.module);
    require(bool(loweredTensorTask), "uint32 tensor task handoff rejected");
    auto invalidExport = lowerNative(*tensorTask.module, "f");
    require(!invalidExport && invalidExport.error().find("parameterless") != std::string::npos,
            "private unsigned task export accepted unsupported parameter ABI");
    auto reduction = analyze("fn f(x:Tensor[u32,?]) -> u32:\n  return reduceSum(x)\n"
                             "fn main() -> u32:\n  return 0\n");
    require(reduction.ok() && bool(lowerNative(*reduction.module)), "uint32 reduction rejected");
    require(dumpTypedIR(*reduction.module).find("reduceSum checked_u32") != std::string::npos,
            "unsigned reduction lost its typed numerical policy");
    for (auto &expr : reduction.module->expressions)
      if (std::holds_alternative<ReduceSum>(expr.operation))
        expr.type.element = NumericType::Int32;
    require(!lowerNative(*reduction.module), "backend accepted mismatched reduction element");
    auto multiply =
        analyze("fn f(x:Tensor[u32,?,?],y:Tensor[u32,?,?]) -> Tensor[u32,?,?]:\n"
                "  return matmul(x,y)\nfn main() -> u32:\n  return 0\n");
    require(multiply.ok() && bool(lowerNative(*multiply.module)), "uint32 matmul rejected");
    require(dumpTypedIR(*multiply.module).find("matmul checked_u32") != std::string::npos,
            "unsigned matmul lost its numerical policy");
    for (unsigned mismatch = 0; mismatch < 3; ++mismatch) {
      auto forged = *multiply.module;
      for (auto &expr : forged.expressions)
        if (auto *op = std::get_if<Matmul>(&expr.operation)) {
          if (mismatch == 0)
            expr.type.element = NumericType::Int32;
          else
            forged.expressions[mismatch == 1 ? op->left : op->right].type.element =
                NumericType::Int32;
        }
      require(!lowerNative(forged), "backend accepted mismatched matmul elements");
    }

#endif
    require(!numericCapabilityError(*task.module, NumericTarget::AppleGpu),
            "supported unsigned GPU numeric capability rejected");
    std::cout << "Checked uint32 literals, constants, unsigned task identity and strict "
                 "type/target gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
