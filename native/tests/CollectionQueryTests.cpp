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
  return check(parse(std::make_shared<SourceFile>("query.k", text)));
}
int main() {
  try {
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"})
      for (unsigned rank : {0u, 1u, 2u, 3u, 8u, 99u}) {
        const bool length = rank == 99;
        std::string input = length ? "[" + std::string(type) + "]" : "Tensor[" + std::string(type);
        if (!length) {
          for (unsigned i = 0; i < rank; ++i)
            input += ",?";
          input += "]";
        }
        const std::string name = length ? "length" : "shape";
        auto typed = analyze("fn query(input:" + input + "):\n  " + name +
                             "(input)\nfn main()->int:\n  0\n");
        require(typed.ok(), "valid query rejected");
        const auto id = typed.module->functions[0].result;
        const auto &node = typed.module->expressions[id];
        require(node.type.element == NumericType::Int64 && node.type.array == !length &&
                    std::holds_alternative<CollectionQuery>(node.operation),
                "query result identity lost");
        require(dumpTypedIR(*typed.module)
                        .find(length ? "array.length checked" : "tensor.shape checked") !=
                    std::string::npos,
                "query missing in IR dump");
        require(bool(continuation::plan(*typed.module)), "query planning failed");
        require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                "query silently enabled in GPU kernel");
#ifndef KALANDRA_QUERY_FRONTEND_ONLY
        auto lowered = lowerNative(*typed.module);
        require(bool(lowered), lowered ? "" : lowered.error());
        for (unsigned surface = 0; surface < 10; ++surface) {
          auto forged = *typed.module;
          auto &result = forged.expressions[id];
          auto &query = std::get<CollectionQuery>(result.operation);
          if (surface == 0)
            query.input = SIZE_MAX;
          if (surface == 1)
            query.kind = static_cast<CollectionQueryKind>(99);
          if (surface == 2)
            result.type.element = NumericType::UInt64;
          if (surface == 3)
            result.type.array = length;
          if (surface == 4)
            result.type.tensor = length;
          if (surface == 5)
            forged.expressions[query.input].type.array = !length;
          if (surface == 6)
            forged.expressions[query.input].type.device = 0;
          if (surface == 7)
            result.type.fallible = true;
          if (surface == 8)
            result.type.shape = {{999, 0}};
          if (surface == 9)
            forged.expressions[query.input].type.task = true;
          require(!lowerNative(forged), "malformed query IR accepted");
        }
#endif
      }
    for (auto source :
         {"fn main():\n  values:[float32]=[]\n  length(values)\n",
          "fn main():\n  length(shape(tensor(7)))\n",
          "fn main():\n  shape(tensor([[1,2],[3,4]]))[1]\n", "fn main():\n  [1,2,3] -> length()\n",
          "fn make()->Tensor[int,2] ! TaskError:\n  tensor([1,2])\nfn main() ! TaskError:\n  "
          "length(shape(try make()))\n",
          "fn main():\n  tensor([[1,2],[3,4]]) -> shape() -> length()\n",
          "fn query(values:[float32]):\n  map([1,2],fn(x):x+length(values))\nfn main()->int:\n  "
          "0\n",
          "fn query(values:Tensor[float64,?,?]):\n  map([1,2],fn(x):x+length(shape(values)))\nfn "
          "main()->int:\n  0\n",
          "fn main() ! TaskError:\n  try parallel:\n    task=spawn shape(tensor([[1,2],[3,4]]))\n  "
          "  dimensions=try await task\n    length(dimensions)\n",
          "fn main() ! TaskError:\n  try parallel:\n    task=spawn length([1,2,3])\n    try await "
          "task\n",
          "fn length(value:int):\n  value+1\nfn main():\n  length(1)\n",
          "fn shape(value:int):\n  value+2\nfn main():\n  shape(1)\n"}) {
      auto typed = analyze(source);
      require(typed.ok(), std::string("valid query program rejected: ") + source);
      require(bool(continuation::plan(*typed.module)), "query helper import failed");
#ifndef KALANDRA_QUERY_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto source :
         {"fn main():\n  length(1)\n", "fn main():\n  shape(1)\n", "fn main():\n  shape([1,2])\n",
          "fn make()->Tensor[int,2] ! TaskError:\n  tensor([1,2])\nfn main() ! TaskError:\n  "
          "length(shape(make()))\n",
          "fn main():\n  length(tensor([1,2]))\n", "fn main():\n  length([])\n",
          "fn main():\n  shape()\n", "fn main():\n  shape(tensor(7),0)\n",
          "fn main():\n  length([1,2],0)\n", "fn main()->uint:\n  length([1,2])\n",
          "fn main()->[uint]:\n  shape(tensor(7))\n",
          "fn main():\n  shape(tensor([[1,2],[3,4]]))[2]\n",
          "fn main():\n  length=1\n  length([1,2])\n",
          "fn made()->[int]:\n  [1,2]\nfn main() ! TaskError:\n  try parallel:\n    pending=spawn "
          "made()\n    length(pending)\n"})
      require(!analyze(source).ok(), std::string("invalid query accepted: ") + source);
    auto resident = check(parse(std::make_shared<SourceFile>(
                              "resident.k",
                              "fn main()->[int] ! TaskError:\n  try parallel:\n    "
                              "values:Tensor[float32,2,2]=tensor([[1,2],[3,4]])\n    resident=try "
                              "await transfer(values,devices.gpu())\n    shape(resident)\n")),
                          {}, {.placement = true});
    bool diagnostic = false;
    for (auto &d : resident.diagnostics)
      diagnostic |= d.code == "S064";
    require(!resident.ok() && diagnostic, "resident tensor bypassed CPU query gate");
    std::cout
        << "collection queries: six numeric types, fixed/dynamic/empty/rank-zero metadata, int64 "
           "result identities, captures, direct tasks, malformed IR and residency gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
