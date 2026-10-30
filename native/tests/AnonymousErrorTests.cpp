#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
CheckResult analyze(const char *source) {
  return check(parse(std::make_shared<SourceFile>("anonymous-errors.k", source)));
}
} // namespace
int main() {
  try {
    for (
        const auto *source :
        {"error E:\n  Bad\nfn apply(value:int,f:fn(int)->int ! E)->int ! E:\n  try f(value)\nfn "
         "main()->int ! (E | TaskError):\n  f=fn(value:int)->int ! E:value\n  try parallel:\n    "
         "try await spawn apply(42,f)\n",
         "fn worker(value:int)->int:\n  value\nfn run(value:int)->int ! TaskError:\n  try "
         "parallel:\n    try await spawn worker(value)\nfn main()->int:\n  f=fn(value:int)->int ! "
         "TaskError:try run(value)\n  0\n",
         "error E:\n  Bad:\n    value:int\nfn main()->int:\n  offset:int=42\n  "
         "validate=fn(value:int)->int ! E:\n    if value<0:\n      fail E.Bad {value:offset}\n    "
         "value+offset\n  validate(-1) catch fn(error):\n    match error:\n      E.Bad "
         "{value}:value\n",
         "error E:\n  Bad:\n    value:int\nfn main()->int:\n  f=fn(value:int)->int ! E:value*2\n  "
         "f(21) catch fn(error):0\n",
         "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
         "E.Bad {value:42}\n  value\nfn main()->int:\n  f=fn(value:int) ! E:\n    result=try "
         "checked(value)\n    result+1\n  f(0) catch fn(error):\n    match error:\n      E.Bad "
         "{value}:value\n",
         "error E:\n  Bad:\n    value:int\nfn main()->int:\n  f:fn(int)->int ! E=fn(value) ! E:\n  "
         "  if value==0:fail E.Bad {value:42}\n    value\n  f(0) catch fn(error):\n    match "
         "error:\n      E.Bad {value}:value\n",
         "error E:\n  Bad:\n    value:int\nerror Other:\n  Bad:\n    text:String\ntype Both=Other "
         "| E | Other\nfn apply(f:fn(int)->int ! Both)->int ! (E | Other):\n  try f(0)\nfn "
         "main()->int:\n  f=fn(value:int)->int ! (E | Other):\n    if value==0:fail Other.Bad "
         "{text:\"é\"+\"!\"}\n    value\n  result=apply(f) catch fn(error):\n    match error:\n    "
         "  Other.Bad {text}:length(text)+40\n      E.Bad {value}:value\n  result\n",
         "error E:\n  Bad:\n    value:int\nerror BuildError:\n  Disabled\nfn main()->int:\n  "
         "factory=fn(blocked:Bool)->(fn(int)->int ! E) ! BuildError:\n    if blocked:fail "
         "BuildError.Disabled\n    offset:int=21\n    fn(value:int)->int ! E:\n      if "
         "value==0:fail E.Bad {value:offset}\n      value+offset\n  f=factory(false) catch "
         "fn(error):fn(value:int)->int ! E:value\n  first=f(0) catch fn(error):\n    match "
         "error:\n      E.Bad {value}:value\n  second=f(0) catch fn(error):\n    match error:\n    "
         "  E.Bad {value}:value\n  first+second\n",
         "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
         "E.Bad {value:42}\n  value\nfn main()->int:\n  factory=fn()->fn(int)->int ! E:checked\n  "
         "f=factory()\n  f(0) catch fn(error):\n    match error:\n      E.Bad {value}:value\n",
         "type Row:\n  text:String\n  values:[int]\nerror E:\n  Bad:\n    row:Row\nfn "
         "main()->int:\n  row=Row {text:\"é\"+\"!\",values:[20,20]}\n  f=fn(value:int)->[int] ! "
         "E:\n    if value==0:fail E.Bad {row:row}\n    concat(row.values,[value])\n  result=f(0) "
         "catch fn(error):\n    match error:\n      E.Bad "
         "{row}:[row.values[0]+row.values[1]+length(row.text)]\n  result[0]\n",
         "error E:\n  Bad:\n    value:int\nfn worker(offset:int)->int ! E:\n  f=fn(value:int)->int "
         "! E:\n    if value==0:fail E.Bad {value:offset}\n    value\n  try f(0)\nfn run()->int ! "
         "(E | TaskError):\n  try parallel:\n    try await spawn worker(42)\nfn main()->int:\n  "
         "run() catch fn(error):\n    match error:\n      E.Bad {value}:value\n      _:0\n",
         "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
         "E.Bad {value:42}\n  value\nfn main()->int:\n  f=fn(value:int) ! E:checked(value)\n  f(0) "
         "catch fn(error):\n    match error:\n      E.Bad {value}:value\n",
         "error E:\n  Bad:\n    value:int\nfn main()->int:\n  f=fn(value:int)->int ! E:\n    if "
         "value==0:fail E.Bad {value:21}\n    value\n  recover=fn(value:int)->int:\n    f(value) "
         "catch fn(error):21\n  result=map([0,21],recover)\n  result[0]+result[1]\n",
         "fn divide(value:int)->int:\n  1/value\nfn attempt()->int ! TaskError:\n  try parallel:\n "
         "   try await spawn divide(0)\nfn recover(error:TaskError)->int:\n  "
         "f=fn(cause:TaskError)->int ! TaskError:fail cause\n  f(error) catch fn(cause):\n    "
         "match cause:\n      TaskError.Fault {kind}:\n        match kind:\n          "
         "FaultKind.Arithmetic:42\n          _:0\n      _:0\nfn main()->int:\n  attempt() catch "
         "fn(error):recover(error)\n",
         "error E:\n  Bad:\n    value:int\nfn divide(value:int)->int:\n  1/value\nfn "
         "attempt()->int ! TaskError:\n  try parallel:\n    try await spawn divide(0)\nfn "
         "recover(error:TaskError)->int:\n  f=fn(cause:TaskError,flag:Bool)->int ! (E | "
         "TaskError):\n    if flag:fail cause\n    fail E.Bad {value:21}\n  a=f(error,false) catch "
         "fn(cause):\n    match cause:\n      E.Bad {value}:value\n      _:0\n  b=f(error,true) "
         "catch fn(cause):\n    match cause:\n      TaskError.Fault {kind}:21\n      _:0\n  "
         "a+b\nfn main()->int:\n  attempt() catch fn(error):recover(error)\n",
         "error ReadError:\n  Missing\n  Bad:\n    reason:String\n    numbers:[int]\nfn "
         "probe()->int ! ReadError:\n  reason=\"é\"+\"🙂\"\n  numbers:[int]=[20,22]\n  f=fn()->int "
         "! ReadError:\n    fail ReadError.Bad {reason:reason,numbers:numbers}\n  try f()\nfn "
         "main()->int:\n  f=probe\n  0\n",
         "error ReadError:\n  Missing\n  Bad:\n    reason:String\n    numbers:[int]\nfn "
         "probe()->int ! ReadError:\n  f=fn()->int ! ReadError:42\n  try f()\nfn main()->int:\n  "
         "f=probe\n  0\n",
         "error ReadError:\n  Missing\n  Bad:\n    reason:String\n    numbers:[int]\n\n\nfn "
         "divide(value:int)->int:\n  1/value\nfn probe()->int ! ReadError:\n  f=fn(value:int)->int "
         "! ReadError:divide(value)\n  try f(0)\nfn main()->int:\n  f=probe\n  0\n",
         "error E:\n  Bad\nfn main()->int:\n  values:Tensor[int,2,2]=tensor([[10,10],[10,10]])\n  "
         "f=fn(amount:int)->Tensor[int,?,?] ! E:\n    if amount==0:fail E.Bad\n    values+amount\n "
         " good=f(1) catch fn(error):tensor([[0,0],[0,0]])\n  bad=f(0) catch "
         "fn(error):tensor([[0,0],[0,0]])\n  reduceSum(good+bad)-2\n",
         "error E:\n  Bad\nfn main()->int:\n  f=fn(value:int)->Tensor[int] ! E:\n    if "
         "value==0:fail E.Bad\n    reshape(tensor([value]),[])\n  result=f(0) catch "
         "fn(error):reshape(tensor([42]),[])\n  result[]\n",
         "type Row:\n  text:String\n  values:[int]\nerror E:\n  Bad:\n    row:Row\nfn "
         "worker(value:int)->int ! E:\n  row=Row {text:\"é\"+\"!\",values:[20,20]}\n  "
         "f=fn(input:int)->int ! E:\n    if input==0:fail E.Bad {row:row}\n    input\n  try "
         "f(value)\nfn run()->int ! (E | TaskError):\n  try parallel:\n    task=spawn worker(0)\n  "
         "  first=(await task) catch fn(error):\n      match error:\n        E.Bad "
         "{row}:length(row.text)+row.values[0]\n        _:0\n    second=(await task) catch "
         "fn(error):\n      match error:\n        E.Bad {row}:row.values[1]\n        _:0\n    "
         "first+second\nfn main()->int ! (E | TaskError):\n  try run()\n",
         "error E:\n  Bad\nfn main()->int:\n  f=fn(value:int) ! E:\n    adjusted=value+1\n    "
         "adjusted*2\n  f(20) catch fn(error):0\n",
         "error E:\n  Bad\nerror BuildError:\n  Disabled\nfn main()->int:\n  "
         "factory=fn(blocked:Bool)->(fn(int)->int ! E) ! BuildError:\n    if blocked:fail "
         "BuildError.Disabled\n    fn(value:int)->int ! E:value\n  f=factory(true) catch "
         "fn(error):fn(value:int)->int ! E:value*2\n  f(21) catch fn(error):0\n",
         "error E:\n  Bad:\n    value:int\nfn worker(value:int)->int:\n  value*2\nfn main()->int ! "
         "(E | TaskError):\n  f=fn(value:int)->int ! E:\n    if value==42:fail E.Bad "
         "{value:value}\n    value\n  try parallel:\n    value=try await spawn worker(21)\n    "
         "f(value) catch fn(error):\n      match error:\n        E.Bad {value}:value\n",
         "error E:\n  Empty\nfn main()->int:\n  prefix:String=\"é\"\n  f=fn(value:String)->String "
         "! E:\n    if length(value)==0:fail E.Empty\n    prefix+value+\"!\"\n  a=f(\"🙂\") catch "
         "fn(error):\"\"\n  b=f(\"\") catch fn(error):\"\"\n  length(a)+length(b)+39\n",
         "fn declared(value:int)->int ! TaskError:\n  value\nfn main()->int:\n  f=fn(value:int) ! "
         "TaskError:\n    result=try declared(value)\n    result+1\n  f(41) catch fn(error):0\n"}) {
      auto result = analyze(source);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid anonymous error declaration rejected");
      auto plans = continuation::plan(*result.module);
      require(bool(plans), plans ? "" : plans.error().c_str());
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
      auto native = lowerNative(*result.module);
      require(bool(native), native ? "" : native.error().c_str());
#endif
    }
    for (const auto *source :
         {"error E:\n  Bad\nfn main()->int:\n  f=fn(value:int)->int ! E:value\n  f(42)+1\n",
          "error E:\n  Bad\nfn main()->int:\n  f:fn(int)->int=fn(value:int)->int ! E:value\n  0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn main()->int:\n  f:fn(int)->int ! "
          "Other=fn(value:int)->int ! E:value\n  0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn checked(value:int)->int ! Other:\n  fail "
          "Other.Bad\nfn main()->int:\n  f=fn(value:int) ! E:checked(value)\n  0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn checked(value:int)->int ! Other:\n  fail "
          "Other.Bad\nfn main()->int:\n  f=fn(value:int) ! E:\n    result=try checked(value)\n    "
          "result\n  0\n",
          "fn main()->int:\n  f=fn(value:int)->int ! Missing:value\n  0\n",
          "type E:\n  value:int\nfn main()->int:\n  f=fn(value:int)->int ! E:value\n  0\n",
          "type A=B\ntype B=A\nfn main()->int:\n  f=fn(value:int)->int ! A:value\n  0\n",
          "error E:\n  Bad\nfn main()->[int]:\n  map([1],fn(value:int)->int ! E:value)\n",
          "error E:\n  Bad\nfn main()->[int]:\n  f=fn(value:int)->int ! E:value\n  map([1],f)\n",
          "error E:\n  Bad\nfn checked(value:int)->int ! E:\n  fail E.Bad\nfn main()->int:\n  "
          "f=fn(value:int)->int ! E:\n    ignored=checked(value)\n    value\n  0\n",
          "error E:\n  Bad\nfn main()->int:\n  f=fn() ! E:fail E.Bad\n  0\n",
          "error E:\n  Bad\nfn main()->int:\n  f=fn(value)->int ! E:value\n  0\n",
          "error E:\n  Bad\nfn checked()->int ! E:\n  fail E.Bad\nfn main()->int:\n  "
          "result=checked()\n  f=fn()->int ! E:try result\n  0\n",

          "fn main()->int:\n  f=fn()->fn(int)->int ! E ! Other:0\n  0\n",
          "fn main()->int:\n  f=fn(value:int)->int ! E | Other:value\n  0\n",
          "fn main()->int:\n  f=fn(value:int)->int ! :value\n  0\n"})
      require(!analyze(source).ok(), "invalid anonymous error declaration accepted");
    auto parsed = parse(std::make_shared<SourceFile>(
        "headers.k", "fn main()->int:\n  f=fn()->(fn(int)->int ! CallError) ! BuildError:0\n  "
                     "g=fn(value:int) ! (CallError | BuildError):value\n  0\n"));
    require(parsed.ok(), "anonymous header grouping fixture rejected");
    unsigned count = 0;
    for (const auto &expression : parsed.module.expressions)
      if (const auto *lambda = std::get_if<LambdaExpr>(&expression.value)) {
        if (!count) {
          require(lambda->errorTypes.size() == 1 &&
                      parsed.module.source->slice(lambda->errorTypes[0]) == "BuildError",
                  "factory channel moved into returned function");
          require(lambda->result && lambda->result->function &&
                      lambda->result->function->errorTypes.size() == 1 &&
                      parsed.module.source->slice(lambda->result->function->errorTypes[0]) ==
                          "CallError",
                  "returned callable channel moved into factory");
        } else
          require(lambda->errorTypes.size() == 2, "anonymous union spans lost");
        for (auto span : lambda->errorTypes)
          require(span.begin >= expression.span.begin && span.end <= expression.span.end,
                  "channel span escaped anonymous expression");
        ++count;
      }
    require(count == 2, "anonymous AST nodes lost");
    auto always = analyze("error E:\n  Bad\nfn main()->int:\n  f=fn(value:int)->int ! E:value\n  "
                          "f(42) catch fn(error):0\n");
    require(always.ok(), "always successful declared-error fixture rejected");
    bool found = false;
    for (const auto &expression : always.module->expressions)
      if (const auto *ref = std::get_if<FunctionReference>(&expression.operation)) {
        const auto &fn = always.module->functions[ref->function];
        require(!expression.type.fallible && expression.type.callable &&
                    expression.type.callable->result.fallible && fn.resultType.fallible &&
                    fn.declaredResult && fn.declaredResult->fallible,
                "declared invocation channel was erased or applied to creation");
        found = true;
      }
    require(found, "annotated callable reference missing");
    for (const auto *text : {"fn main()->int:\n  f=fn()->fn(int)->int ! E ! Other:0\n  0\n",
                             "fn main()->int:\n  f=fn(value:int)->int ! E | Other:value\n  0\n"}) {
      auto invalid = parse(std::make_shared<SourceFile>("invalid.k", text));
      require(!invalid.ok() && std::ranges::any_of(invalid.diagnostics,
                                                   [](const auto &d) { return d.code == "P006"; }),
              "anonymous header lost grouping diagnostic");
    }
    auto root = std::make_shared<const SourceFile>("root.k", "fn main()->int:\n  0\n");
    auto helper = std::make_shared<const SourceFile>(
        "helper.k", "fn helper()->int:\n  f=fn(value:int)->int ! Missing:value\n  0\n");
    auto combined = std::make_shared<const SourceFile>(
        "root.k", root->text() + helper->text(),
        std::vector<SourceFile::Part>{{0, root}, {root->text().size(), helper}});
    auto origin = check(parse(combined));
    require(!origin.ok(), "unknown imported anonymous channel accepted");
    bool located = false;
    for (const auto &d : origin.diagnostics)
      if (d.code == "S072")
        located |= origin.source->origin(d.span.begin).first == helper.get();
    require(located, "anonymous error diagnostic lost its module origin");
    SyntaxLimits limits;
    limits.maxTokens = 40;
    std::string many = "fn main()->int:\n  f=fn(value:int)->int ! (";
    for (unsigned i = 0; i < 80; ++i) {
      if (i)
        many += " | ";
      many += "E";
    }
    many += "):value\n  0\n";
    require(!parse(std::make_shared<SourceFile>("budget.k", many), limits).ok(),
            "anonymous error list bypassed token budget");
    std::cout << "Anonymous error declarations, inference, canonical signatures, grouping and "
                 "source origins passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
