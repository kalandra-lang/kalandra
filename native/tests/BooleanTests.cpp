#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("boolean.k", text)));
}
static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (auto type : {"i32", "u32", "i64", "u64", "f32", "f64"})
      for (auto op : {"==", "!=", "<", "<=", ">", ">="}) {
        auto typed = analyze("fn compare(a:" + std::string(type) + ",b:" + type +
                             ") -> bool:\n  a " + op + " b\nfn main() -> bool:\n  compare(1,2)\n");
        require(typed.ok(), "numeric comparison rejected");
#ifndef KALANDRA_BOOLEAN_FRONTEND_ONLY
        require(bool(lowerNative(*typed.module)), "numeric comparison did not lower");
#endif
        require(bool(continuation::plan(*typed.module)), "comparison planning failed");
        require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                "GPU comparison admitted");
      }
    for (auto text : {"fn main()->bool:\n  return true\n", "fn main()->bool:\n  return false\n",
                      "fn main()->bool:\n  return (1<2)==true\n", "fn main()->bool:\n  return true!=false\n",
                      "fn f(x:i32)->bool:\n  return 1<x\n", "fn f(x:f32)->bool:\n  return x<1.0\n",
                      "fn main()->bool:\n  return 1<1.5\n", "fn main()->bool:\n  return 1.5>1\n",
                      "fn twice(x:i64)->i64:\n  return x*2\nfn main()->bool:\n  return 2->twice>3\n"})
      require(analyze(text).ok(), "valid Boolean source rejected");
    for (auto text :
         {"fn main()->bool:\n  1<2<3\n", "fn main()->bool:\n  return true<false\n",
          "fn main()->bool:\n  return true==1\n", "fn main()->i64:\n  return true+1\n",
          "fn main()->i64:\n  return i64(true)\n", "fn main()->bool:\n  return bool(1)\n",
          "fn main()->bool:\n  return 1\n", "fn main()->bool:\n  true=1\n  return false\n",
          "fn f(true:bool)->bool:\n  return true\n", "fn false()->bool:\n  return true\n",
          "fn f(a:i32,b:i64)->bool:\n  return a==b\n", "fn f(a:u64,b:i64)->bool:\n  return a<b\n",
          "fn f(a:f32,b:f64)->bool:\n  return a==b\n", "fn f(a:Tensor[i64,2])->bool:\n  return a==a\n",
          "fn f(a:Tensor[bool,2])->bool:\n  return true\n",
          "fn f()->bool:\n  return true\nfn main()->bool ! TaskError:\n  return try parallel:\n    work=spawn "
          "f()\n    work==true\n",
          "fn f()->bool ! TaskError:\n  return true\nfn main()->bool:\n  return f()==true\n"})
      require(!analyze(text).ok(), "invalid Boolean source admitted");
    for (auto text :
         {"fn main()->bool ! TaskError:\n  return try parallel:\n    false=true\n    true\n",
          "fn f()->bool ! TaskError:\n  return true\nfn main()->bool:\n  return f() catch fn(true): return false\n"})
      require(!analyze(text).ok(), "Boolean literal rebound in nested scope");
#ifndef KALANDRA_BOOLEAN_FRONTEND_ONLY
    auto forged = analyze("fn main()->bool:\n  return 1<2\n");
    for (auto &expr : forged.module->expressions)
      if (std::holds_alternative<ScalarCompare>(expr.operation))
        expr.type.builtin = BuiltinValue::Numeric;
    require(!lowerNative(*forged.module), "forged comparison result type admitted");
#endif
    auto typed = analyze("fn f(x:i64)->i64:\n  return x\nfn main()->bool:\n  return f(1)<f(2)\n");
    require(typed.ok(), "comparison call source rejected");
    unsigned calls = 0;
    for (const auto &expr : typed.module->expressions)
      calls += std::holds_alternative<FunctionCall>(expr.operation);
    require(calls == 2, "comparison duplicated operand calls");
    std::cout << "36 CPU numeric comparisons, Bool literals/equality, precedence, operand "
                 "evaluation, strict type/task checks and GPU gate passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
