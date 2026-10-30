#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("control.k", text)));
}
static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
int main() {
  try {
    for (auto type : {"Bool", "int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      auto typed =
          analyze("fn choose(flag:Bool,a:" + std::string(type) + ",b:" + type + ")->" + type +
                  ":\n  if flag:\n    a\n  else:\n    b\nfn main()->Bool:\n  true\n");
      require(typed.ok(), "conditional scalar type rejected");
#ifndef KALANDRA_CONTROL_FRONTEND_ONLY
      auto native = lowerNative(*typed.module);
      require(bool(native), "conditional scalar lowering failed");
      require(native->llvm.find("task.entry.") == std::string::npos,
              "ordinary conditional introduced scheduler entries");
#endif
      require(bool(continuation::plan(*typed.module)), "conditional planning failed");
      require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
              "GPU conditional admitted");
    }
    for (
        auto text :
        {"fn main()->Bool:\n  not false and true or false\n",
         "fn main()->Bool:\n  true and 1<2 or false\n",
         "fn main()->int64:\n  if true: 1\n  else: 2\n",
         "fn main()->int64:\n  if true:\n    if false: 1\n    else: 2\n  else:\n    3\n",
         "fn main()->int64:\n  x=5\n  y=if true:\n    x=2\n    x\n  else:\n    x=3\n    x\n  x+y\n",
         "fn choose(flag:Bool)->Tensor[int64,?]:\n  if flag:\n    tensor([1,2])\n  else:\n    "
         "tensor([3,4,5])\n",
         "fn value()->Bool:\n  true\nfn main()->Bool ! TaskError:\n  try parallel:\n    work=spawn "
         "value()\n    if true:\n      try await work\n    else:\n      try await work\n"})
      require(analyze(text).ok(), "valid Boolean control flow rejected");
    for (auto text :
         {"fn main()->int64:\n  if 1: 2\n  else: 3\n", "fn main()->int64:\n  if true:\n    1\n",
          "fn main()->Bool:\n  true and 1\n", "fn main()->Bool:\n  1 or false\n",
          "fn main()->Bool:\n  not 1\n", "fn main()->Bool:\n  !true\n",
          "fn main():\n  if true: 1\n  else: false\n",
          "fn main()->int64:\n  result=if true:\n    x=1\n    x\n  else:\n    2\n  x\n",
          "fn main()->Bool:\n  and=false\n  and\n", "fn main()->Bool:\n  true and (1<2<3)\n",
          "fn value()->Bool:\n  true\nfn main()->Bool ! TaskError:\n  try parallel:\n    "
          "work=spawn value()\n    if true:\n      try await work\n    else:\n      false\n",
          "fn value()->Bool:\n  true\nfn main()->Bool ! TaskError:\n  try parallel:\n    "
          "work=spawn value()\n    true and (try await work)\n",
          "fn value()->Bool:\n  true\nfn main()->Bool ! TaskError:\n  try parallel:\n    if "
          "true:\n      work=spawn value()\n      true\n    else:\n      false\n"})
      require(!analyze(text).ok(), "invalid conditional/task obligation admitted");
#ifndef KALANDRA_CONTROL_FRONTEND_ONLY
    auto forged = analyze("fn main()->int64:\n  if true: 1\n  else: 2\n");
    require(forged.ok(), "conditional forge fixture rejected");
    for (auto &expr : forged.module->expressions)
      if (const auto *choice = std::get_if<ConditionalOperation>(&expr.operation))
        forged.module->expressions[choice->condition].type.builtin = BuiltinValue::Numeric;
    require(!lowerNative(*forged.module), "non-Boolean condition IR admitted");
#endif
    std::cout << "CPU conditional/logical typing, lexical scopes, precedence, branch task "
                 "obligations, tensor shape joins and ordinary native branches passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
