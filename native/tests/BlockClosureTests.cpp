#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
CheckResult analyze(const char *source) {
  return check(parse(std::make_shared<SourceFile>("block-closures.k", source)));
}
} // namespace
int main() {
  try {
    for (const auto *source :
         {"fn main()->i64:\n  f=fn(value:i64):\n    value=value+1\n    return value\n  return "
          "f(41)\n",
          "fn main()->i64:\n  f=fn(value:i64):\n    adjusted=value+1\n    adjusted=adjusted+1\n    "
          "return adjusted\n  return f(40)\n",
          "fn main()->i64:\n  f=fn(value:i64):\n    if value>0:value\n    return value\n  return "
          "f(42)\n",
          "fn worker(value:i64)->i64:\n  return value\nfn run(value:i64)->i64 ! TaskError:\n  "
          "return try "
          "parallel:\n    try await spawn worker(value)\nfn main()->i64:\n  f=fn(value:i64):\n    "
          "result=run(value) catch fn(error):return 0\n    return result\n  return f(42)\n",
          "fn makeAdder(offset:i64)->fn(i64)->i64:\n  return fn(value):\n    "
          "adjusted=value+offset\n    "
          "return adjusted*2\nfn main()->i64:\n  addOne=makeAdder(1)\n  return addOne(20)\n",
          "fn main()->i64:\n  f:fn(i64)->i64=fn(value):\n    adjusted=value+1\n    return "
          "adjusted*2\n  "
          "return f(20)\n",
          "fn make(base:i64)->fn(i64)->fn(i64)->i64:\n  return fn(first):\n    "
          "subtotal=base+first\n    return fn(second):\n      result=subtotal+second\n      return "
          "result\nfn main()->i64:\n  outer=make(10)\n  inner=outer(20)\n  return inner(12)\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value):\n    adjusted=offset+value\n    "
          "return if "
          "adjusted>0:adjusted\n    else:0-adjusted\nfn main()->i64:\n  f=make(20)\n  return "
          "f(22)\n",
          "fn make(value:i64)->fn(i64)->i64:\n  return fn(input):\n    value=value+input\n    "
          "return value*2\nfn "
          "main()->i64:\n  f=make(20)\n  return f(1)\n",
          "fn make(values:[i64])->fn(i64)->[i64]:\n  return fn(value):\n    tail:[i64]=[value]\n   "
          " return "
          "concat(values,tail)\nfn main()->i64:\n  f=make([10,20])\n  result=f(12)\n  "
          "return result[0]+result[1]+result[2]\n",
          "type Row:\n  text:String\n  values:[i64]\nfn make(row:Row)->fn(i64)->Row:\n  return "
          "fn(value):\n    text=row.text+\"!\"\n    values=concat(row.values,[value])\n    return "
          "Row "
          "{text:text,values:values}\nfn main()->i64:\n  f=make(Row "
          "{text:\"é\"+\"🙂\",values:[10,20]})\n  result=f(9)\n  return "
          "length(result.text)+result.values[0]+result.values[1]+result.values[2]\n",
          "error E:\n  Bad:\n    value:i64\nfn make(offset:i64)->fn(i64)->i64 ! E:\n  return "
          "fn(value):\n    if value<0:\n      fail E.Bad {value:offset}\n    "
          "adjusted=value+offset\n    return adjusted\nfn main()->i64:\n  f=make(21)\n  a=f(-1) "
          "catch fn(error):\n    return match error:\n      E.Bad {value}:value\n  b=f(0) catch "
          "fn(error):return 0\n  return a+b\n",
          "error E:\n  Bad:\n    value:i64\nfn checked(value:i64,offset:i64)->i64 ! E:\n  if "
          "value==0:fail E.Bad {value:offset}\n  return value+offset\nfn "
          "make(offset:i64)->fn(i64)->i64 ! "
          "E:\n  return fn(value):\n    adjusted=try checked(value,offset)\n    return "
          "adjusted+1\nfn "
          "main()->i64:\n  f=make(20)\n  a=f(0) catch fn(error):\n    return match error:\n      "
          "E.Bad "
          "{value}:value\n  b=f(1) catch fn(error):return 0\n  return a+b\n",
          "error E:\n  Bad:\n    value:i64\nfn checked(value:i64)->i64 ! E:\n  if value==0:fail "
          "E.Bad {value:21}\n  return value\nfn make(offset:i64)->fn(i64)->i64:\n  return "
          "fn(value):\n    "
          "outcome=checked(value)\n    return outcome catch fn(error):\n      fallback=offset+1\n  "
          "    "
          "return fallback\nfn main()->i64:\n  f=make(20)\n  result=map([0,21],f)\n  return "
          "result[0]+result[1]\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value):\n    adjusted=value+offset\n    "
          "return adjusted*2\nfn worker(value:i64)->i64:\n  f=make(1)\n  return f(value)\nfn "
          "main()->i64 ! "
          "TaskError:\n  return try parallel:\n    try await spawn worker(20)\n",
          "fn worker(value:i64)->i64:\n  return value*2\nfn make(offset:i64)->fn(i64)->i64:\n  "
          "return fn(value):\n    adjusted=value+offset\n    return adjusted\nfn main()->i64 ! "
          "TaskError:\n  f=make(10)\n  return try parallel:\n    value=try await spawn "
          "worker(16)\n    f(value)\n",
          "fn main()->i64:\n  offset:i64=10\n  f=fn(value:i64):\n    adjusted=value+offset\n    "
          "return "
          "adjusted\n  result=map([10,12],f)\n  return result[0]+result[1]\n",
          "fn main()->i64:\n  offset:i64=42\n  f=fn(value:i64):\n    divisor=value-value\n    "
          "return offset/divisor\n  empty:[i64]=[]\n  result=map(empty,f)\n  return "
          "length(result)+offset\n",
          "enum Choice:\n  Left:\n    value:i64\n  Right:\n    value:i64\nfn "
          "make(offset:i64)->fn(Choice)->i64:\n  return fn(choice):\n    base=offset*2\n    return "
          "match "
          "choice:\n      Choice.Left {value}:base+value\n      Choice.Right "
          "{value}:base-value\nfn "
          "main()->i64:\n  f=make(20)\n  return f(Choice.Left {value:2})\n",
          "fn make(offset:i64)->fn(i64)->fn(i64)->i64:\n  return fn(first):\n    "
          "values:[i64]=[offset,first]\n    return fn(second):\n      "
          "result=values[0]+values[1]+second\n  "
          "    return result\nfn main()->i64:\n  f=make(10)\n  g=f(20)\n  return g(12)\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value):\n    adjusted=value+offset\n    "
          "return adjusted*2\nfn main()->i64:\n  f=make(1)\n  return 20->f()\n",
          "fn main()->i64:\n  first:fn(i64)->i64=fn(value):\n    adjusted=value+1\n    return "
          "adjusted*2\n "
          " second=fn(value:i64):\n    result=first(value)\n    return result\n  return "
          "second(20)\n",
          "error BuildError:\n  Disabled\nfn make(offset:i64)->(fn(i64)->i64) ! BuildError:\n  if "
          "offset==0:fail BuildError.Disabled\n  return fn(value):\n    adjusted=value+offset\n    "
          "return adjusted*2\nfn main()->i64:\n  f=make(1) catch fn(error):return "
          "fn(value:i64):return value\n  return f(20)\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value):\n    local=value+offset\n    "
          "return if "
          "value>0:\n      other=local+1\n      other\n    else:\n      other=local-1\n      "
          "other\nfn main()->i64:\n  f=make(20)\n  return f(21)\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value):\n    adjusted=value+offset\n    "
          "return adjusted\nfn probe(input:i64)->i64:\n  f=make(input)\n  return f(2)\nfn "
          "main()->i64:\n  return "
          "probe(40)\n",
          "type Row:\n  text:String\n  values:[i64]\nfn make(row:Row)->fn(i64)->i64:\n  return "
          "fn(value):\n    sum=row.values[0]+row.values[1]\n    count=length(row.text)\n    "
          "return sum+count+value\nfn probe(input:i64)->i64:\n  f=make(Row "
          "{text:\"é\"+\"!\",values:[10,20]})\n  return f(input)\nfn main()->i64:\n  return "
          "probe(10)\n",
          "type Row:\n  text:String\n  values:[i64]\nfn make(offset:i64)->fn(i64)->i64:\n  row=Row "
          "{text:\"é\"+\"!\",values:[10,20]}\n  return fn(value):\n    "
          "sum=row.values[0]+row.values[1]\n   "
          " count=length(row.text)\n    return sum+count+offset+value\nfn main()->i64:\n  "
          "f=make(10)\n  "
          "return f(0)\n",
          "fn make(base:i64)->fn(i64)->i64:\n  return fn(value):\n    adjusted=base+1\n    "
          "return adjusted/value\nfn probe(input:i64)->i64:\n  f=make(41)\n  return f(input)\nfn "
          "main()->i64:\n  return "
          "probe(1)\n",
          "type Row:\n  text:String\n  values:[i64]\nfn make(row:Row)->fn(i64)->Row:\n  return "
          "fn(value):\n    words=row.text+\"!\"\n    numbers=concat(row.values,[value])\n    "
          "return Row "
          "{text:words,values:numbers}\nfn worker(value:i64)->Row:\n  f=make(Row "
          "{text:\"é\"+\"🙂\",values:[10,20]})\n  return f(value)\nfn run()->i64 ! TaskError:\n  "
          "return try "
          "parallel:\n    task=spawn worker(9)\n    first=try await task\n    second=try await "
          "task\n    first.values[0]+first.values[1]+second.values[2]+length(second.text)\nfn "
          "main()->i64 ! TaskError:\n  return try run()\n",
          "error E:\n  Bad:\n    value:i64\nfn make(offset:i64)->fn(i64)->i64 ! E:\n  return "
          "fn(value):\n    if value==0:\n      reason=offset*2\n      fail E.Bad {value:reason}\n  "
          "  divisor=value-value\n    return offset/divisor\nfn main()->i64:\n  f=make(21)\n  "
          "return f(0) catch fn(error):\n    return match error:\n      E.Bad {value}:value\n",
          "fn main()->i64:\n  f=fn(value:i64)->i64:\n    first=value+1\n    return first*2\n  "
          "return f(20)\n",
          "fn main()->i64:\n  prefix:String=\"é\"\n  f=fn(value:String)->String:\n    "
          "text=prefix+value\n    return text+\"!\"\n  return length(f(\"🙂\"))+39\n"}) {
      auto result = analyze(source);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid block closure rejected");
      auto plans = continuation::plan(*result.module);
      require(bool(plans), plans ? "" : plans.error().c_str());
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
      auto native = lowerNative(*result.module);
      require(bool(native), native ? "" : native.error().c_str());
#endif
    }
    // Worker aggregate transport now supports these former rejection cases.
    for (const auto *accepted : {"fn make(value:i64)->fn(i64)->i64:\n  "
                                 "targets=[fn(item:i64):return item+value]\n  return fn(item):\n   "
                                 " result=targets[0](item)\n    return result\nfn main()->i64 ! "
                                 "TaskError:\n  return try parallel:\n    "
                                 "f=try await spawn make(21)\n    f(21)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    // Scalar rebinding is now supported; the rejection catalog keeps actual
    // scope, type, error-channel and incomplete-suite violations.
    for (const auto *source :
         {"fn main()->i64:\n  f=fn(value:i64):\n    hidden=value+1\n    return hidden\n  return "
          "hidden\n",
          "fn main()->i64:\n  f=fn(value):\n    adjusted=value+1\n    return adjusted\n  return "
          "f(41)\n",
          "fn main()->i64:\n  f=fn(value:i64):\n    adjusted:String=value\n    return "
          "length(adjusted)\n  "
          "return f(41)\n",
          "fn main()->i64:\n  f=fn(value:i64)->String:\n    adjusted=value+1\n    return "
          "adjusted\n  return 0\n",
          "error E:\n  Bad\nfn checked(value:i64)->i64 ! E:\n  fail E.Bad\nfn main()->i64:\n  "
          "f=fn(value:i64):\n    ignored=checked(value)\n    return value\n  return f(42)\n",
          "error E:\n  Bad\nfn main()->i64:\n  f=fn(value:i64)->i64:\n    if value==0:fail E.Bad\n "
          "   return value\n  return f(42)\n",
          "error E:\n  Bad\nfn make()->fn(i64)->i64 ! E:\n  return fn(value):\n    return if "
          "value==0:fail "
          "E.Bad\nfn main()->i64:\n  return 0\n",

          "error E:\n  Bad\nfn checked()->i64 ! E:\n  fail E.Bad\nfn main()->i64:\n  "
          "outcome=checked()\n  f=fn(value:i64):\n    result=outcome\n    return result\n  return "
          "0\n",
          "fn main()->i64:\n  f=fn(value:i64)->i64:\n    adjusted=value+1\n  return 42\n",
          "fn main()->i64:\n  f=fn(value:i64):\n  42\n",
          "fn main()->[i64]:\n  map([1],fn(value):\n    adjusted=value+1\n    adjusted)\n"})
      require(!analyze(source).ok(), "invalid block closure accepted: " + std::string(source));
    require(analyze("fn main()->i64:\n  f=fn(value:i64):\n    adjusted=value+1\n  return 42\n").ok(),
            "unit-valued block closure rejected");
    auto tree = parse(std::make_shared<SourceFile>(
        "tree.k", "fn main()->i64:\n  f=fn(value:i64):\n    "
                  "adjusted:i64=value+1\n    return adjusted*2\n  return f(20)\n"));
    require(tree.ok(), "block AST fixture rejected");
    bool found = false;
    for (ExprId id = 0; id < tree.module.expressions.size(); ++id)
      if (const auto *lambda = std::get_if<LambdaExpr>(&tree.module.expressions[id].value)) {
        require(lambda->body.size() == 2 && lambda->body[0].binding && lambda->body[0].annotation &&
                    !lambda->body[1].binding,
                "block AST lost ordered bindings or result");
        for (const auto &statement : lambda->body) {
          require(statement.expression < id, "block AST child does not precede parent");
          require(statement.span.begin >= tree.module.expressions[id].span.begin &&
                      statement.span.end <= tree.module.expressions[id].span.end,
                  "block AST span escaped closure");
        }
        found = true;
      }
    require(found && tree.module.functions[0].body.size() == 2,
            "dedented call attached to closure body");
    for (auto text :
         {"fn main()->i64:\n  f=fn(value:i64):\n    adjusted=value+1\n  return 0\n",
          "fn main()->i64:\n  f=fn(value:i64):\n    return if value>0:value\n  return 0\n"}) {
      auto parsed = parse(std::make_shared<SourceFile>("statement-ending.k", text));
      if (std::string_view(text).find("return if") == std::string_view::npos)
        require(parsed.ok(), "statement-ending closure syntax rejected");
      else {
        require(!parsed.ok(), "result if without else accepted");
        require(std::ranges::any_of(parsed.diagnostics,
                                    [](const auto &d) { return d.code == "P007"; }),
                "incomplete result if lost its parser diagnostic");
      }
    }
    SyntaxLimits limits;
    limits.maxDepth = 8;
    std::string deep = "fn main()->i64:\n";
    for (unsigned i = 0; i < 24; ++i)
      deep += std::string(2 + i * 2, ' ') + "fn():\n";
    deep += std::string(50, ' ') + "42\n";
    auto limited = parse(std::make_shared<SourceFile>("deep.k", deep), limits);
    require(!limited.ok(), "block closure nesting bypassed syntax budget");
    limits = {};
    limits.maxNodes = 8;
    std::string large = "fn main()->i64:\n  f=fn(value:i64):\n";
    for (unsigned i = 0; i < 32; ++i)
      large += "    v" + std::to_string(i) + "=value+1\n";
    large += "    value\n  f(42)\n";
    require(!parse(std::make_shared<SourceFile>("large.k", large), limits).ok(),
            "block body bypassed expression node budget");
    std::string crlf = "fn main()->i64:\n  f=fn(value:i64):\n    adjusted=value+1\n    return "
                       "adjusted*2\n  return f(20)";
    require(analyze(crlf.c_str()).ok(), "CRLF or EOF block termination changed");
    auto mapTree = parse(std::make_shared<SourceFile>(
        "map.k", "error E:\n  Bad\nfn checked()->i64 ! E:\n  fail E.Bad\nfn main()->i64:\n  "
                 "f=fn(value:i64):\n    ignored=checked()\n    return value\n  "
                 "result=map([42],f)\n  return result[0]\n"));
    require(mapTree.ok(), "direct map block AST fixture rejected");
    ExprId callback = 0;
    for (ExprId id = 0; id < mapTree.module.expressions.size(); ++id)
      if (std::holds_alternative<LambdaExpr>(mapTree.module.expressions[id].value))
        callback = id;
    auto &mainBody = mapTree.module.functions.back().body;
    mainBody.erase(mainBody.begin());
    auto &call = std::get<CallExpr>(mapTree.module.expressions[mainBody.front().expression].value);
    call.arguments[1] = callback;
    auto invalidMap = check(std::move(mapTree));
    require(!invalidMap.ok() && std::ranges::any_of(invalidMap.diagnostics,
                                                    [](const auto &d) { return d.code == "S036"; }),
            "direct map block discarded an expected error");
    std::cout << "Block closure syntax, ordered bindings, captures, errors and budgets passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
