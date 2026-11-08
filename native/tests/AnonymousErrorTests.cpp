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
        {"error E:\n  Bad\nfn apply(value:i64,f:fn(i64)->i64 ! E)->i64 ! E:\n  return try f(value)\nfn "
         "main()->i64 ! (E | TaskError):\n  f=fn(value:i64)->i64 ! E:return value\n  return try parallel:\n    "
         "try await spawn apply(42,f)\n",
         "fn worker(value:i64)->i64:\n  return value\nfn run(value:i64)->i64 ! TaskError:\n  return try "
         "parallel:\n    try await spawn worker(value)\nfn main()->i64:\n  f=fn(value:i64)->i64 ! "
         "TaskError:return try run(value)\n  return 0\n",
         "error E:\n  Bad:\n    value:i64\nfn main()->i64:\n  offset:i64=42\n  "
         "validate=fn(value:i64)->i64 ! E:\n    if value<0:\n      fail E.Bad {value:offset}\n    "
         "return value+offset\n  return validate(-1) catch fn(error):\n    return match error:\n      E.Bad "
         "{value}:value\n",
         "error E:\n  Bad:\n    value:i64\nfn main()->i64:\n  f=fn(value:i64)->i64 ! E:return value*2\n  return "
         "f(21) catch fn(error):return 0\n",
         "error E:\n  Bad:\n    value:i64\nfn checked(value:i64)->i64 ! E:\n  if value==0:fail "
         "E.Bad {value:42}\n  return value\nfn main()->i64:\n  f=fn(value:i64) ! E:\n    result=try "
         "checked(value)\n    return result+1\n  return f(0) catch fn(error):\n    return match error:\n      E.Bad "
         "{value}:value\n",
         "error E:\n  Bad:\n    value:i64\nfn main()->i64:\n  f:fn(i64)->i64 ! E=fn(value) ! E:\n  "
         "  if value==0:fail E.Bad {value:42}\n    return value\n  return f(0) catch fn(error):\n    return match "
         "error:\n      E.Bad {value}:value\n",
         "error E:\n  Bad:\n    value:i64\nerror Other:\n  Bad:\n    text:String\ntype Both=Other "
         "| E | Other\nfn apply(f:fn(i64)->i64 ! Both)->i64 ! (E | Other):\n  return try f(0)\nfn "
         "main()->i64:\n  f=fn(value:i64)->i64 ! (E | Other):\n    if value==0:fail Other.Bad "
         "{text:\"é\"+\"!\"}\n    return value\n  result=apply(f) catch fn(error):\n    return match error:\n    "
         "  Other.Bad {text}:length(text)+40\n      E.Bad {value}:value\n  return result\n",
         "error E:\n  Bad:\n    value:i64\nerror BuildError:\n  Disabled\nfn main()->i64:\n  "
         "factory=fn(blocked:bool)->(fn(i64)->i64 ! E) ! BuildError:\n    if blocked:fail "
         "BuildError.Disabled\n    offset:i64=21\n    return fn(value:i64)->i64 ! E:\n      if "
         "value==0:fail E.Bad {value:offset}\n      return value+offset\n  f=factory(false) catch "
         "fn(error):return fn(value:i64)->i64 ! E:return value\n  first=f(0) catch fn(error):\n    return match "
         "error:\n      E.Bad {value}:value\n  second=f(0) catch fn(error):\n    return match error:\n    "
         "  E.Bad {value}:value\n  return first+second\n",
         "error E:\n  Bad:\n    value:i64\nfn checked(value:i64)->i64 ! E:\n  if value==0:fail "
         "E.Bad {value:42}\n  return value\nfn main()->i64:\n  factory=fn()->fn(i64)->i64 ! E:return checked\n  "
         "f=factory()\n  return f(0) catch fn(error):\n    return match error:\n      E.Bad {value}:value\n",
         "type Row:\n  text:String\n  values:[i64]\nerror E:\n  Bad:\n    row:Row\nfn "
         "main()->i64:\n  row=Row {text:\"é\"+\"!\",values:[20,20]}\n  f=fn(value:i64)->[i64] ! "
         "E:\n    if value==0:fail E.Bad {row:row}\n    return concat(row.values,[value])\n  result=f(0) "
         "catch fn(error):\n    return match error:\n      E.Bad "
         "{row}:[row.values[0]+row.values[1]+length(row.text)]\n  return result[0]\n",
         "error E:\n  Bad:\n    value:i64\nfn worker(offset:i64)->i64 ! E:\n  f=fn(value:i64)->i64 "
         "! E:\n    if value==0:fail E.Bad {value:offset}\n    return value\n  return try f(0)\nfn run()->i64 ! "
         "(E | TaskError):\n  return try parallel:\n    try await spawn worker(42)\nfn main()->i64:\n  return "
         "run() catch fn(error):\n    return match error:\n      E.Bad {value}:value\n      _:0\n",
         "error E:\n  Bad:\n    value:i64\nfn checked(value:i64)->i64 ! E:\n  if value==0:fail "
         "E.Bad {value:42}\n  return value\nfn main()->i64:\n  f=fn(value:i64) ! E:return checked(value)\n  return f(0) "
         "catch fn(error):\n    return match error:\n      E.Bad {value}:value\n",
         "error E:\n  Bad:\n    value:i64\nfn main()->i64:\n  f=fn(value:i64)->i64 ! E:\n    if "
         "value==0:fail E.Bad {value:21}\n    return value\n  recover=fn(value:i64)->i64:\n    return f(value) "
         "catch fn(error):return 21\n  result=map([0,21],recover)\n  return result[0]+result[1]\n",
         "fn divide(value:i64)->i64:\n  return 1/value\nfn attempt()->i64 ! TaskError:\n  return try parallel:\n "
         "   try await spawn divide(0)\nfn recover(error:TaskError)->i64:\n  "
         "f=fn(cause:TaskError)->i64 ! TaskError:fail cause\n  return f(error) catch fn(cause):\n    return "
         "match cause:\n      TaskError.Fault {kind}:\n        match kind:\n          "
         "FaultKind.Arithmetic:42\n          _:0\n      _:0\nfn main()->i64:\n  return attempt() catch "
         "fn(error):return recover(error)\n",
         "error E:\n  Bad:\n    value:i64\nfn divide(value:i64)->i64:\n  return 1/value\nfn "
         "attempt()->i64 ! TaskError:\n  return try parallel:\n    try await spawn divide(0)\nfn "
         "recover(error:TaskError)->i64:\n  f=fn(cause:TaskError,flag:bool)->i64 ! (E | "
         "TaskError):\n    if flag:fail cause\n    fail E.Bad {value:21}\n  a=f(error,false) catch "
         "fn(cause):\n    return match cause:\n      E.Bad {value}:value\n      _:0\n  b=f(error,true) "
         "catch fn(cause):\n    return match cause:\n      TaskError.Fault {kind}:21\n      _:0\n  "
         "return a+b\nfn main()->i64:\n  return attempt() catch fn(error):return recover(error)\n",
         "error ReadError:\n  Missing\n  Bad:\n    reason:String\n    numbers:[i64]\nfn "
         "probe()->i64 ! ReadError:\n  reason=\"é\"+\"🙂\"\n  numbers:[i64]=[20,22]\n  f=fn()->i64 "
         "! ReadError:\n    fail ReadError.Bad {reason:reason,numbers:numbers}\n  return try f()\nfn "
         "main()->i64:\n  f=probe\n  return 0\n",
         "error ReadError:\n  Missing\n  Bad:\n    reason:String\n    numbers:[i64]\nfn "
         "probe()->i64 ! ReadError:\n  f=fn()->i64 ! ReadError:return 42\n  return try f()\nfn main()->i64:\n  "
         "f=probe\n  return 0\n",
         "error ReadError:\n  Missing\n  Bad:\n    reason:String\n    numbers:[i64]\n\n\nfn "
         "divide(value:i64)->i64:\n  return 1/value\nfn probe()->i64 ! ReadError:\n  f=fn(value:i64)->i64 "
         "! ReadError:return divide(value)\n  return try f(0)\nfn main()->i64:\n  f=probe\n  return 0\n",
         "error E:\n  Bad\nfn main()->i64:\n  values:Tensor[i64,2,2]=tensor([[10,10],[10,10]])\n  "
         "f=fn(amount:i64)->Tensor[i64,?,?] ! E:\n    if amount==0:fail E.Bad\n    return values+amount\n "
         " good=f(1) catch fn(error):return tensor([[0,0],[0,0]])\n  bad=f(0) catch "
         "fn(error):return tensor([[0,0],[0,0]])\n  return reduceSum(good+bad)-2\n",
         "error E:\n  Bad\nfn main()->i64:\n  f=fn(value:i64)->Tensor[i64] ! E:\n    if "
         "value==0:fail E.Bad\n    return reshape(tensor([value]),[])\n  result=f(0) catch "
         "fn(error):return reshape(tensor([42]),[])\n  return result[]\n",
         "type Row:\n  text:String\n  values:[i64]\nerror E:\n  Bad:\n    row:Row\nfn "
         "worker(value:i64)->i64 ! E:\n  row=Row {text:\"é\"+\"!\",values:[20,20]}\n  "
         "f=fn(input:i64)->i64 ! E:\n    if input==0:fail E.Bad {row:row}\n    return input\n  return try "
         "f(value)\nfn run()->i64 ! (E | TaskError):\n  return try parallel:\n    task=spawn worker(0)\n  "
         "  first=(await task) catch fn(error):\n      return match error:\n        E.Bad "
         "{row}:length(row.text)+row.values[0]\n        _:0\n    second=(await task) catch "
         "fn(error):\n      return match error:\n        E.Bad {row}:row.values[1]\n        _:0\n    "
         "first+second\nfn main()->i64 ! (E | TaskError):\n  return try run()\n",
         "error E:\n  Bad\nfn main()->i64:\n  f=fn(value:i64) ! E:\n    adjusted=value+1\n    "
         "return adjusted*2\n  return f(20) catch fn(error):return 0\n",
         "error E:\n  Bad\nerror BuildError:\n  Disabled\nfn main()->i64:\n  "
         "factory=fn(blocked:bool)->(fn(i64)->i64 ! E) ! BuildError:\n    if blocked:fail "
         "BuildError.Disabled\n    return fn(value:i64)->i64 ! E:return value\n  f=factory(true) catch "
         "fn(error):return fn(value:i64)->i64 ! E:return value*2\n  return f(21) catch fn(error):return 0\n",
         "error E:\n  Bad:\n    value:i64\nfn worker(value:i64)->i64:\n  return value*2\nfn main()->i64 ! "
         "(E | TaskError):\n  f=fn(value:i64)->i64 ! E:\n    if value==42:fail E.Bad "
         "{value:value}\n    return value\n  return try parallel:\n    value=try await spawn worker(21)\n    "
         "f(value) catch fn(error):\n      return match error:\n        E.Bad {value}:value\n",
         "error E:\n  Empty\nfn main()->i64:\n  prefix:String=\"é\"\n  f=fn(value:String)->String "
         "! E:\n    if length(value)==0:fail E.Empty\n    return prefix+value+\"!\"\n  a=f(\"🙂\") catch "
         "fn(error):return \"\"\n  b=f(\"\") catch fn(error):return \"\"\n  return length(a)+length(b)+39\n",
         "fn declared(value:i64)->i64 ! TaskError:\n  return value\nfn main()->i64:\n  f=fn(value:i64) ! "
         "TaskError:\n    result=try declared(value)\n    return result+1\n  return f(41) catch fn(error):return 0\n"}) {
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
         {"error E:\n  Bad\nfn main()->i64:\n  f=fn(value:i64)->i64 ! E:return value\n  return f(42)+1\n",
          "error E:\n  Bad\nfn main()->i64:\n  f:fn(i64)->i64=fn(value:i64)->i64 ! E:return value\n  return 0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn main()->i64:\n  f:fn(i64)->i64 ! "
          "Other=fn(value:i64)->i64 ! E:return value\n  return 0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn checked(value:i64)->i64 ! Other:\n  fail "
          "Other.Bad\nfn main()->i64:\n  f=fn(value:i64) ! E:return checked(value)\n  return 0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn checked(value:i64)->i64 ! Other:\n  fail "
          "Other.Bad\nfn main()->i64:\n  f=fn(value:i64) ! E:\n    result=try checked(value)\n    "
          "return result\n  return 0\n",
          "fn main()->i64:\n  f=fn(value:i64)->i64 ! Missing:return value\n  return 0\n",
          "type E:\n  value:i64\nfn main()->i64:\n  f=fn(value:i64)->i64 ! E:return value\n  return 0\n",
          "type A=B\ntype B=A\nfn main()->i64:\n  f=fn(value:i64)->i64 ! A:return value\n  return 0\n",
          "error E:\n  Bad\nfn main()->[i64]:\n  return map([1],fn(value:i64)->i64 ! E:return value)\n",
          "error E:\n  Bad\nfn main()->[i64]:\n  f=fn(value:i64)->i64 ! E:return value\n  return map([1],f)\n",
          "error E:\n  Bad\nfn checked(value:i64)->i64 ! E:\n  fail E.Bad\nfn main()->i64:\n  "
          "f=fn(value:i64)->i64 ! E:\n    ignored=checked(value)\n    return value\n  return 0\n",
          "error E:\n  Bad\nfn main()->i64:\n  f=fn() ! E:fail E.Bad\n  return 0\n",
          "error E:\n  Bad\nfn main()->i64:\n  f=fn(value)->i64 ! E:return value\n  return 0\n",
          "error E:\n  Bad\nfn checked()->i64 ! E:\n  fail E.Bad\nfn main()->i64:\n  "
          "result=checked()\n  f=fn()->i64 ! E:return try result\n  return 0\n",

          "fn main()->i64:\n  f=fn()->fn(i64)->i64 ! E ! Other:0\n  0\n",
          "fn main()->i64:\n  f=fn(value:i64)->i64 ! E | Other:value\n  0\n",
          "fn main()->i64:\n  f=fn(value:i64)->i64 ! :value\n  0\n"})
      require(!analyze(source).ok(), "invalid anonymous error declaration accepted");
    auto parsed = parse(std::make_shared<SourceFile>(
        "headers.k", "fn main()->i64:\n  f=fn()->(fn(i64)->i64 ! CallError) ! BuildError:return 0\n  "
                     "g=fn(value:i64) ! (CallError | BuildError):return value\n  return 0\n"));
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
    auto always = analyze("error E:\n  Bad\nfn main()->i64:\n  f=fn(value:i64)->i64 ! E:return value\n  return "
                          "f(42) catch fn(error):return 0\n");
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
    for (const auto *text : {"fn main()->i64:\n  f=fn()->fn(i64)->i64 ! E ! Other:0\n  0\n",
                             "fn main()->i64:\n  f=fn(value:i64)->i64 ! E | Other:value\n  0\n"}) {
      auto invalid = parse(std::make_shared<SourceFile>("invalid.k", text));
      require(!invalid.ok() && std::ranges::any_of(invalid.diagnostics,
                                                   [](const auto &d) { return d.code == "P006"; }),
              "anonymous header lost grouping diagnostic");
    }
    auto root = std::make_shared<const SourceFile>("root.k", "fn main()->i64:\n  return 0\n");
    auto helper = std::make_shared<const SourceFile>(
        "helper.k", "fn helper()->i64:\n  f=fn(value:i64)->i64 ! Missing:return value\n  return 0\n");
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
    std::string many = "fn main()->i64:\n  f=fn(value:i64)->i64 ! (";
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
