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
    for (auto type : {"i32", "u32", "i64", "u64", "f32", "f64"}) {
      auto typed = analyze("fn convert(input:[" + std::string(type) +
                           "]):\n  tensor(input)\n"
                           "fn main()->i64:\n  0\n");
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
         {"fn run(values:[i64],offset:i64):\n  return values -> map(fn(x): return x+offset) -> tensor() -> "
          "reduceSum()\n",
          "fn run(values:[i64])->Tensor[i64,4]:\n  return tensor(values)\n",
          "fn run():\n  values:[u32]=[]\n  return tensor(values)\n",
          "fn run():\n  values=[1,2,3]\n  converted=tensor(values)\n  "
          "return reduceSum(converted)+reduceSum(tensor(values))\n",
          "fn run(flag:bool):\n  values=if flag: [1,2]\n  else: [3,4,5]\n  return tensor(values)\n",
          "fn run(flag:bool)->Tensor[f32]:\n  value:f32=if flag: 1\n  else: 2.5\n  "
          "return tensor(value)\n",
          "fn run(flag:bool)->Tensor[f32,?]:\n  values:[f32]=if flag: []\n  else: []\n  "
          "return tensor(values)\n",
          "fn run()->Tensor[i32,2]:\n  return tensor([1,2])\n",
          "fn run()->Tensor[f32]:\n  return tensor(-1.5)\n",
          "fn run(values:[i64]) ! TaskError:\n  return try parallel:\n    work=spawn tensor(values)\n    "
          "first=try await work\n    again=try await work\n    again\n",
          "fn run(values:[i64],offset:i64):\n  return map(values,fn(x): return reduceSum(tensor(map([x],fn(y): "
          "return y+offset))))\n"}) {
      auto typed = analyze(std::string(text) + "fn main()->i64:\n  return 0\n");
      require(typed.ok(), std::string("valid array/tensor constructor rejected: ") + text);
      require(bool(continuation::plan(*typed.module)), "constructor helper import failed");
#ifndef KALANDRA_ARRAY_TENSOR_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered),
              "constructor helper lowering failed: " + (lowered ? std::string{} : lowered.error()));
#endif
    }
    for (auto text :
         {"fn main():\n  values=[1,2]\n  return tensor(values,[2,2])\n",
          "fn convert(values:[i64])->Tensor[f32,?]:\n  return tensor(values)\n",
          "fn convert(values:[i64])->Tensor[i64,?,?]:\n  return tensor(values)\n",
          "fn convert(values:[i64])->[i64]:\n  return tensor(values)\n", "fn main():\n  return tensor([])\n",
          "fn main():\n  values=[1,2]\n  return reduceSum(values)\n",
          "fn main():\n  values=tensor([1,2])\n  return tensor(values)\n",
          "fn make()->[i64]:\n  return [1,2]\nfn main() ! TaskError:\n  return try parallel:\n    pending=spawn "
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
