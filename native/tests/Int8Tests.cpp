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
  return check(parse(std::make_shared<SourceFile>("int8.k", text)));
}
int main() {
  try {
    for (const auto *source :
         {"fn main()->int8:\n  -128\n", "fn main()->uint8:\n  255\n",
          "fn main()->int8:\n  x:int8=120\n  x+7\n",
          "fn f(x:int8,y:int8)->int8:\n  x+y\nfn main()->int8:\n  f(20,22)\n",
          "fn main()->uint8:\n  uint8(255)\n",
          "fn f(x:int8)->int:\n  int(x)\nfn main()->int:\n  f(-128)\n",
          "fn f(x:float)->int8:\n  int8(x)\nfn main()->int8:\n  f(-12.9)\n",
          "fn main()->int8 ! TaskError:\n  try parallel:\n    x:int8=-128\n    try await spawn "
          "int8(int(x))\n",
          "fn f(x:uint8)->uint8:\n  if x>10:x-1\n  else:x+1\nfn main()->uint8 ! TaskError:\n  try "
          "parallel:\n    work=spawn f(255)\n    a=try await work\n    b=try await work\n    b\n",
          "fn get(i:uint8)->String:\n  [\"a\",\"é\"][i]\nfn main()->String:\n  get(1)\n",
          "fn get(i:int8)->String:\n  slice(\"aé\",i,2)\nfn main()->String:\n  get(1)\n"}) {
      auto result = analyze(source);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), std::string("valid int8 program rejected: ") + source);
      require(bool(continuation::plan(*result.module)), "int8 planning failed");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "numeric CPU capability rejected");
#ifndef KALANDRA_INT8_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *value :
         {"128", "-129", "127+1", "-128-1", "64*2", "-(-128)", "-128/-1", "1/0", "1.0"})
      require(!analyze(std::string("fn main()->int8:\n  ") + value + "\n").ok(),
              "invalid signed byte accepted");
    for (const auto *value : {"256", "-1", "255+1", "0-1", "128*2", "-uint8(1)", "1%0"})
      require(!analyze(std::string("fn main()->uint8:\n  ") + value + "\n").ok(),
              "invalid unsigned byte accepted");
    for (const auto *source :
         {"fn main()->int8:\n  value=1\n  value\n", "fn main()->int8:\n  int8(128)\n",
          "fn main()->uint8:\n  uint8(-1)\n",
          "fn bad(a:int8,b:uint8):\n  a+b\nfn main()->int:\n  0\n", "fn main()->int128:\n  1\n",
          "fn main()->Tensor[float16,1]:\n  tensor([1])\n", "fn main():\n  uint8([1])\n",
          "type Row:\n  value:Tensor[float16,1]\nfn main()->int:\n  0\n",
          "fn main():\n  int64(map([1],fn(x):int8(x)))\n"})
      require(!analyze(source).ok(), std::string("unsupported int8 surface accepted: ") + source);
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto forged = analyze("fn main()->int8:\n  1\n");
      auto &node = forged.module->expressions[forged.module->functions[0].result];
      if (mode == 0)
        node.operation = Int32Literal{128};
      if (mode == 1)
        node.operation = UInt32Literal{1};
      if (mode == 2)
        node.operation = Int64Literal{1};
      if (mode == 3)
        node.type.tensor = node.type.array = true;
      require(bool(numericCapabilityError(*forged.module, NumericTarget::Cpu)),
              "malformed byte literal admitted");
    }
    std::cout << "int8/uint8: exact widths, checked literals/arithmetic/conversions, worker plans "
                 "and aggregate/GPU gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
