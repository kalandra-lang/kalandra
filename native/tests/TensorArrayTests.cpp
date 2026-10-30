#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("tensor-array.k", text)));
}
int main() {
  try {
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"})
      for (unsigned rank : {0u, 1u, 2u, 3u, 8u}) {
        std::string input = "Tensor[" + std::string(type);
        for (unsigned i = 0; i < rank; ++i)
          input += ",?";
        input += "]";
        auto typed = analyze("fn change(values:" + input + ")->[" + type +
                             "]:\n  array(values)\nfn main()->int:\n  0\n");
        require(typed.ok(), "valid tensor-to-array rejected");
        const auto id = typed.module->functions[0].result;
        const auto &result = typed.module->expressions[id];
        require(std::holds_alternative<TensorToArray>(result.operation), "lost tensor-to-array IR");
        require(result.type.array && result.type.shape.size() == 1, "lost array result category");
        if (!rank)
          require(result.type.shape[0].constant == 1, "rank-zero count must be one");
        if (rank == 1) {
          const auto &op = std::get<TensorToArray>(result.operation);
          require(result.type.shape[0].identity ==
                      typed.module->expressions[op.input].type.shape[0].identity,
                  "rank-one length identity lost");
        }
        require(dumpTypedIR(*typed.module).find("array.from_tensor ordered_copy") !=
                    std::string::npos,
                "missing tensor-to-array dump");
        require(bool(continuation::plan(*typed.module)), "tensor-to-array continuation failed");
        require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                "CPU array operation admitted on GPU");
#ifndef KALANDRA_TENSOR_ARRAY_FRONTEND_ONLY
        auto lowered = lowerNative(*typed.module);
        require(bool(lowered), lowered ? "" : lowered.error());
        for (unsigned surface = 0; surface < 10; ++surface) {
          auto forged = *typed.module;
          auto &node = forged.expressions[id];
          auto &op = std::get<TensorToArray>(node.operation);
          if (surface == 0)
            op.input = SIZE_MAX;
          if (surface == 1)
            node.type.array = false;
          if (surface == 2)
            node.type.shape.clear();
          if (surface == 3)
            node.type.element =
                node.type.element == NumericType::Int32 ? NumericType::Float32 : NumericType::Int32;
          if (surface == 4)
            forged.expressions[op.input].type.array = true;
          if (surface == 5)
            forged.expressions[op.input].type.device = 0;
          if (surface == 6)
            node.type.fallible = true;
          if (surface == 7)
            node.type.task = true;
          if (surface == 8)
            forged.expressions[op.input].type.task = true;
          if (surface == 9)
            forged.expressions[op.input].type.tensor = false;
          require(!lowerNative(forged), "malformed tensor-to-array IR admitted");
        }
#endif
      }
    for (auto source :
         {"fn main():\n  array(tensor([[1,2],[3,4]]))[2]\n",
          "fn main():\n  length(array(tensor(7)))\n",
          "fn main():\n  values:Tensor[float32,0,3]=tensor([],[0,3])\n  length(array(values))\n",
          "fn main():\n  tensor([[1,2],[3,4]]) -> array() -> map(fn(x):x*2) -> tensor() -> "
          "reduceSum()\n",
          "fn change(values:Tensor[int,?,?]):\n  map([1,2],fn(x):array(values)[x])\nfn main():\n  "
          "change(tensor([[1,2],[3,4]]))[1]\n",
          "fn main() ! TaskError:\n  try parallel:\n    work=spawn array(tensor([[1,2],[3,4]]))\n  "
          "  values=try await work\n    values[2]\n",
          "fn make():\n  tensor([[1,2],[3,4]])\nfn main() ! TaskError:\n  try parallel:\n    "
          "work=spawn make()\n    array(try await work)[2]\n",
          "fn array(x:int):\n  x+1\nfn main():\n  array(1)\n"}) {
      auto typed = analyze(source);
      require(typed.ok(), std::string("valid tensor-to-array program rejected: ") + source);
      require(bool(continuation::plan(*typed.module)), "helper import failed");
#ifndef KALANDRA_TENSOR_ARRAY_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto source :
         {"fn main():\n  array()\n", "fn main():\n  array(tensor([1]),tensor([2]))\n",
          "fn main():\n  array(1)\n", "fn main():\n  array([1,2])\n", "fn main():\n  array(true)\n",
          "fn change(values:Tensor[int,?])->[float32]:\n  array(values)\n",
          "fn change(values:Tensor[int,?])->Tensor[int,?]:\n  array(values)\n",
          "fn main():\n  array=1\n  array(tensor([2]))\n",
          "fn main() ! TaskError:\n  try parallel:\n    work=spawn tensor([1,2])\n    "
          "array(work)\n"})
      require(!analyze(source).ok(), std::string("invalid tensor-to-array admitted: ") + source);
    auto resident = check(
        parse(std::make_shared<SourceFile>(
            "resident.k", "fn main() ! TaskError:\n  try parallel:\n    "
                                 "values:Tensor[float32,2]=tensor([1,2])\n    resident=try "
                                 "await transfer(values,devices.gpu())\n    array(resident)\n")),
        {}, {.placement = true});
    bool diagnostic = false;
    for (const auto &d : resident.diagnostics)
      diagnostic |= d.code == "S026";
    require(!resident.ok() && diagnostic, "resident tensor bypassed array construction gate");
    for (auto input : {"Tensor[int,2,3]", "Tensor[int,0,?]", "Tensor[int]"}) {
      auto typed = analyze("fn change(values:" + std::string(input) +
                           "):\n  array(values)\nfn main()->int:\n  0\n");
      require(typed.ok(), "known count program failed");
      auto expected = std::string(input) == "Tensor[int,2,3]" ? 6
                      : std::string(input) == "Tensor[int]"   ? 1
                                                              : 0;
      require(typed.module->functions[0].resultType.shape[0].constant == expected,
              "known flattened count lost");
    }
    std::cout << "tensor-to-array: six types, ranks zero/one/two/three/eight, length facts, "
                 "closures, pipelines, workers, diagnostics and malformed IR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
