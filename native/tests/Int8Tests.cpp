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
         {"fn main()->i8:\n  return -128\n", "fn main()->u8:\n  return 255\n",
          "fn main()->i8:\n  x:i8=120\n  return x+7\n",
          "fn f(x:i8,y:i8)->i8:\n  return x+y\nfn main()->i8:\n  return f(20,22)\n",
          "fn main()->u8:\n  return u8(255)\n",
          "fn f(x:i8)->i64:\n  return i64(x)\nfn main()->i64:\n  return f(-128)\n",
          "fn f(x:f64)->i8:\n  return i8(x)\nfn main()->i8:\n  return f(-12.9)\n",
          "fn main()->i8 ! TaskError:\n  return try parallel:\n    x:i8=-128\n    try await spawn "
          "i8(i64(x))\n",
          "fn f(x:u8)->u8:\n  return if x>10:x-1\n  else:x+1\nfn main()->u8 ! TaskError:\n  return try "
          "parallel:\n    work=spawn f(255)\n    a=try await work\n    b=try await work\n    b\n",
          "fn get(i:u8)->String:\n  return [\"a\",\"é\"][i]\nfn main()->String:\n  return get(1)\n",
          "fn get(i:i8)->String:\n  return slice(\"aé\",i,2)\nfn main()->String:\n  return get(1)\n"}) {
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
      require(!analyze(std::string("fn main()->i8:\n  ") + value + "\n").ok(),
              "invalid signed byte accepted");
    for (const auto *value : {"256", "-1", "255+1", "0-1", "128*2", "-uint8(1)", "1%0"})
      require(!analyze(std::string("fn main()->u8:\n  ") + value + "\n").ok(),
              "invalid unsigned byte accepted");
    for (const auto *source :
         {"fn main()->i8:\n  value=1\n  return value\n", "fn main()->i8:\n  return i8(128)\n",
          "fn main()->u8:\n  return u8(-1)\n",
          "fn bad(a:i8,b:u8):\n  return a+b\nfn main()->i64:\n  return 0\n", "fn main()->int128:\n  return 1\n",
          "fn main()->Tensor[float16,1]:\n  return tensor([1])\n", "fn main():\n  return u8([1])\n",
          "type Row:\n  value:Tensor[float16,1]\nfn main()->i64:\n  return 0\n",
          "fn main():\n  return i64(map([1],fn(x):return i8(x)))\n"})
      require(!analyze(source).ok(), std::string("unsupported int8 surface accepted: ") + source);
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto forged = analyze("fn main()->i8:\n  return 1\n");
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
