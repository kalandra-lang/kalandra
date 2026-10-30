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
  return check(parse(std::make_shared<SourceFile>("array-tensor.k", text)));
}
int main() {
  try {
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      auto typed = analyze("fn convert(input:[" + std::string(type) +
                           "]):\n  tensor(input)\n"
                           "fn main()->int:\n  0\n");
      require(typed.ok(), "array constructor rejected numeric type");
      const auto &fn = typed.module->functions[0];
      const auto &value = typed.module->expressions[fn.result];
      const auto &input = typed.module->symbols[fn.parameters[0]].type;
      require(value.type.tensor && !value.type.array && value.type.element == input.element &&
                  value.type.shape.size() == 1 &&
                  value.type.shape[0].identity == input.shape[0].identity &&
                  std::holds_alternative<ArrayToTensor>(value.operation),
              "constructor changed element/length identity or lost explicit category boundary");
      require(dumpTypedIR(*typed.module).find("tensor.from_array ordered_copy") !=
                  std::string::npos,
              "array constructor not represented in typed IR");
      require(bool(continuation::plan(*typed.module)), "constructor continuation planning failed");
#ifndef KALANDRA_ARRAY_TENSOR_FRONTEND_ONLY
      require(bool(lowerNative(*typed.module)), "array constructor lowering failed");
      for (unsigned surface = 0; surface < 6; ++surface) {
        auto forged = *typed.module;
        auto &node = forged.expressions[forged.functions[0].result];
        auto &construction = std::get<ArrayToTensor>(node.operation);
        if (surface == 0)
          construction.input = SIZE_MAX;
        if (surface == 1)
          node.type.array = true;
        if (surface == 2)
          node.type.shape[0].identity++;
        if (surface == 3)
          node.type.element =
              input.element == NumericType::Int32 ? NumericType::Float64 : NumericType::Int32;
        if (surface == 4)
          node.type.shape.clear();
        if (surface == 5)
          forged.expressions[construction.input].type.array = false;
        require(!lowerNative(forged), "forged constructor metadata admitted");
      }
#endif
    }
    for (auto text :
         {"fn run(values:[int],offset:int):\n  values -> map(fn(x): x+offset) -> tensor() -> "
          "reduceSum()\n",
          "fn run(values:[int])->Tensor[int,4]:\n  tensor(values)\n",
          "fn run():\n  values:[uint32]=[]\n  tensor(values)\n",
          "fn run():\n  values=[1,2,3]\n  converted=tensor(values)\n  "
          "reduceSum(converted)+reduceSum(tensor(values))\n",
          "fn run(flag:Bool):\n  values=if flag: [1,2]\n  else: [3,4,5]\n  tensor(values)\n",
          "fn run(flag:Bool)->Tensor[float32]:\n  value:float32=if flag: 1\n  else: 2.5\n  "
          "tensor(value)\n",
          "fn run(flag:Bool)->Tensor[float32,?]:\n  values:[float32]=if flag: []\n  else: []\n  "
          "tensor(values)\n",
          "fn run()->Tensor[int32,2]:\n  tensor([1,2])\n",
          "fn run()->Tensor[float32]:\n  tensor(-1.5)\n",
          "fn run(values:[int]) ! TaskError:\n  try parallel:\n    work=spawn tensor(values)\n    "
          "first=try await work\n    again=try await work\n    again\n",
          "fn run(values:[int],offset:int):\n  map(values,fn(x): reduceSum(tensor(map([x],fn(y): "
          "y+offset))))\n"}) {
      auto typed = analyze(std::string(text) + "fn main()->int:\n  0\n");
      require(typed.ok(), std::string("valid array/tensor constructor rejected: ") + text);
      require(bool(continuation::plan(*typed.module)), "constructor helper import failed");
#ifndef KALANDRA_ARRAY_TENSOR_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered),
              "constructor helper lowering failed: " + (lowered ? std::string{} : lowered.error()));
#endif
    }
    for (auto text :
         {"fn main():\n  values=[1,2]\n  tensor(values,[2,2])\n",
          "fn convert(values:[int])->Tensor[float32,?]:\n  tensor(values)\n",
          "fn convert(values:[int])->Tensor[int,?,?]:\n  tensor(values)\n",
          "fn convert(values:[int])->[int]:\n  tensor(values)\n", "fn main():\n  tensor([])\n",
          "fn main():\n  values=[1,2]\n  reduceSum(values)\n",
          "fn main():\n  values=tensor([1,2])\n  tensor(values)\n",
          "fn make()->[int]:\n  [1,2]\nfn main() ! TaskError:\n  try parallel:\n    pending=spawn "
          "make()\n    tensor(pending)\n"})
      require(!analyze(text).ok(), "invalid array/tensor boundary accepted");
    std::cout << "array-to-tensor: six numeric identities, literals and contextual scalars, "
                 "empty/dynamic lengths, explicit boundaries, captures, worker planning and "
                 "malformed IR gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
