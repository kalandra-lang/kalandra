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
    for (auto type : {"i32", "u32", "i64", "u64", "f32", "f64"}) {
      for (auto index : {"i32", "u32", "i64", "u64"}) {
        auto typed = analyze("fn get(values:[" + std::string(type) + "],index:" + index + ")->" +
                             type + ":\n  values[index]\nfn main()->i64:\n  0\n");
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
         {"fn main()->f32:\n  return [1,2][1]\n", "fn main():\n  return [10,20,30][1+1]*2+1\n",
          "fn make()->[i64]:\n  return [10,20]\nfn main():\n  return make()[1]\n",
          "fn get(values:[i64],index:i64):\n  return values[index]\nfn main():\n  return get([1,2],1)\n",
          "fn run(values:[i64],index:u32):\n  return map([1,2],fn(x): return values[index]+x)\nfn "
          "main()->i64:\n  return 0\n",
          "fn run(flag:bool,index:i64):\n  values=if flag: [1]\n  else: [2,3]\n  "
          "return values[index]\nfn main()->i64:\n  return 0\n",
          "fn get(values:[i64],index:i64):\n  return values[index]\nfn main() ! TaskError:\n  return "
          "try parallel:\n    work=spawn get([1,2],1)\n    try await work\n",
          "fn make()->[i64]:\n  return [10,20]\nfn main() ! TaskError:\n  return try parallel:\n    "
          "work=spawn make()\n    values=try await work\n    values[1]\n",
          "fn main()->i64:\n  values=[1,2]\n  values[0]=3\n  return values[0]\n",
          "fn main()->bool:\n  values=[false,true]\n  values[0]=true\n  return values[0]\n"}) {
      auto typed = analyze(source);
      require(typed.ok(), std::string("valid index program rejected: ") + source);
      require(bool(continuation::plan(*typed.module)), "index graph import failed");
#ifndef KALANDRA_ARRAY_INDEX_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto source :
         {"fn main():\n  return [1,2][-1]\n", "fn main():\n  return [1,2][2]\n", "fn main():\n  return [1,2][1.0]\n",
          "fn main():\n  return [1,2][true]\n", "fn main():\n  return [1,2][[1]]\n", "fn main():\n  return 1[0]\n",
          "fn main():\n  return tensor([1,2])[0,0]\n", "fn main():\n  return [1,2][u64(18446744073709551615)]\n",
          "fn main():\n  empty:[i64]=[]\n  return empty[0]\n",
          "fn run(index:i64):\n  empty:[i64]=[]\n  return empty[index]\n",
          "fn write(values:[i64])->i64:\n  values[0]=3\n  return values[0]\nfn main()->i64:\n  return "
          "write([1,2])\n",
          "fn main():\n  values=[1,2]\n  values[-1]=3\n  return 0\n",
          "fn main():\n  values=[1,2]\n  values[2]=3\n  return 0\n",
          "fn main():\n  values=[1,2]\n  values[0]=true\n  return 0\n", "fn main():\n  return [1,2][]\n",
          "fn main():\n  return [1,2][0,1]\n", "fn main():\n  [1,2][0\n", "fn main():\n  return [1,2][0][0]\n",
          "fn make()->[i64]:\n  return [1,2]\nfn main() ! TaskError:\n  return try parallel:\n    work=spawn "
          "make()\n    work[0]\n"})
      require(!analyze(source).ok(), std::string("invalid index accepted: ") + source);
    std::cout << "checked array indexing: 24 numeric/index pairs, precedence, read-only "
                 "boundaries, captures, continuations and malformed IR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
