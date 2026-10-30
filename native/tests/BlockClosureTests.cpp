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
  return check(parse(std::make_shared<SourceFile>("block-closures.k", source)));
}
} // namespace
int main() {
  try {
    for (
        const auto *source :
        {"fn worker(value:int)->int:\n  value\nfn run(value:int)->int ! TaskError:\n  try "
         "parallel:\n    try await spawn worker(value)\nfn main()->int:\n  f=fn(value:int):\n    "
         "result=run(value) catch fn(error):0\n    result\n  f(42)\n",
         "fn makeAdder(offset:int)->fn(int)->int:\n  fn(value):\n    adjusted=value+offset\n    "
         "adjusted*2\nfn main()->int:\n  addOne=makeAdder(1)\n  addOne(20)\n",
         "fn main()->int:\n  f:fn(int)->int=fn(value):\n    adjusted=value+1\n    adjusted*2\n  "
         "f(20)\n",
         "fn make(base:int)->fn(int)->fn(int)->int:\n  fn(first):\n    subtotal=base+first\n    "
         "fn(second):\n      result=subtotal+second\n      result\nfn main()->int:\n  "
         "outer=make(10)\n  inner=outer(20)\n  inner(12)\n",
         "fn make(offset:int)->fn(int)->int:\n  fn(value):\n    adjusted=offset+value\n    if "
         "adjusted>0:adjusted\n    else:0-adjusted\nfn main()->int:\n  f=make(20)\n  f(22)\n",
         "fn make(value:int)->fn(int)->int:\n  fn(input):\n    value=value+input\n    value*2\nfn "
         "main()->int:\n  f=make(20)\n  f(1)\n",
         "fn make(values:[int])->fn(int)->[int]:\n  fn(value):\n    tail:[int]=[value]\n    "
         "concat(values,tail)\nfn main()->int:\n  f=make([10,20])\n  result=f(12)\n  "
         "result[0]+result[1]+result[2]\n",
         "type Row:\n  text:String\n  values:[int]\nfn make(row:Row)->fn(int)->Row:\n  "
         "fn(value):\n    text=row.text+\"!\"\n    values=concat(row.values,[value])\n    Row "
         "{text:text,values:values}\nfn main()->int:\n  f=make(Row "
         "{text:\"é\"+\"🙂\",values:[10,20]})\n  result=f(9)\n  "
         "length(result.text)+result.values[0]+result.values[1]+result.values[2]\n",
         "error E:\n  Bad:\n    value:int\nfn make(offset:int)->fn(int)->int ! E:\n  fn(value):\n  "
         "  if value<0:\n      fail E.Bad {value:offset}\n    adjusted=value+offset\n    "
         "adjusted\nfn main()->int:\n  f=make(21)\n  a=f(-1) catch fn(error):\n    match error:\n  "
         "    E.Bad {value}:value\n  b=f(0) catch fn(error):0\n  a+b\n",
         "error E:\n  Bad:\n    value:int\nfn checked(value:int,offset:int)->int ! E:\n  if "
         "value==0:fail E.Bad {value:offset}\n  value+offset\nfn make(offset:int)->fn(int)->int ! "
         "E:\n  fn(value):\n    adjusted=try checked(value,offset)\n    adjusted+1\nfn "
         "main()->int:\n  f=make(20)\n  a=f(0) catch fn(error):\n    match error:\n      E.Bad "
         "{value}:value\n  b=f(1) catch fn(error):0\n  a+b\n",
         "error E:\n  Bad:\n    value:int\nfn checked(value:int)->int ! E:\n  if value==0:fail "
         "E.Bad {value:21}\n  value\nfn make(offset:int)->fn(int)->int:\n  fn(value):\n    "
         "outcome=checked(value)\n    outcome catch fn(error):\n      fallback=offset+1\n      "
         "fallback\nfn main()->int:\n  f=make(20)\n  result=map([0,21],f)\n  result[0]+result[1]\n",
         "fn make(offset:int)->fn(int)->int:\n  fn(value):\n    adjusted=value+offset\n    "
         "adjusted*2\nfn worker(value:int)->int:\n  f=make(1)\n  f(value)\nfn main()->int ! "
         "TaskError:\n  try parallel:\n    try await spawn worker(20)\n",
         "fn worker(value:int)->int:\n  value*2\nfn make(offset:int)->fn(int)->int:\n  "
         "fn(value):\n    adjusted=value+offset\n    adjusted\nfn main()->int ! TaskError:\n  "
         "f=make(10)\n  try parallel:\n    value=try await spawn worker(16)\n    f(value)\n",
         "fn main()->int:\n  offset:int=10\n  f=fn(value:int):\n    adjusted=value+offset\n    "
         "adjusted\n  result=map([10,12],f)\n  result[0]+result[1]\n",
         "fn main()->int:\n  offset:int=42\n  f=fn(value:int):\n    divisor=value-value\n    "
         "offset/divisor\n  empty:[int]=[]\n  result=map(empty,f)\n  length(result)+offset\n",
         "enum Choice:\n  Left:\n    value:int\n  Right:\n    value:int\nfn "
         "make(offset:int)->fn(Choice)->int:\n  fn(choice):\n    base=offset*2\n    match "
         "choice:\n      Choice.Left {value}:base+value\n      Choice.Right {value}:base-value\nfn "
         "main()->int:\n  f=make(20)\n  f(Choice.Left {value:2})\n",
         "fn make(offset:int)->fn(int)->fn(int)->int:\n  fn(first):\n    "
         "values:[int]=[offset,first]\n    fn(second):\n      result=values[0]+values[1]+second\n  "
         "    result\nfn main()->int:\n  f=make(10)\n  g=f(20)\n  g(12)\n",
         "fn make(offset:int)->fn(int)->int:\n  fn(value):\n    adjusted=value+offset\n    "
         "adjusted*2\nfn main()->int:\n  f=make(1)\n  20->f()\n",
         "fn main()->int:\n  first:fn(int)->int=fn(value):\n    adjusted=value+1\n    adjusted*2\n "
         " second=fn(value:int):\n    result=first(value)\n    result\n  second(20)\n",
         "error BuildError:\n  Disabled\nfn make(offset:int)->(fn(int)->int) ! BuildError:\n  if "
         "offset==0:fail BuildError.Disabled\n  fn(value):\n    adjusted=value+offset\n    "
         "adjusted*2\nfn main()->int:\n  f=make(1) catch fn(error):fn(value:int):value\n  f(20)\n",
         "fn make(offset:int)->fn(int)->int:\n  fn(value):\n    local=value+offset\n    if "
         "value>0:\n      other=local+1\n      other\n    else:\n      other=local-1\n      "
         "other\nfn main()->int:\n  f=make(20)\n  f(21)\n",
         "fn make(offset:int)->fn(int)->int:\n  fn(value):\n    adjusted=value+offset\n    "
         "adjusted\nfn probe(input:int)->int:\n  f=make(input)\n  f(2)\nfn main()->int:\n  "
         "probe(40)\n",
         "type Row:\n  text:String\n  values:[int]\nfn make(row:Row)->fn(int)->int:\n  "
         "fn(value):\n    sum=row.values[0]+row.values[1]\n    count=length(row.text)\n    "
         "sum+count+value\nfn probe(input:int)->int:\n  f=make(Row "
         "{text:\"é\"+\"!\",values:[10,20]})\n  f(input)\nfn main()->int:\n  probe(10)\n",
         "type Row:\n  text:String\n  values:[int]\nfn make(offset:int)->fn(int)->int:\n  row=Row "
         "{text:\"é\"+\"!\",values:[10,20]}\n  fn(value):\n    sum=row.values[0]+row.values[1]\n   "
         " count=length(row.text)\n    sum+count+offset+value\nfn main()->int:\n  f=make(10)\n  "
         "f(0)\n",
         "fn make(base:int)->fn(int)->int:\n  fn(value):\n    adjusted=base+1\n    "
         "adjusted/value\nfn probe(input:int)->int:\n  f=make(41)\n  f(input)\nfn main()->int:\n  "
         "probe(1)\n",
         "type Row:\n  text:String\n  values:[int]\nfn make(row:Row)->fn(int)->Row:\n  "
         "fn(value):\n    words=row.text+\"!\"\n    numbers=concat(row.values,[value])\n    Row "
         "{text:words,values:numbers}\nfn worker(value:int)->Row:\n  f=make(Row "
         "{text:\"é\"+\"🙂\",values:[10,20]})\n  f(value)\nfn run()->int ! TaskError:\n  try "
         "parallel:\n    task=spawn worker(9)\n    first=try await task\n    second=try await "
         "task\n    first.values[0]+first.values[1]+second.values[2]+length(second.text)\nfn "
         "main()->int ! TaskError:\n  try run()\n",
         "error E:\n  Bad:\n    value:int\nfn make(offset:int)->fn(int)->int ! E:\n  fn(value):\n  "
         "  if value==0:\n      reason=offset*2\n      fail E.Bad {value:reason}\n    "
         "divisor=value-value\n    offset/divisor\nfn main()->int:\n  f=make(21)\n  f(0) catch "
         "fn(error):\n    match error:\n      E.Bad {value}:value\n",
         "fn main()->int:\n  f=fn(value:int)->int:\n    first=value+1\n    first*2\n  f(20)\n",
         "fn main()->int:\n  prefix:String=\"é\"\n  f=fn(value:String)->String:\n    "
         "text=prefix+value\n    text+\"!\"\n  length(f(\"🙂\"))+39\n"}) {
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
    for (const auto *accepted :
         {"fn make(value:int)->fn(int)->int:\n  targets=[fn(item:int):item+value]\n  fn(item):\n   "
          " result=targets[0](item)\n    result\nfn main()->int ! TaskError:\n  try parallel:\n    "
          "f=try await spawn make(21)\n    f(21)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    for (const auto *source :
         {"fn main()->int:\n  f=fn(value:int):\n    value=value+1\n    value\n  f(41)\n",
          "fn main()->int:\n  f=fn(value:int):\n    adjusted=value+1\n    adjusted=adjusted+1\n    "
          "adjusted\n  f(40)\n",
          "fn main()->int:\n  f=fn(value:int):\n    hidden=value+1\n    hidden\n  hidden\n",
          "fn main()->int:\n  f=fn(value):\n    adjusted=value+1\n    adjusted\n  f(41)\n",
          "fn main()->int:\n  f=fn(value:int):\n    adjusted:String=value\n    length(adjusted)\n  "
          "f(41)\n",
          "fn main()->int:\n  f=fn(value:int)->String:\n    adjusted=value+1\n    adjusted\n  0\n",
          "error E:\n  Bad\nfn checked(value:int)->int ! E:\n  fail E.Bad\nfn main()->int:\n  "
          "f=fn(value:int):\n    ignored=checked(value)\n    value\n  f(42)\n",
          "error E:\n  Bad\nfn main()->int:\n  f=fn(value:int)->int:\n    if value==0:fail E.Bad\n "
          "   value\n  f(42)\n",
          "error E:\n  Bad\nfn make()->fn(int)->int ! E:\n  fn(value):\n    if value==0:fail "
          "E.Bad\nfn main()->int:\n  0\n",

          "error E:\n  Bad\nfn checked()->int ! E:\n  fail E.Bad\nfn main()->int:\n  "
          "outcome=checked()\n  f=fn(value:int):\n    result=outcome\n    result\n  0\n",
          "fn main()->int:\n  f=fn(value:int):\n    adjusted=value+1\n  42\n",
          "fn main()->int:\n  f=fn(value:int):\n  42\n",
          "fn main()->int:\n  f=fn(value:int):\n    if value>0:value\n    value\n  f(42)\n",
          "fn main()->[int]:\n  map([1],fn(value):\n    adjusted=value+1\n    adjusted)\n"})
      require(!analyze(source).ok(), "invalid block closure accepted");
    auto tree = parse(
        std::make_shared<SourceFile>("tree.k", "fn main()->int:\n  f=fn(value:int):\n    "
                                               "adjusted:int=value+1\n    adjusted*2\n  f(20)\n"));
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
    unsigned diagnosticCase = 0;
    for (auto text :
         {"fn main()->int:\n  f=fn(value:int):\n    adjusted=value+1\n  0\n",
          "fn main()->int:\n  f=fn(value:int):\n    if value>0:value\n    value\n  0\n"}) {
      auto invalid = parse(std::make_shared<SourceFile>("invalid.k", text));
      require(!invalid.ok(), "invalid block syntax accepted");
      const auto expected = diagnosticCase++ == 0 ? "P007" : "P035";
      require(std::ranges::any_of(invalid.diagnostics,
                                  [&](const auto &d) { return d.code == expected; }),
              "block parsing replaced a required diagnostic");
    }
    SyntaxLimits limits;
    limits.maxDepth = 8;
    std::string deep = "fn main()->int:\n";
    for (unsigned i = 0; i < 24; ++i)
      deep += std::string(2 + i * 2, ' ') + "fn():\n";
    deep += std::string(50, ' ') + "42\n";
    auto limited = parse(std::make_shared<SourceFile>("deep.k", deep), limits);
    require(!limited.ok(), "block closure nesting bypassed syntax budget");
    limits = {};
    limits.maxNodes = 8;
    std::string large = "fn main()->int:\n  f=fn(value:int):\n";
    for (unsigned i = 0; i < 32; ++i)
      large += "    v" + std::to_string(i) + "=value+1\n";
    large += "    value\n  f(42)\n";
    require(!parse(std::make_shared<SourceFile>("large.k", large), limits).ok(),
            "block body bypassed expression node budget");
    std::string crlf = "fn main()->int:\r\n  f=fn(value:int):\r\n    adjusted=value+1\r\n    "
                       "adjusted*2\r\n  f(20)";
    require(analyze(crlf.c_str()).ok(), "CRLF or EOF block termination changed");
    auto mapTree = parse(std::make_shared<SourceFile>(
        "map.k",
        "error E:\n  Bad\nfn checked()->int ! E:\n  fail E.Bad\nfn main()->int:\n  "
        "f=fn(value:int):\n    ignored=checked()\n    value\n  result=map([42],f)\n  result[0]\n"));
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
