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
  return check(parse(std::make_shared<SourceFile>("tensor-index.k", text)));
}
int main() {
  try {
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      for (unsigned rank : {0u, 1u, 2u, 3u, 8u}) {
        for (auto indexType : {"int32", "uint32", "int64", "uint64"}) {
          std::string parameters = "values:Tensor[" + std::string(type);
          for (unsigned axis = 0; axis < rank; ++axis)
            parameters += ",?";
          parameters += "]";
          std::string indices;
          for (unsigned axis = 0; axis < rank; ++axis) {
            auto name = "i" + std::to_string(axis);
            parameters += "," + name + ":" + indexType;
            if (axis)
              indices += ",";
            indices += name;
          }
          auto typed = analyze("fn read(" + parameters + ")->" + type + ":\n  values[" + indices +
                               "]\nfn main()->int:\n  0\n");
          require(typed.ok(), "valid tensor read rejected");
          auto id = typed.module->functions[0].result;
          require(std::holds_alternative<TensorIndex>(typed.module->expressions[id].operation),
                  "tensor index operation lost");
          require(dumpTypedIR(*typed.module).find("tensor.index checked") != std::string::npos,
                  "tensor read not visible in IR");
          require(bool(continuation::plan(*typed.module)), "tensor read planning failed");
          require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                  "tensor read silently enabled on GPU");
#ifndef KALANDRA_TENSOR_INDEX_FRONTEND_ONLY
          auto lowered = lowerNative(*typed.module);
          require(bool(lowered), lowered ? "" : lowered.error());
          for (unsigned surface = 0; surface < 10; ++surface) {
            auto forged = *typed.module;
            auto &node = forged.expressions[id];
            auto &read = std::get<TensorIndex>(node.operation);
            if (surface == 0)
              read.input = SIZE_MAX;
            if (surface == 1)
              read.indices.push_back(SIZE_MAX);
            if (surface == 2)
              node.type.tensor = true;
            if (surface == 3)
              node.type.element = node.type.element == NumericType::Float64 ? NumericType::Int32
                                                                            : NumericType::Float64;
            if (surface == 4)
              forged.expressions[read.input].type.array = true;
            if (surface == 5)
              forged.expressions[read.input].type.task = true;
            if (surface == 6)
              forged.expressions[read.input].type.device = 0;
            if (surface == 7)
              node.type.fallible = true;
            if (surface == 8) {
              if (rank)
                read.indices[0] = SIZE_MAX;
              else
                forged.expressions[read.input].type.shape.push_back({1, 0});
            }
            if (surface == 9) {
              if (rank)
                forged.expressions[read.indices[0]].type.element = NumericType::Float32;
              else
                node.type.builtin = BuiltinValue::Bool;
            }
            require(!lowerNative(forged), "malformed tensor indexing admitted");
          }
#endif
        }
      }
    }
    for (auto source :
         {"fn main():\n  tensor([[1,2],[3,4]])[1,0]*2+1\n", "fn main():\n  tensor(7)[]\n",
          "fn row()->int:\n  1\nfn column()->uint32:\n  0\nfn main() ! TaskError:\n  "
          "values=tensor([[1,2],[3,4]])\n  try parallel:\n    a=spawn row()\n    b=spawn "
          "column()\n    values[try await a,try await b]\n",
          "fn main():\n  tensor([1,2])[1,]\n",
          "fn read(values:Tensor[int,?,?],row:int,column:uint32):\n  map([1,2],fn(x): "
          "values[row,column]+x)\nfn main()->int:\n  0\n",
          "fn read(values:Tensor[int,?,?],row:int,column:uint32):\n  values[row,column]\nfn main() "
          "! TaskError:\n  try parallel:\n    work=spawn read(tensor([[1,2],[3,4]]),1,0)\n    try "
          "await work\n",
          "fn made()->Tensor[int,2,2]:\n  tensor([[1,2],[3,4]])\nfn main() ! TaskError:\n  try "
          "parallel:\n    work=spawn made()\n    values=try await work\n    values[1,1]\n",
          "fn read(flag:Bool,row:int,column:int):\n  values=if flag: tensor([[1,2]])\n  else: "
          "tensor([[3,4],[5,6]])\n  values[row,column]\nfn main()->int:\n  0\n"}) {
      auto typed = analyze(source);
      require(typed.ok(), std::string("valid tensor program rejected: ") + source);
      require(bool(continuation::plan(*typed.module)), "tensor read import failed");
#ifndef KALANDRA_TENSOR_INDEX_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto source :
         {"fn main():\n  tensor([[1,2],[3,4]])[0]\n",
          "fn main():\n  tensor([[1,2],[3,4]])[0,0,0]\n", "fn main():\n  tensor([1,2])[]\n",
          "fn main():\n  tensor(7)[0]\n", "fn main():\n  tensor([[1,2],[3,4]])[2,0]\n",
          "fn main():\n  tensor([[1,2],[3,4]])[0,-1]\n",
          "fn main():\n  tensor([[1,2],[3,4]])[0,2]\n",
          "fn main():\n  tensor([[1,2],[3,4]])[0,uint(18446744073709551615)]\n",
          "fn main():\n  tensor([[1,2],[3,4]])[0,true]\n",
          "fn main():\n  tensor([[1,2],[3,4]])[0,1.0]\n",
          "fn read(values:Tensor[int,0,2],row:int,column:int):\n  values[row,column]\n",
          "fn main():\n  values=tensor([[1,2],[3,4]])\n  values[0,0]=1\n  0\n",
          "fn main() ! TaskError:\n  try parallel:\n    work=spawn tensor([[1,2],[3,4]])\n    "
          "work[0,0]\n",
          "fn main() ! TaskError:\n  try parallel:\n    device=devices.gpu()\n    "
          "work=transfer(tensor([[1,2],[3,4]]),device)\n    values=try await work\n    "
          "values[0,0]\n"})
      require(!analyze(source).ok(), std::string("invalid tensor read accepted: ") + source);
    const std::string placementPrefix = "fn main()->float32 ! TaskError:\n  try parallel:\n"
                                        "    values:Tensor[float32,2,2]=tensor([[1,2],[3,4]])\n"
                                        "    resident=try await transfer(values,devices.gpu())\n";
    auto resident = check(parse(std::make_shared<SourceFile>(
                              "resident.k", placementPrefix + "    resident[0,0]\n")),
                          {}, {.placement = true});
    require(!resident.ok(), "host indexing silently downloaded a device tensor");
    bool residencyDiagnostic = false;
    for (const auto &diagnostic : resident.diagnostics)
      residencyDiagnostic |= diagnostic.code == "S061";
    require(residencyDiagnostic, "resident lookup did not reach the indexing gate");
    auto transferred =
        check(parse(std::make_shared<SourceFile>(
                  "transferred.k",
                  placementPrefix +
                      "    host=try await transfer(resident,devices.cpu())\n    host[0,0]\n")),
              {}, {.placement = true});
    require(transferred.ok(), "explicit CPU transfer did not enable host read");
    std::cout
        << "tensor indexing: six numeric types, four index types, ranks zero/one/two/three/eight, "
           "rank/bounds/type/residency gates, closures, worker planning and malformed IR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
