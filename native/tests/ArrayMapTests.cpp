#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("map.k", text)));
}
int main() {
  try {
    for (auto source : {"int32", "uint32", "int64", "uint64", "float32", "float64"})
      for (auto destination : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
        auto typed = analyze("fn convert(value:" + std::string(source) + ")->" + destination +
                             ":\n  " + destination + "(value)\nfn mapped(input:[" + source +
                             "]):\n  input -> map(convert)\nfn main()->int64:\n  0\n");
        require(typed.ok(), "map type-pair rejected");
        auto &fn = typed.module->functions[1];
        const auto &node = typed.module->expressions[fn.result];
        require(node.type.array && node.type.element == resolveNumeric(destination) &&
                    std::holds_alternative<ArrayMap>(node.operation),
                "map result lost array/element identity");
        require(node.type.shape[0].identity ==
                    typed.module->symbols[fn.parameters[0]].type.shape[0].identity,
                "map changed length identity");
        require(bool(continuation::plan(*typed.module)), "map callback import/planning failed");
#ifndef KALANDRA_MAP_FRONTEND_ONLY
        require(bool(lowerNative(*typed.module)), "map pair native lowering failed");
#endif
      }
    for (auto body : {"map([1,2,3],twice)", "[1,2,3] -> map(twice)", "map([],twice)"}) {
      auto typed = analyze(std::string("fn twice(value:int)->int:\n  value*2\nfn mapped():\n  ") +
                           body + "\nfn main()->int:\n  0\n");
      require(typed.ok(), "map call/pipeline/contextual empty rejected");
      require(dumpTypedIR(*typed.module).find("array.map sequential") != std::string::npos,
              "map missing from typed IR");
    }
    for (auto source : {"int32", "uint32", "int64", "uint64", "float32", "float64"})
      for (auto destination : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
        auto typed = analyze("fn mapped(input:[" + std::string(source) +
                             "]):\n  "
                             "input -> map(fn(value): " +
                             destination +
                             "(value))\n"
                             "fn main()->int:\n  0\n");
        require(typed.ok(), "anonymous map numeric pair rejected");
        require(bool(continuation::plan(*typed.module)), "anonymous map planning failed");
#ifndef KALANDRA_MAP_FRONTEND_ONLY
        require(bool(lowerNative(*typed.module)), "anonymous map lowering failed");
#endif
      }
    for (auto text :
         {"fn main():\n  map([],fn(value:int)->float: float(value))\n",
          "fn main():\n  map([1,2],fn(value): value*2)\n",
          "fn main():\n  offset=10\n  map([1,2],fn(value): value+offset)\n",
          "fn main():\n  value=true\n  map([1,2],fn(value): value*2)\n",
          "fn choose(value:int,flag:Bool)->int:\n  if flag: value\n  else: -value\n"
          "fn main():\n  flag=true\n  map([1,2],fn(value): choose(value,flag))\n",
          "fn main():\n  bias:Tensor[int,2]=tensor([3,4])\n  map([1,2],fn(value): "
          "value+reduceSum(bias))\n",
          "fn keep(values:[int],value:int)->int:\n  value\n"
          "fn main():\n  offset=10\n  extra=[1,2]\n  map([3,4],fn(x): keep(map(extra,fn(y): "
          "y+x+offset),x))\n",
          "fn identity(value:int)->int:\n  value\nfn main() ! TaskError:\n  try parallel:\n"
          "    pending=spawn identity(1)\n    result=map([1,2],fn(value): value*2)\n"
          "    try await pending\n"}) {
      auto fixture = std::string(text);
      fixture.replace(fixture.find("fn main()"), 9, "fn example()");
      fixture += "fn main()->int:\n  0\n";
      auto typed = analyze(fixture);
      require(typed.ok(), std::string("valid closure rejected: ") + text);
      require(bool(continuation::plan(*typed.module)), "closure captures/helper import failed");
#ifndef KALANDRA_MAP_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module, "main");
      require(bool(lowered), std::string("closure capture native lowering failed: ") + text +
                                 (lowered ? "" : lowered.error()));
#endif
    }
    for (auto body :
         {"map([1],fn(): 1)", "map([1],fn(x,y): x+y)", "map([],fn(x): x)", "map([1],fn(x:Bool): 1)",
          "map([1],fn(x): [x])", "map([1],fn(x): missing+x)", "callback=fn(x): x\n  0",
          "map([1],fn(x): try x)", "map([1],fn(x): spawn identity(x))"})
      require(
          !analyze("fn identity(x:int)->int:\n  x\nfn main():\n  " + std::string(body) + "\n").ok(),
          "invalid anonymous callback accepted");
    require(!analyze("fn identity(x:int)->int:\n  x\nfn main() ! TaskError:\n  try parallel:\n"
                     "    pending=spawn identity(1)\n    map([1],fn(x): pending)\n")
                 .ok(),
            "pending task captured by closure");
    require(!analyze("fn identity(x:int)->int:\n  x\nfn helper(x:int)->int ! TaskError:\n"
                     "  try parallel:\n    try await spawn identity(x)\n"
                     "fn main():\n  map([1],fn(x): helper(x) catch fn(error): x)\n")
                 .ok(),
            "transitively resumable anonymous callback admitted");
    require(!analyze("fn identity(x:int)->int:\n  x\nfn helper(x:int)->int ! TaskError:\n"
                     "  try parallel:\n    try await spawn identity(x)\n"
                     "fn main():\n  pending=helper(1)\n  map([1],fn(x): pending)\n")
                 .ok(),
            "unhandled fallible value captured");
    auto repeated = analyze("fn mapped(values:[int],offset:int):\n"
                            "  map(values,fn(x): x+offset+offset)\nfn main()->int:\n  0\n");
    require(repeated.ok(), "repeated capture rejected");
    require(std::get<ArrayMap>(
                repeated.module->expressions[repeated.module->functions[0].result].operation)
                    .captures.size() == 1,
            "one lexical binding captured multiple times");
    auto captured = analyze("fn mapped(input:[int],offset:int):\n  map(input,fn(x): x+offset)\n"
                            "fn main()->int:\n  0\n");
    require(captured.ok(), "capture fixture failed");
    auto &captureMap = std::get<ArrayMap>(
        captured.module->expressions[captured.module->functions[0].result].operation);
    require(captureMap.captures.size() == 1 &&
                captured.module->functions[captureMap.function].parameters.size() == 2,
            "capture not lifted exactly once");
#ifndef KALANDRA_MAP_FRONTEND_ONLY
    for (unsigned surface = 0; surface < 4; ++surface) {
      auto forged = *captured.module;
      auto &map = std::get<ArrayMap>(forged.expressions[forged.functions[0].result].operation);
      if (surface == 0)
        map.captures[0] = SIZE_MAX;
      if (surface == 1)
        map.captures.clear();
      if (surface == 2)
        forged.expressions[map.captures[0]].type.element = NumericType::Float64;
      if (surface == 3)
        forged.functions[map.function].parameters.pop_back();
      require(!lowerNative(forged), "forged capture metadata admitted");
    }
#endif
    const std::string prelude = "fn twice(value:int)->int:\n  value*2\n";
    for (auto body : {"map([1,2])", "map([1,2],twice,twice)", "map(tensor([1,2]),twice)",
                      "map([1.0,2.0],twice)", "map([1,2],int64)", "map([1,2],missing)",
                      "callback=1\n  map([1,2],callback)", "map([1,2],twice(2))"})
      require(!analyze(prelude + "fn main():\n  " + body + "\n").ok(), "invalid map accepted");
    for (auto callback :
         {"fn transform()->int:\n  1\n", "fn transform(a:int,b:int)->int:\n  a+b\n",
          "fn transform(a:[int])->int:\n  1\n", "fn transform(a:int)->[int]:\n  [a]\n",
          "fn transform(a:Bool)->Bool:\n  a\n", "fn transform(a:int)->int ! TaskError:\n  a\n"})
      require(!analyze(std::string(callback) + "fn mapped():\n  map([1,2],transform)\n").ok(),
              "unsupported callback signature accepted");
    auto effect = analyze("fn identity(value:int)->int:\n  value\nfn helper(value:int)->int ! "
                          "TaskError:\n  try parallel:\n    try await spawn identity(value)\nfn "
                          "transform(value:int)->int:\n  attempted=helper(value)\n  attempted "
                          "catch fn(error): value\nfn mapped():\n  map([1,2],transform)\n");
    require(!effect.ok(), "transitively resumable callback admitted");
    auto helpers =
        analyze("fn multiply(value:int)->int:\n  value*2\nfn transform(value:int)->int:\n  if "
                "value<0:\n    -value\n  else:\n    multiply(value)\nfn run(values:[int]):\n  "
                "map(values,transform)\nfn main()->int ! TaskError:\n  try parallel:\n    "
                "work=spawn run([1,2,3])\n    result=try await work\n    7\n");
    require(helpers.ok(), "callback helper graph rejected");
    auto plan = continuation::plan(*helpers.module);
    require(bool(plan), "callback helper graph import failed");
#ifndef KALANDRA_MAP_FRONTEND_ONLY
    require(bool(lowerNative(*helpers.module)), "worker callback helper graph lowering failed");
    auto basic =
        analyze(prelude + "fn mapped(input:[int]):\n  map(input,twice)\nfn main()->int:\n  0\n");
    for (unsigned surface = 0; surface < 5; ++surface) {
      auto forged = *basic.module;
      auto &node = forged.expressions[forged.functions[1].result];
      auto &map = std::get<ArrayMap>(node.operation);
      if (surface == 0)
        map.function = SIZE_MAX;
      if (surface == 1)
        map.input = SIZE_MAX;
      if (surface == 2)
        node.type.array = false;
      if (surface == 3)
        node.type.shape[0].identity++;
      if (surface == 4)
        node.type.element = NumericType::Float64;
      require(!lowerNative(forged), "forged map metadata admitted");
    }
#endif
    std::cout << "sequential map: 36 named and anonymous numeric type pairs, lexical captures, "
                 "contexts, nested helper import, worker planning and rejection gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
