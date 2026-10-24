#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
CheckResult analyze(std::string source) {
  return check(parse(std::make_shared<SourceFile>("fallible-functions.k", std::move(source))));
}
void positive() {
  for (const auto *source :
       {"error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn choose(flag:Bool)->fn(int)->int ! E:\n  if flag:checked\n  "
        "else:other\nfn main()->int:\n  f=choose(true)\n  f(0) catch fn(error):\n    match "
        "error:\n      E.Bad {value}:value\n",
        "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn double(value:int)->int:\n  value*2\nfn "
        "create(flag:Bool)->(fn(int)->int) ! E:\n  if flag:fail E.Bad {value:42}\n  double\nfn "
        "main()->int:\n  factory=create\n  f=tryFactory(factory)\n  f(21)\nfn "
        "tryFactory(factory:fn(Bool)->(fn(int)->int) ! E)->fn(int)->int:\n  factory(true) catch "
        "fn(error):double\n",
        "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn create(flag:Bool)->(fn(int)->int ! E) ! E:\n  if flag:fail "
        "E.Bad {value:42}\n  checked\nfn run()->int ! E:\n  factory=create\n  f=try "
        "factory(false)\n  try f(0)\nfn main()->int:\n  run() catch fn(error):\n    match error:\n "
        "     E.Bad {value}:value\n",
        "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn recover(value:int)->int:\n  checked(value) catch "
        "fn(error):42\nfn main()->int:\n  f=recover\n  result=map([0,21],f)\n  "
        "result[0]+result[1]-42\n",
        "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn worker(value:int)->int ! E:\n  f=checked\n  try f(value)\nfn "
        "run()->int ! (E | TaskError):\n  try parallel:\n    task=spawn worker(0)\n    try await "
        "task\nfn main()->int:\n  run() catch fn(error):\n    match error:\n      E.Bad "
        "{value}:value\n      _:0\n",
        "type Row:\n  text:String\n  values:[int]\nerror E:\n  Bad:\n    row:Row\nfn "
        "checked(value:int)->[int] ! E:\n  if value==0:fail E.Bad {row:Row "
        "{text:\"é\"+\"!\",values:[20,20]}}\n  [value]\nfn main()->int:\n  f=checked\n  "
        "result=f(0) catch fn(error):\n    match error:\n      E.Bad "
        "{row}:[row.values[0]+row.values[1]+length(row.text)]\n  result[0]\n",
        "error E:\n  Bad\nfn checked(value:int)->int ! E:\n  if value==0:fail E.Bad\n  value\ntype "
        "Alias=E | E\nfn apply(value:int,f:fn(int)->int ! Alias)->int ! E:\n  try f(value)\nfn "
        "main()->int:\n  apply(42,checked) catch fn(error):0\n",
        "fn checked(value:int)->int ! TaskError:\n  value\nfn main()->int:\n  f=checked\n  f(42) "
        "catch fn(error):0\n",
        "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn main()->int ! E:\n  f=checked\n  try f(21)\n",
        "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn main()->int:\n  f=checked\n  result=21->f()\n  result catch "
        "fn(error):0\n",
        "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
        "E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad "
        "{value:21}\n  value*3\nfn main()->int:\n  f=checked\n  result=f(0)\n  first=result catch "
        "fn(error):21\n  second=result catch fn(error):21\n  first+second\n"}) {
    auto result = analyze(source);
    if (!result.ok())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << renderDiagnostic(*result.source, diagnostic);
    require(result.ok(), "valid expected-error callable rejected");
    auto plans = continuation::plan(*result.module);
    require(bool(plans), plans ? "" : plans.error().c_str());
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
    auto native = lowerNative(*result.module);
    require(bool(native), native ? "" : native.error().c_str());
    require(native->llvm.find(".unchecked") == std::string::npos,
            "unchecked function address survived");
#endif
  }
}
void negative() {
  const std::string base = "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if "
                           "value==0:fail E.Bad {value:42}\n  value*2\nfn other(value:int)->int ! "
                           "E:\n  if value==0:fail E.Bad {value:21}\n  value*3\n";
  require(
      analyze(
          base +
          "fn use(value:int,f:fn(int)->int ! E)->int ! E:\n  try f(value)\n"
          "fn forward(f:fn(int)->int ! E)->int ! (E | TaskError):\n  try parallel:\n    try await "
          "spawn use(0,f)\n"
          "fn main()->int ! (E | TaskError):\n  try forward(checked)\n")
          .ok(),
      "fallible callable parameter forwarding rejected");
  // Coordinating callees may now flow through a synchronous catch wrapper;
  // the continuation planner tracks that effect through the function value.
  auto coordinating = analyze(base +
      "fn worker(value:int)->int ! (E | TaskError):\n  try parallel:\n    try await spawn other(value)\n"
      "fn hidden(value:int)->int:\n  worker(value) catch fn(error):0\n"
      "fn main()->int:\n  f=hidden\n  f(1)\n");
  require(coordinating.ok(), "coordinating catch wrapper rejected");
  require(bool(continuation::plan(*coordinating.module)), "coordinating catch wrapper was not planned");
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
  require(bool(lowerNative(*coordinating.module)), "coordinating catch wrapper was not lowered");
#endif
  for (const auto *source :
       {"fn main()->int:\n  f=checked\n  f(1)+1\n",
        "fn main()->int:\n  f:fn(int)->int=checked\n  0\n",
        "error Other:\n  Bad\nfn main()->int:\n  f:fn(int)->int ! Other=checked\n  0\n",
        "fn main()->int:\n  f=checked\n  f(\"bad\") catch fn(error):0\n",
        "fn main()->int:\n  f=checked\n  f() catch fn(error):0\n",
        "fn use(f:fn(int)->int ! Missing)->int:\n  0\nfn main()->int:\n  0\n",
        "fn main()->int:\n  f=checked\n  result=map([1],f)\n  result[0]\n",
        "fn create()->(fn(int)->int ! E) ! E:\n  fail E.Bad {value:42}\nfn main()->int:\n  "
        "f=create()\n  f(1) catch fn(error):0\n",
        "fn use(f:fn(int)->int ! E)->int ! E:\n  try f(1)\nfn main()->int:\n  use(1) catch "
        "fn(error):0\n"}) {
    auto result = analyze(base + source);
    require(!result.ok(), std::string("invalid channel, call or execution boundary accepted: ") + source);
  }
}
void forged() {
  auto result = analyze(
      "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail E.Bad "
      "{value:42}\n  value*2\nfn other(value:int)->int ! E:\n  if value==0:fail E.Bad {value:21}\n "
      " value*3\nfn main()->int:\n  f=checked\n  f(0) catch fn(error):42\n");
  require(result.ok(), "forged IR fixture rejected");
  for (unsigned mode = 0; mode < 9; ++mode) {
    auto module = *result.module;
    bool changed = false;
    for (ValueId id = 0; id < module.expressions.size(); ++id) {
      auto &expression = module.expressions[id];
      if (std::holds_alternative<FunctionReference>(expression.operation) && mode < 4) {
        auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
        if (mode == 0)
          signature->result.fallible = false;
        if (mode == 1)
          signature->result.errorRecord = SIZE_MAX;
        if (mode == 2)
          signature->parameters[0].fallible = true;
        if (mode == 3)
          expression.type.fallible = true;
        expression.type.callable = signature;
        changed = true;
        break;
      }
      if (auto *invoke = std::get_if<FunctionInvoke>(&expression.operation); invoke && mode >= 4) {
        if (mode == 4)
          expression.type.fallible = false;
        if (mode == 5)
          expression.type.errorRecord.reset();
        if (mode == 6)
          expression.type.element = NumericType::Float32;
        if (mode == 7)
          module.expressions[invoke->callee].type.fallible = true;
        if (mode == 8)
          invoke->arguments.clear();
        changed = true;
        break;
      }
    }
    require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
            "forged error channel accepted");
  }
}
} // namespace
int main() {
  try {
    positive();
    negative();
    forged();
    std::cout
        << "Fallible function signatures, factories, error sets and execution boundaries passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
