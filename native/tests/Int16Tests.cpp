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
  return check(parse(std::make_shared<SourceFile>("int16.k", text)));
}
int main() {
  try {
    for (const auto *source :
         {"fn main()->int16:\n  -32768\n", "fn main()->uint16:\n  65535\n",
          "fn main()->int16:\n  x:int16=120\n  x+7\n",
          "fn f(x:int16,y:int16)->int16:\n  x+y\nfn main()->int16:\n  f(20,22)\n",
          "fn main()->uint16:\n  uint16(65535)\n",
          "fn f(x:int16)->int:\n  int(x)\nfn main()->int:\n  f(-32768)\n",
          "fn f(x:float)->int16:\n  int16(x)\nfn main()->int16:\n  f(-12.9)\n",
          "fn main()->int16 ! TaskError:\n  try parallel:\n    x:int16=-32768\n    try await spawn "
          "int16(int(x))\n",
          "fn f(x:uint16)->uint16:\n  if x>10:x-1\n  else:x+1\nfn main()->uint16 ! TaskError:\n  "
          "try "
          "parallel:\n    work=spawn f(65535)\n    a=try await work\n    b=try await work\n    b\n",
          "fn get(i:uint16)->String:\n  [\"a\",\"é\"][i]\nfn main()->String:\n  get(1)\n",
          "fn get(i:int16)->String:\n  slice(\"aé\",i,2)\nfn main()->String:\n  get(1)\n"}) {
      auto result = analyze(source);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), std::string("valid int16 program rejected: ") + source);
      require(bool(continuation::plan(*result.module)), "int16 planning failed");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "numeric CPU capability rejected");
#ifndef KALANDRA_INT16_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *value : {"32768", "-32769", "32767+1", "-32768-1", "16384*2", "-(-32768)",
                              "-32768/-1", "1/0", "1.0"})
      require(!analyze(std::string("fn main()->int16:\n  ") + value + "\n").ok(),
              "invalid signed 16-bit accepted");
    for (const auto *value : {"65536", "-1", "65535+1", "0-1", "32768*2", "-uint16(1)", "1%0"})
      require(!analyze(std::string("fn main()->uint16:\n  ") + value + "\n").ok(),
              "invalid unsigned 16-bit accepted");
    for (const auto *source :
         {"fn main()->int16:\n  value=1\n  value\n", "fn main()->int16:\n  int16(32768)\n",
          "fn main()->uint16:\n  uint16(-1)\n",
          "fn bad(a:int16,b:uint16):\n  a+b\nfn main()->int:\n  0\n", "fn main()->int128:\n  1\n",
          "fn main()->Tensor[float16,1]:\n  tensor([1])\n", "fn main():\n  uint16([1])\n",
          "type Row:\n  value:Tensor[float16,1]\nfn main()->int:\n  0\n",
          "fn main():\n  int64(map([1],fn(x):int16(x)))\n"})
      require(!analyze(source).ok(), std::string("unsupported int16 surface accepted: ") + source);
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto forged = analyze("fn main()->int16:\n  1\n");
      auto &node = forged.module->expressions[forged.module->functions[0].result];
      if (mode == 0)
        node.operation = Int32Literal{32768};
      if (mode == 1)
        node.operation = UInt32Literal{1};
      if (mode == 2)
        node.operation = Int64Literal{1};
      if (mode == 3)
        node.type.tensor = node.type.array = true;
      require(bool(numericCapabilityError(*forged.module, NumericTarget::Cpu)),
              "malformed 16-bit literal admitted");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto forged = analyze("fn main()->uint16:\n  1\n");
      auto &node = forged.module->expressions[forged.module->functions[0].result];
      if (mode == 0)
        node.operation = UInt32Literal{65536};
      if (mode == 1)
        node.operation = Int32Literal{1};
      if (mode == 2)
        node.operation = UInt64Literal{1};
      if (mode == 3)
        node.type.tensor = node.type.array = true;
      require(bool(numericCapabilityError(*forged.module, NumericTarget::Cpu)),
              "malformed unsigned 16-bit literal admitted");
    }
    std::cout
        << "int16/uint16: exact widths, checked literals/arithmetic/conversions, worker plans "
           "and aggregate/GPU gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
