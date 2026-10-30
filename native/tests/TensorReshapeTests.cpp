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
  return check(parse(std::make_shared<SourceFile>("tensor-reshape.k", text)));
}
int main() {
  try {
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"})
      for (unsigned rank : {0u, 1u, 2u, 3u, 8u})
        for (auto dim : {"int32", "uint32", "int64", "uint64"}) {
          std::string input = "Tensor[" + std::string(type);
          for (unsigned i = 0; i < rank; ++i)
            input += ",?";
          input += "]";
          auto typed = analyze("fn change(values:" + input + ",rows:" + dim + ",columns:" + dim +
                               "):\n  reshape(values,[rows,columns])\nfn main()->int:\n  0\n");
          require(typed.ok(), "valid numeric tensor reshape rejected");
          auto id = typed.module->functions[0].result;
          require(std::holds_alternative<TensorReshape>(typed.module->expressions[id].operation),
                  "reshape typed operation lost");
          require(dumpTypedIR(*typed.module).find("tensor.reshape ordered_copy") !=
                      std::string::npos,
                  "reshape missing from IR dump");
          require(bool(continuation::plan(*typed.module)), "reshape continuation planning failed");
          require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                  "reshape silently enabled in GPU kernel");
#ifndef KALANDRA_TENSOR_RESHAPE_FRONTEND_ONLY
          auto lowered = lowerNative(*typed.module);
          require(bool(lowered), lowered ? "" : lowered.error());
          for (unsigned surface = 0; surface < 10; ++surface) {
            auto forged = *typed.module;
            auto &node = forged.expressions[id];
            auto &op = std::get<TensorReshape>(node.operation);
            if (surface == 0)
              op.input = SIZE_MAX;
            if (surface == 1)
              op.dimensions[0] = SIZE_MAX;
            if (surface == 2)
              node.type.array = true;
            if (surface == 3)
              node.type.element = node.type.element == NumericType::Float64 ? NumericType::Int32
                                                                            : NumericType::Float64;
            if (surface == 4)
              forged.expressions[op.input].type.array = true;
            if (surface == 5)
              forged.expressions[op.input].type.device = 0;
            if (surface == 6)
              node.type.fallible = true;
            if (surface == 7)
              node.type.shape.clear();
            if (surface == 8)
              forged.expressions[op.dimensions[0]].type.element = NumericType::Float32;
            if (surface == 9)
              forged.expressions[op.dimensions[0]].type.task = true;
            require(!lowerNative(forged), "malformed reshape IR admitted");
          }
#endif
        }
    for (auto source :
         {"fn main():\n  reshape(tensor([[1,2],[3,4]]),[4])[2]\n",
          "fn main():\n  reshape(tensor(7),[1,1])[0,0]\n",
          "fn main():\n  reshape(tensor([[7]]),[])[]\n",
          "fn main():\n  empty:Tensor[float32,0,3]=tensor([],[0,3])\n  "
          "reduceSum(reshape(empty,[3,0]))\n",
          "fn change(values:Tensor[int,?,?],a:int32,b:uint32)->Tensor[int,2,2]:\n  "
          "reshape(values,[a,b])\nfn main()->int:\n  0\n",
          "fn change(values:Tensor[int,?,?]):\n  "
          "map([1,2],fn(x):reduceSum(reshape(values,[x,2])))\nfn main()->int:\n  0\n",
          "fn main() ! TaskError:\n  try parallel:\n    work=spawn "
          "reshape(tensor([[1,2],[3,4]]),[4])\n    values=try await work\n    values[2]\n",
          "fn dims()->int:\n  2\nfn main() ! TaskError:\n  try parallel:\n    work=spawn dims()\n  "
          "  reduceSum(reshape(tensor([[1,2],[3,4]]),[try await work,2]))\n",
          "fn reshape(value:int):\n  value+1\nfn main():\n  reshape(1)\n"}) {
      auto typed = analyze(source);
      require(typed.ok(), std::string("valid reshape program rejected: ") + source);
      require(bool(continuation::plan(*typed.module)), "reshape helper import failed");
#ifndef KALANDRA_TENSOR_RESHAPE_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto source :
         {"fn main():\n  reshape([1,2],[2])\n", "fn main():\n  reshape(1,[])\n",
          "fn main():\n  reshape(tensor([1,2]),[3])\n", "fn main():\n  reshape(tensor([1,2]),[])\n",
          "fn main():\n  reshape(tensor([1,2]),[-1,2])\n",
          "fn main():\n  reshape(tensor([1,2]),[1.0,2])\n",
          "fn main():\n  reshape(tensor([1,2]),[true,2])\n",
          "fn main():\n  reshape(tensor([1,2]),[uint(18446744073709551615),0])\n",
          "fn main():\n  dimensions=[2]\n  reshape(tensor([1,2]),dimensions)\n",
          "fn main():\n  reshape(tensor([1,2]))\n",
          "fn change(values:Tensor[int,?])->Tensor[float32,?]:\n  reshape(values,[2])\n",
          "fn change(values:Tensor[int,?])->[int]:\n  reshape(values,[2])\n",
          "fn change(values:Tensor[int,?])->Tensor[int,2,2]:\n  reshape(values,[1,4])\n",
          "fn main() ! TaskError:\n  try parallel:\n    work=spawn tensor([1,2])\n    "
          "reshape(work,[2])\n"})
      require(!analyze(source).ok(), std::string("invalid tensor reshape admitted: ") + source);
    auto resident = check(parse(std::make_shared<SourceFile>(
                              "resident.k",
                              "fn main()->Tensor[float32,4] ! TaskError:\n  try parallel:\n    "
                              "values:Tensor[float32,2,2]=tensor([[1,2],[3,4]])\n    resident=try "
                              "await transfer(values,devices.gpu())\n    reshape(resident,[4])\n")),
                          {}, {.placement = true});
    bool diagnostic = false;
    for (auto &d : resident.diagnostics)
      diagnostic |= d.code == "S026";
    require(!resident.ok() && diagnostic, "resident tensor bypassed CPU reshape gate");
    std::cout << "tensor reshape: six numeric types, four dimension types, ranks "
                 "zero/one/two/three/eight, fixed extents/counts, closures, direct workers, "
                 "malformed IR and residency gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
