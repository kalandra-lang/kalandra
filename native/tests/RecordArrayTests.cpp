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
  return check(parse(std::make_shared<SourceFile>("record-arrays.k", text)));
}
int main() {
  try {
    for (auto name : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      const auto t = std::string(name);
      auto result =
          analyze("type Reading:\n  data:Data\n  enabled:Bool\ntype Data:\n  amount:" + t +
                  "\n  values:[" + t + "]\n  matrix:Tensor[" + t +
                  ",?,?]\nfn choose(flag:Bool,rows:[Reading])->[Reading]:\n  if flag: rows\n  "
                  "else: []\nfn total(rows:[Reading])->" +
                  t + ":\n  rows[0].data.amount+rows[0].data.values[0]\nfn main()->" + t +
                  " ! TaskError:\n  rows=[Reading {data:Data "
                  "{amount:2,values:[3],matrix:tensor([[1,2]])},enabled:true}]\n  result=try "
                  "parallel:\n    try await spawn choose(true,rows)\n  total(result)\n");
      require(result.ok(), "record array rejected: " + t);
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "record array capability gate rejected");
      require(bool(continuation::plan(*result.module)), "record array planning failed");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    auto valid =
        analyze("type Point:\n  x:int\ntype Other:\n  x:int\nfn make()->[Point]:\n  [Point "
                "{x:2},Point {x:3}]\nfn main():\n  rows=make()\n  length(rows)+rows[0].x\n");
    require(valid.ok(), "record array metadata fixture failed");
    require(describeType(valid.module->functions[0].resultType) == "[Point]",
            "record array type display lost category");
    const auto literal = valid.module->functions[0].result;
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
      auto forged = *valid.module;
      auto &value = forged.expressions[literal];
      auto &op = std::get<TensorLiteral>(value.operation);
      switch (mutation) {
      case 0:
        value.type.record = 1;
        break;
      case 1:
        value.type.record.reset();
        break;
      case 2:
        value.type.shape.clear();
        break;
      case 3:
        value.type.tensor = false;
        break;
      case 4:
        value.type.shape[0].constant = 3;
        break;
      case 5:
        op.elements[0] = SIZE_MAX;
        break;
      case 6:
        forged.expressions[op.elements[0]].type.record = 1;
        break;
      case 7:
        value.type.device = 0;
        break;
      case 8:
        value.type.array = false;
        break;
      }
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
              "forged record array admitted");
    }
    const std::string mapDecl = "type Point:\n  x:int\n  data:[int]\ntype Other:\n  x:int\n"
                                "fn extract(p:Point)->int:\n  p.x\n"
                                "fn wrap(x:int)->Point:\n  Point {x:x,data:[x]}\n"
                                "fn move(p:Point)->Point:\n  p with {x:p.x+1}\n";
    for (auto body :
         {"map([Point {x:1,data:[2]}],extract)", "map([Point {x:1,data:[2]}],fn(p):p.x)",
          "map([1,2],wrap)", "map([1,2],fn(x):Point {x:x,data:[x]})", "map([],move)",
          "map([],fn(p:Point)->Point:p)", "map([Point {x:1,data:[2]}],fn(p):Other {x:p.x})",
          "offset=2\n  map([Point {x:1,data:[2]}],fn(p):p with {x:p.x+offset})"}) {
      auto mapped = analyze(mapDecl + "fn mapped():\n  " + body + "\nfn main():\n  0\n");
      require(mapped.ok(), "record map frontend rejected: " + std::string(body));
      require(!numericCapabilityError(*mapped.module, NumericTarget::Cpu),
              "record map capability rejected");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      auto lowered = lowerNative(*mapped.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto body :
         {"map([Other {x:1}],move)", "map([1],move)", "map([Point {x:1,data:[2]}],fn(p:Other):p.x)",
          "map([Point {x:1,data:[2]}],fn(p):p.data)", "map([],fn(p):p.x)"})
      require(!analyze(mapDecl + "fn main():\n  " + body + "\n").ok(),
              "invalid record map accepted");
    auto mapped =
        analyze(mapDecl + "fn mapped(rows:[Point]):\n  map(rows,move)\nfn main():\n  0\n");
    require(mapped.ok(), "record map mutation fixture rejected");
    ValueId mapId = 0;
    for (const auto &e : mapped.module->expressions)
      if (std::holds_alternative<ArrayMap>(e.operation))
        mapId = &e - mapped.module->expressions.data();
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
      auto forged = *mapped.module;
      auto &e = forged.expressions[mapId];
      auto &op = std::get<ArrayMap>(e.operation);
      if (mutation == 0)
        e.type.record = 1;
      if (mutation == 1)
        forged.expressions[op.input].type.record = 1;
      if (mutation == 2)
        e.type.shape[0].identity += 100;
      if (mutation == 3)
        e.type.array = false;
      if (mutation == 4)
        forged.functions[op.function].resultType.task = true;
      if (mutation == 5)
        forged.symbols[forged.functions[op.function].parameters[0]].type.tensor = true;
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
              "forged record map admitted: " + std::to_string(mutation));
    }
    const auto decl = std::string("type Point:\n  x:int\ntype Other:\n  x:int\n");
    for (auto body :
         {"fn main():\n  [Point {x:1},Other {x:1}]\n", "fn main():\n  [Point {x:1},1]\n",
          "fn main():\n  [Point {x:1}][1].x\n", "fn main():\n  [Point {x:1}][-1].x\n",
          "fn main():\n  points:[Point]=[]\n  points[0].x\n",
          "fn main():\n  points=[Point {x:1}]\n  points.x\n",
          "fn main():\n  points=[Point {x:1}]\n  points with {x:2}\n",
          "fn main():\n  points=[Point {x:1}]\n  tensor(points)\n",
          "fn main():\n  points=[Point {x:1}]\n  shape(points)\n",
          "type Tree:\n  children:[Tree]\nfn main():\n  spawn Tree {children:[]}\n"})
      require(!analyze(decl + body).ok(), "unsupported record array program admitted");
    auto captures =
        analyze(decl + "fn main()->[int] ! TaskError:\n  points=[Point {x:2},Point {x:3}]\n  try "
                       "parallel:\n    try await spawn map([0,1],fn(i):points[i].x)\n");
    require(captures.ok() && bool(continuation::plan(*captures.module)),
            "record array callback capture failed");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
    require(bool(lowerNative(*captures.module)), "record array captured kernel lowering failed");
    auto display = analyze(decl + "fn main():\n  [Point {x:2}]\n");
    require(display.ok() && !lowerNative(*display.module), "record array display silently enabled");
#endif
    std::string large = "type R0:\n  x:int\n";
    for (unsigned n = 1; n <= 17; ++n)
      large += "type R" + std::to_string(n) + ":\n  left:R" + std::to_string(n - 1) +
               "\n  right:R" + std::to_string(n - 1) + "\n";
    auto limited =
        analyze(large + "fn copy(value:R17)->[R17]:\n  [value,value,value]\nfn main():\n  0\n");
    require(!limited.ok(), "expanded record array literal budget missing");
    bool budget = false;
    for (const auto &diagnostic : limited.diagnostics)
      budget |= diagnostic.message.find("expanded storage budget") != std::string::npos;
    require(budget, "large literal failed for unrelated reason");
    std::cout << "record arrays: six families, nested collection fields, length/index/branches, "
                 "captures, nominal and malformed IR guards, literal budgets passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
