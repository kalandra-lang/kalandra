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
  return check(parse(std::make_shared<SourceFile>("slice.k", text)));
}
int main() {
  try {
    const std::string record = "type Row:\n  text:[String]\n  values:[int]\n";
    for (auto element :
         {"int32", "uint32", "int64", "uint64", "float32", "float64", "Bool", "String", "Row"})
      for (auto bound : {"int32", "uint32", "int64", "uint64"}) {
        auto result =
            analyze(record + "fn part(values:[" + element + "],start:" + bound + ",end:" + bound +
                    ")->[" + element + "]:\n  slice(values,start,end)\nfn main()->int:\n  0\n");
        require(result.ok(), "valid array/bound combination rejected");
        const auto id = result.module->functions[0].result;
        require(std::holds_alternative<ArraySlice>(result.module->expressions[id].operation),
                "slice IR missing");
        require(dumpTypedIR(*result.module).find("array.slice checked") != std::string::npos,
                "slice dump missing");
        require(bool(continuation::plan(*result.module)), "slice planning failed");
        require(bool(numericCapabilityError(*result.module, NumericTarget::AppleGpu)),
                "GPU slice enabled");
#ifndef KALANDRA_ARRAY_SLICE_FRONTEND_ONLY
        auto lowered = lowerNative(*result.module);
        require(bool(lowered), lowered ? "" : lowered.error());
#endif
      }
    for (const auto *source :
         {"fn main()->[float32]:\n  slice([],0,0)\n", "fn main()->[float32]:\n  slice([1,2],0,1)\n",
          "fn main():\n  [1,2,3]->slice(1,3)->map(fn(x):x*2)\n",
          "fn main()->[Bool]:\n  slice([true,false],2,2)\n",
          "fn main()->[String]:\n  slice([\"é\",\"👩\"],0,2)\n",
          "fn main()->[int] ! TaskError:\n  try parallel:\n    work=spawn slice([1,2,3],1,3)\n    "
          "a=try await work\n    b=try await work\n    map(a,fn(x):b[0]+x)\n",
          "fn main():\n  slice(\"é👩\",0,1)\n"}) {
      auto result = analyze(source);
      require(result.ok(), std::string("valid slice rejected: ") + source);
      require(bool(continuation::plan(*result.module)), "slice composition failed");
#ifndef KALANDRA_ARRAY_SLICE_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *source :
         {"fn main():\n  slice([1,2],-1,1)\n", "fn main():\n  slice([1,2],0,3)\n",
          "fn main():\n  slice([1,2],2,1)\n", "fn main():\n  slice([1],2,2)\n",
          "fn main():\n  slice([1],0,uint(18446744073709551615))\n",
          "fn main():\n  slice([1],true,1)\n", "fn main():\n  slice([1],0,1.0)\n",
          "fn main():\n  slice(tensor([1]),0,1)\n", "fn main():\n  slice([1],0)\n",
          "fn main():\n  slice([],0,0)\n", "fn main():\n  slice([1],0,0)[0]\n",
          "fn main()->[float32]:\n  values=[1,2]\n  slice(values,0,1)\n",
          "fn main():\n  slice([\"a\"],0,1)->tensor()\n"})
      require(!analyze(source).ok(), std::string("invalid slice accepted: ") + source);
    for (unsigned mode = 0; mode < 12; ++mode) {
      auto result = analyze("fn part(values:[int],start:int,end:int)->[int]:\n  "
                            "slice(values,start,end)\nfn main()->int:\n  0\n");
      auto &node = result.module->expressions[result.module->functions[0].result];
      auto &op = std::get<ArraySlice>(node.operation);
      if (mode == 0)
        op.input = SIZE_MAX;
      if (mode == 1)
        op.start = SIZE_MAX;
      if (mode == 2)
        op.end = SIZE_MAX;
      if (mode == 3)
        node.type.array = false;
      if (mode == 4)
        node.type.element = NumericType::Float64;
      if (mode == 5)
        node.type.shape.clear();
      if (mode == 6)
        node.type.task = true;
      if (mode == 7)
        result.module->expressions[op.input].type.fallible = true;
      if (mode == 8)
        result.module->expressions[op.start].type.element = NumericType::Float64;
      if (mode == 9)
        result.module->expressions[op.end].type.array = true;
      if (mode == 10)
        node.type.device = 0;
      if (mode == 11)
        node.type.builtin = BuiltinValue::String;
      require(bool(numericCapabilityError(*result.module, NumericTarget::Cpu)),
              "malformed slice IR accepted");
#ifndef KALANDRA_ARRAY_SLICE_FRONTEND_ONLY
      require(!lowerNative(*result.module), "malformed slice reached backend");
#endif
    }
    std::cout << "Array slicing: nine element types, four integer bounds, context, planning, "
                 "malformed IR and strict CPU gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
