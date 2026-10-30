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
  return check(parse(std::make_shared<SourceFile>("bool-arrays.k", text)));
}
int main() {
  try {
    for (const auto *body :
         {"fn main():\n  [true,false]\n", "fn main()->[Bool]:\n  []\n",
          "fn main():\n  map([1,2],fn(x):x>1)\n",
          "fn flip(x:Bool)->Bool:\n  not x\nfn main():\n  map([],flip)\n",
          "fn main():\n  map([],fn(x:Bool)->Bool:not x)\n",
          "fn encode(flag:Bool)->int:\n  if flag:1\n  else:0\nfn main():\n  "
          "map([true,false],encode)\n",
          "type Flag:\n  enabled:Bool\nfn main():\n  rows=map([true,false],fn(x):Flag "
          "{enabled:x})\n  map(rows,fn(row):row.enabled)\n",
          "fn main()->[Bool] ! TaskError:\n  flags=[true,false]\n  try parallel:\n    try await "
          "spawn map(flags,fn(x):not x)\n",
          "fn encode(flag:Bool)->int:\n  if flag:1\n  else:0\nfn main()->[int] ! TaskError:\n  "
          "flags=[true,false]\n  try parallel:\n    try await spawn "
          "map([0,1],fn(i):encode(flags[i]))\n"}) {
      auto checked = analyze(body);
      require(checked.ok(), "valid Bool array rejected: " + std::string(body));
      require(!numericCapabilityError(*checked.module, NumericTarget::Cpu),
              "Bool array capability rejected");
      require(bool(continuation::plan(*checked.module)), "Bool array plan rejected");
#ifndef KALANDRA_BOOL_ARRAY_FRONTEND_ONLY
      auto lowered = lowerNative(*checked.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *body :
         {"fn main():\n  [true,1]\n", "fn main()->[Bool]:\n  [1]\n", "fn main():\n  [[true]]\n",
          "fn main():\n  [true,false]+[true,false]\n", "fn main():\n  not [true]\n",
          "fn main():\n  [true]==[false]\n", "fn main():\n  [true][true]\n",
          "fn main():\n  [true][1]\n", "fn main():\n  tensor([true,false])\n",
          "fn main()->Tensor[Bool,2]:\n  tensor([true,false])\n",
          "fn main():\n  flags=[true,false]\n  shape(flags)\n",
          "fn main():\n  flags=[true,false]\n  int(flags)\n",
          "fn main():\n  flags=[true,false]\n  reduceSum(flags)\n",
          "fn main():\n  map([true],fn(x:int):x)\n", "fn main():\n  map([],fn(x):true)\n"})
      require(!analyze(body).ok(), "invalid Bool array admitted: " + std::string(body));
    auto valid = analyze("fn flags(values:[int])->[Bool]:\n  map(values,fn(x):x>0)\nfn "
                         "main()->[Bool]:\n  flags([1,2])\n");
    require(valid.ok() && describeType(valid.module->functions[0].resultType) == "[Bool]",
            "Bool array type name lost");
    auto mapId = valid.module->functions[0].result;
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
      auto forged = *valid.module;
      auto &e = forged.expressions[mapId];
      auto &map = std::get<ArrayMap>(e.operation);
      if (mutation == 0)
        e.type.array = false;
      if (mutation == 1)
        e.type.tensor = false;
      if (mutation == 2)
        e.type.device = 0;
      if (mutation == 3)
        e.type.shape.clear();
      if (mutation == 4)
        e.type.builtin = BuiltinValue::Numeric;
      if (mutation == 5)
        forged.functions[map.function].resultType.tensor = true;
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
              "forged Bool map admitted");
    }
    auto boolean = analyze("fn main()->Bool:\n  true\n");
    require(boolean.ok(), "Boolean literal fixture rejected");
    auto forged = *boolean.module;
    auto &literal = forged.expressions[forged.functions[0].result].type;
    literal.array = literal.tensor = true;
    literal.shape = {{1, 0}};
    forged.functions[0].resultType = literal;
    forged.functions[0].declaredResult = literal;
    require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
            "Boolean scalar literal forged into an array");
    std::cout << "Bool arrays: literals, map categories, captures, workers, nominal guards and "
                 "malformed IR passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
