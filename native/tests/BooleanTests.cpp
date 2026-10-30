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
    for (auto type : {"int32", "uint32", "int64", "uint64", "float32", "float64"})
      for (auto op : {"==", "!=", "<", "<=", ">", ">="}) {
        auto typed = analyze("fn compare(a:" + std::string(type) + ",b:" + type +
                             ") -> Bool:\n  a " + op + " b\nfn main() -> Bool:\n  compare(1,2)\n");
        require(typed.ok(), "numeric comparison rejected");
#ifndef KALANDRA_BOOLEAN_FRONTEND_ONLY
        require(bool(lowerNative(*typed.module)), "numeric comparison did not lower");
#endif
        require(bool(continuation::plan(*typed.module)), "comparison planning failed");
        require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
                "GPU comparison admitted");
      }
    for (auto text : {"fn main()->Bool:\n  true\n", "fn main()->Bool:\n  false\n",
                      "fn main()->Bool:\n  (1<2)==true\n", "fn main()->Bool:\n  true!=false\n",
                      "fn f(x:int32)->Bool:\n  1<x\n", "fn f(x:float32)->Bool:\n  x<1.0\n",
                      "fn main()->Bool:\n  1<1.5\n", "fn main()->Bool:\n  1.5>1\n",
                      "fn twice(x:int64)->int64:\n  x*2\nfn main()->Bool:\n  2->twice>3\n"})
      require(analyze(text).ok(), "valid Boolean source rejected");
    for (auto text :
         {"fn main()->Bool:\n  1<2<3\n", "fn main()->Bool:\n  true<false\n",
          "fn main()->Bool:\n  true==1\n", "fn main()->int64:\n  true+1\n",
          "fn main()->int64:\n  int64(true)\n", "fn main()->Bool:\n  Bool(1)\n",
          "fn main()->Bool:\n  1\n", "fn main()->Bool:\n  true=1\n  false\n",
          "fn f(true:Bool)->Bool:\n  true\n", "fn false()->Bool:\n  true\n",
          "fn f(a:int32,b:int64)->Bool:\n  a==b\n", "fn f(a:uint64,b:int64)->Bool:\n  a<b\n",
          "fn f(a:float32,b:float64)->Bool:\n  a==b\n", "fn f(a:Tensor[int64,2])->Bool:\n  a==a\n",
          "fn f(a:Tensor[Bool,2])->Bool:\n  true\n",
          "fn f()->Bool:\n  true\nfn main()->Bool ! TaskError:\n  try parallel:\n    work=spawn "
          "f()\n    work==true\n",
          "fn f()->Bool ! TaskError:\n  true\nfn main()->Bool:\n  f()==true\n"})
      require(!analyze(text).ok(), "invalid Boolean source admitted");
    for (auto text :
         {"fn main()->Bool ! TaskError:\n  try parallel:\n    false=true\n    true\n",
          "fn f()->Bool ! TaskError:\n  true\nfn main()->Bool:\n  f() catch fn(true): false\n"})
      require(!analyze(text).ok(), "Boolean literal rebound in nested scope");
#ifndef KALANDRA_BOOLEAN_FRONTEND_ONLY
    auto forged = analyze("fn main()->Bool:\n  1<2\n");
    for (auto &expr : forged.module->expressions)
      if (std::holds_alternative<ScalarCompare>(expr.operation))
        expr.type.builtin = BuiltinValue::Numeric;
    require(!lowerNative(*forged.module), "forged comparison result type admitted");
#endif
    auto typed = analyze("fn f(x:int64)->int64:\n  x\nfn main()->Bool:\n  f(1)<f(2)\n");
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
