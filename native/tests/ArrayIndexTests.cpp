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
  return check(parse(std::make_shared<SourceFile>("index.k", text)));
}
int main() {
  try {
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      for (auto index : {"int32", "uint32", "int64", "uint64"}) {
        auto typed = analyze("fn get(values:[" + std::string(type) + "],index:" + index + ")->" +
                             type + ":\n  values[index]\nfn main()->int:\n  0\n");
        require(typed.ok(), "valid numeric/index combination rejected");
        auto id = typed.module->functions[0].result;
        require(std::holds_alternative<ArrayIndex>(typed.module->expressions[id].operation),
                "index not represented in typed IR");
        require(dumpTypedIR(*typed.module).find("array.index checked") != std::string::npos,
                "index not visible in IR dump");
        require(bool(continuation::plan(*typed.module)), "index continuation planning failed");
#ifndef KALANDRA_ARRAY_INDEX_FRONTEND_ONLY
        auto lowered = lowerNative(*typed.module);
        require(bool(lowered), lowered ? "" : lowered.error());
        for (unsigned surface = 0; surface < 8; ++surface) {
          auto forged = *typed.module;
          auto &node = forged.expressions[id];
          auto &read = std::get<ArrayIndex>(node.operation);
          if (surface == 0)
            read.input = SIZE_MAX;
          if (surface == 1)
            read.index = SIZE_MAX;
          if (surface == 2)
            node.type.tensor = true;
          if (surface == 3)
            node.type.element = node.type.element == NumericType::Float64 ? NumericType::Int32
                                                                          : NumericType::Float64;
          if (surface == 4)
            forged.expressions[read.input].type.array = false;
          if (surface == 5)
            forged.expressions[read.index].type.element = NumericType::Float32;
          if (surface == 6)
            forged.expressions[read.input].type.task = true;
          if (surface == 7)
            forged.expressions[read.index].type.fallible = true;
          require(!lowerNative(forged), "malformed indexing IR accepted");
        }
#endif
      }
    }
    for (auto source :
         {"fn main()->float32:\n  [1,2][1]\n", "fn main():\n  [10,20,30][1+1]*2+1\n",
          "fn make()->[int]:\n  [10,20]\nfn main():\n  make()[1]\n",
          "fn get(values:[int],index:int):\n  values[index]\nfn main():\n  get([1,2],1)\n",
          "fn run(values:[int],index:uint32):\n  map([1,2],fn(x): values[index]+x)\nfn "
          "main()->int:\n  0\n",
          "fn run(flag:Bool,index:int):\n  values=if flag: [1]\n  else: [2,3]\n  "
          "values[index]\nfn main()->int:\n  0\n",
          "fn get(values:[int],index:int):\n  values[index]\nfn main() ! TaskError:\n  "
          "try parallel:\n    work=spawn get([1,2],1)\n    try await work\n",
          "fn make()->[int]:\n  [10,20]\nfn main() ! TaskError:\n  try parallel:\n    "
          "work=spawn make()\n    values=try await work\n    values[1]\n"}) {
      auto typed = analyze(source);
      require(typed.ok(), std::string("valid index program rejected: ") + source);
      require(bool(continuation::plan(*typed.module)), "index graph import failed");
#ifndef KALANDRA_ARRAY_INDEX_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto source :
         {"fn main():\n  [1,2][-1]\n", "fn main():\n  [1,2][2]\n", "fn main():\n  [1,2][1.0]\n",
          "fn main():\n  [1,2][true]\n", "fn main():\n  [1,2][[1]]\n", "fn main():\n  1[0]\n",
          "fn main():\n  tensor([1,2])[0,0]\n", "fn main():\n  [1,2][uint(18446744073709551615)]\n",
          "fn main():\n  empty:[int]=[]\n  empty[0]\n",
          "fn run(index:int):\n  empty:[int]=[]\n  empty[index]\n",
          "fn main():\n  values=[1,2]\n  values[0]=3\n  0\n", "fn main():\n  [1,2][]\n",
          "fn main():\n  [1,2][0,1]\n", "fn main():\n  [1,2][0\n", "fn main():\n  [1,2][0][0]\n",
          "fn make()->[int]:\n  [1,2]\nfn main() ! TaskError:\n  try parallel:\n    work=spawn "
          "make()\n    work[0]\n"})
      require(!analyze(source).ok(), std::string("invalid index accepted: ") + source);
    std::cout << "checked array indexing: 24 numeric/index pairs, precedence, immutable "
                 "boundaries, captures, continuations and malformed IR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
