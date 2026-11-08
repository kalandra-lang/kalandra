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
         {"fn main():\n  return [true,false]\n", "fn main()->[bool]:\n  return []\n",
          "fn main():\n  return map([1,2],fn(x):return x>1)\n",
          "fn flip(x:bool)->bool:\n  return not x\nfn main():\n  return map([],flip)\n",
          "fn main():\n  return map([],fn(x:bool)->bool:return not x)\n",
          "fn encode(flag:bool)->i64:\n  return if flag:1\n  else:0\nfn main():\n  "
          "return map([true,false],encode)\n",
          "type Flag:\n  enabled:bool\nfn main():\n  rows=map([true,false],fn(x):return Flag "
          "{enabled:x})\n  return map(rows,fn(row):return row.enabled)\n",
          "fn main()->[bool] ! TaskError:\n  flags=[true,false]\n  return try parallel:\n    try await "
          "spawn map(flags,fn(x):return not x)\n",
          "fn encode(flag:bool)->i64:\n  return if flag:1\n  else:0\nfn main()->[i64] ! TaskError:\n  "
          "flags=[true,false]\n  return try parallel:\n    try await spawn "
          "map([0,1],fn(i):return encode(flags[i]))\n"}) {
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
         {"fn main():\n  return [true,1]\n", "fn main()->[bool]:\n  return [1]\n", "fn main():\n  return [[true]]\n",
          "fn main():\n  return [true,false]+[true,false]\n", "fn main():\n  return not [true]\n",
          "fn main():\n  return [true]==[false]\n", "fn main():\n  return [true][true]\n",
          "fn main():\n  return [true][1]\n", "fn main():\n  return tensor([true,false])\n",
          "fn main()->Tensor[bool,2]:\n  return tensor([true,false])\n",
          "fn main():\n  flags=[true,false]\n  return shape(flags)\n",
          "fn main():\n  flags=[true,false]\n  return i64(flags)\n",
          "fn main():\n  flags=[true,false]\n  return reduceSum(flags)\n",
          "fn main():\n  return map([true],fn(x:i64):return x)\n", "fn main():\n  return map([],fn(x):return true)\n"})
      require(!analyze(body).ok(), "invalid Bool array admitted: " + std::string(body));
    auto valid = analyze("fn flags(values:[i64])->[bool]:\n  return map(values,fn(x):return x>0)\nfn "
                         "main()->[bool]:\n  return flags([1,2])\n");
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
    auto boolean = analyze("fn main()->bool:\n  return true\n");
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
