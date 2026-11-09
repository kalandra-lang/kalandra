#include "kalandra/Backend.h"
#include "kalandra/StringLiteral.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("string.k", text)));
}
int main() {
  try {
    for (const auto *source : {"fn main()->String:\n  return \"\"\n",
                               "fn main():\n  text:String=\"hello\"\n  return text\n",
                               "fn pass(x:String)->String:\n  return x\nfn main()->String:\n  "
                               "return pass(\"سلام\\0👩\\u{200d}💻\")\n",
                               "fn choose(flag:bool,a:String,b:String)->String:\n  return if "
                               "flag:\n    a\n  else:\n    b\nfn "
                               "main()->String:\n  return choose(true,\"é\",\"é\")\n",
                               "fn main()->bool:\n  ignored=\"hello\"\n  return true\n",
                               "fn main()->[i64]:\n  ignored=\"hello\"\n  return [1,2]\n"}) {
      auto result = analyze(source);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid String source rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
      require(bool(lowerNative(*result.module)), "String lowering failed");
#endif
      require(bool(numericCapabilityError(*result.module, NumericTarget::AppleGpu)),
              "GPU String admitted");
    }
    for (const auto *source : {
             "fn main()->String:\n  return 1\n",
             "fn main()->i64:\n  return \"1\"\n",
             "fn f(x:String)->String:\n  return x\nfn main()->String:\n  return f(true)\n",
         })
      require(!analyze(source).ok(), "unsupported String source admitted");
    for (const auto *source :
         {"fn f(x:String)->String:\n  return x\nfn main()->String ! TaskError:\n  return try "
          "parallel:\n    try "
          "await spawn f(\"é\")\n",
          "fn f()->String ! TaskError:\n  return \"ok\"\nfn relay()->String ! TaskError:\n  return "
          "f() catch "
          "fn(error): fail error\nfn main()->String ! TaskError:\n  return try relay()\n",
          "fn work(text:String)->[i64]:\n  return map([1,2],fn(x): return length(text)+x)\nfn "
          "main()->[i64] "
          "! TaskError:\n  return try parallel:\n    try await spawn work(\"é\")\n"}) {
      auto value = analyze(source);
      require(value.ok(), "String workers/captures/rethrow rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
      require(bool(lowerNative(*value.module)), "String worker lowering failed");
#endif
    }
    for (const auto *source : {"fn main()->String:\n  return \"a\"+\"b\"\n",
                               "fn eq(a:String,b:String)->bool:\n  return a==b\nfn main()->bool:\n "
                               " return eq(\"é\",\"e\\u{301}\")\n",
                               "fn main()->bool:\n  return \"a\"!=\"b\" and \"x\"+\"y\"==\"xy\"\n",
                               "fn same(x:i64)->bool:\n  return \"a\"+\"b\"==\"ab\"\nfn main():\n  "
                               "return [1,2]->map(same)\n"}) {
      auto value = analyze(source);
      if (!value.ok())
        for (const auto &d : value.diagnostics)
          std::cerr << renderDiagnostic(*value.source, d);
      require(value.ok(), "valid String operation rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
      require(bool(lowerNative(*value.module)), "String operation lowering failed");
#endif
    }
    for (const auto *source :
         {"fn main():\n  return \"a\"+1\n", "fn main():\n  return 1+\"a\"\n",
          "fn main():\n  return \"a\"-\"b\"\n", "fn main():\n  return \"a\"<\"b\"\n",
          "fn main():\n  return \"a\"==true\n", "fn main():\n  return \"a\"!=1\n",
          "fn task()->i64 ! TaskError:\n  fail 1\nfn main()->bool:\n  return task() catch "
          "fn(error): return "
          "error.message==error.message\n"})
      require(!analyze(source).ok(), "invalid or borrowed-diagnostic String operation admitted");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto value = analyze("fn main():\n  return \"a\"+\"b\"\n");
      for (auto &expr : value.module->expressions)
        if (auto *concat = std::get_if<StringConcat>(&expr.operation)) {
          if (mode == 0)
            expr.type.builtin = BuiltinValue::Bool;
          if (mode == 1)
            concat->left = value.module->expressions.size();
          if (mode == 2)
            value.module->expressions[concat->left].type.builtin = BuiltinValue::Numeric;
          if (mode == 3)
            expr.type.task = true;
        }
      require(!lowerNative(*value.module), "forged String concat admitted");
    }
    auto order = analyze("fn main():\n  return \"a\"==\"b\"\n");
    for (auto &expr : order.module->expressions)
      if (auto *cmp = std::get_if<ScalarCompare>(&expr.operation))
        cmp->operation = ComparisonKind::Less;
    require(!lowerNative(*order.module), "forged String ordering admitted");
#endif
    auto diagnostic = analyze(
        "fn task()->bool ! TaskError:\n  return true\nfn main()->bool:\n  return task() catch "
        "fn(error):\n    return "
        "match error:\n      TaskError.Fault { message }: message==message\n      _: false\n");
    require(diagnostic.ok(), "owned diagnostic equality rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    require(bool(lowerNative(*diagnostic.module)), "owned diagnostic equality lowering failed");
#endif
    for (const auto *source :
         {"fn size(value:String)->i64:\n  return length(value)+byteLength(value)\nfn main():\n  "
          "return size(\"👩‍💻\")\n",
          "fn main():\n  return \"سلام\" -> length\n",
          "fn main():\n  return \"سلام\" -> byteLength\n",
          "fn count(value:i64)->i64:\n  return value+length(\"é\")\nfn main():\n  return "
          "[1,2]->map(count)\n"}) {
      auto value = analyze(source);
      require(value.ok(), "valid String length rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
      require(bool(lowerNative(*value.module)), "String length lowering failed");
#endif
    }
    for (const auto *source :
         {"fn main():\n  return length()\n", "fn main():\n  return length(\"a\",\"b\")\n",
          "fn main():\n  return byteLength()\n", "fn main():\n  return byteLength(\"a\",\"b\")\n",
          "fn main():\n  return byteLength([1,2])\n", "fn main():\n  return byteLength(3)\n",
          "fn main():\n  return byteLength(true)\n",
          "fn main():\n  return byteLength(tensor([1,2]))\n", "fn main():\n  return shape(\"a\")\n",
          "fn main()->i32:\n  return length(\"a\")\n",
          "fn main()->bool:\n  return byteLength(\"a\")\n",
          "fn main() ! TaskError:\n  return try parallel:\n    try await spawn length(\"a\")\n"})
      require(!analyze(source).ok(), "invalid String length type, arity or worker admitted");
    for (const auto *query : {"length", "byteLength"}) {
      auto value =
          analyze("fn task()->i64 ! TaskError:\n  return 1\nfn main()->i64:\n  return task() catch "
                  "fn(error):\n    return match error:\n      TaskError.Fault { message }: " +
                  std::string(query) + "(message)\n      _: 0\n");
      require(value.ok(), "owned diagnostic String length rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
      require(bool(lowerNative(*value.module)), "diagnostic String length lowering failed");
#endif
    }
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    for (unsigned mode = 0; mode < 6; ++mode) {
      auto value = analyze("fn main():\n  return length(\"é\")\n");
      for (auto &expr : value.module->expressions)
        if (auto *q = std::get_if<StringQuery>(&expr.operation)) {
          if (mode == 0)
            q->input = value.module->expressions.size();
          if (mode == 1)
            q->kind = static_cast<StringQueryKind>(99);
          if (mode == 2)
            expr.type.element = NumericType::UInt64;
          if (mode == 3)
            expr.type.builtin = BuiltinValue::Bool;
          if (mode == 4)
            expr.type.tensor = true;
          if (mode == 5)
            value.module->expressions[q->input].type.task = true;
        }
      require(!lowerNative(*value.module), "forged String length admitted");
    }
#endif
    for (const auto *source : {"fn main()->String:\n  return \"👩‍💻\"[1]\n",
                               "fn pick(text:String,index:i32)->String:\n  return text[index]\nfn "
                               "main():\n  return pick(\"é\",0)\n",
                               "fn main():\n  letter=\"é\"[0]\n  return letter[0]+\"!\"\n"}) {
      auto value = analyze(source);
      require(value.ok(), "valid String index rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
      require(bool(lowerNative(*value.module)), "String index lowering failed");
#endif
    }
    for (const auto *source :
         {"fn main():\n  return \"é\"[1]\n", "fn main():\n  return \"\"[0]\n",
          "fn main():\n  return \"a\"[-1]\n", "fn main():\n  text=\"é\"\n  return text[1]\n",
          "fn main():\n  return (\"é\"+\"a\")[2]\n",
          "fn main()->String:\n  return if true:\n    text=\"\"\n    text[0]\n  else:\n    "
          "\"ok\"\n",
          "fn main():\n  return \"é\"[0][1]\n",
          "fn main():\n  position:u64=18446744073709551615\n  return \"a\"[position]\n"}) {
      auto value = analyze(source);
      bool bounds = false;
      for (const auto &d : value.diagnostics)
        bounds |= d.code == "S062";
      require(!value.ok() && bounds, "known bad String index did not produce S062");
    }
    for (const auto *source :
         {"fn main():\n  return \"a\"[true]\n", "fn main():\n  return \"a\"[1.0]\n",
          "fn main():\n  return \"a\"[0,0]\n", "fn main():\n  return \"a\"[]\n",
          "fn main():\n  return \"a\"[\"0\"]\n", "fn main():\n  return \"a\"[[0]]\n",
          "fn main()->i64:\n  return \"a\"[0]\n"})
      require(!analyze(source).ok(), "invalid String index type/arity admitted");
    auto indexDiagnostic =
        analyze("fn task()->i64 ! TaskError:\n  return 1\nfn main()->i64:\n  return task() catch "
                "fn(error):\n    return match error:\n      TaskError.Fault { message }:\n        "
                "first=message[0]\n        1\n      _: 0\n");
    require(indexDiagnostic.ok(), "owned diagnostic String index rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    require(bool(lowerNative(*indexDiagnostic.module)), "diagnostic String index lowering failed");
#endif
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    for (unsigned mode = 0; mode < 6; ++mode) {
      auto value = analyze("fn main():\n  return \"é\"[0]\n");
      for (auto &expr : value.module->expressions)
        if (auto *read = std::get_if<StringIndex>(&expr.operation)) {
          if (mode == 0)
            read->input = value.module->expressions.size();
          if (mode == 1)
            read->index = value.module->expressions.size();
          if (mode == 2)
            expr.type.builtin = BuiltinValue::Bool;
          if (mode == 3)
            value.module->expressions[read->index].type.element = NumericType::Float64;
          if (mode == 4)
            value.module->expressions[read->index].type.task = true;
          if (mode == 5)
            expr.type.task = true;
        }
      require(!lowerNative(*value.module), "forged String index admitted");
    }
#endif
    for (const auto *source : {"fn main()->String:\n  return slice(\"aé👩\",1,3)\n",
                               "fn main()->String:\n  return slice(\"\",0,0)\n",
                               "fn main():\n  return \"aé\"->slice(0,2)\n",
                               "fn main()->String:\n  return slice(slice(\"aé👩\",1,3),0,1)\n"}) {
      auto value = analyze(source);
      require(value.ok(), "valid String slice rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
      require(bool(lowerNative(*value.module)), "String slice lowering failed");
#endif
    }
    for (const auto *source :
         {"fn main():\n  return slice(\"abc\",2,1)\n", "fn main():\n  return slice(\"é\",0,2)\n",
          "fn main():\n  return slice(\"abc\",-1,2)\n",
          "fn main():\n  return slice(\"abc\",0,-1)\n", "fn main():\n  return slice(\"\",1,1)\n",
          "fn main():\n  return slice(\"a\",0,0)[0]\n",
          "fn main():\n  return slice(\"éa\",0,1)[1]\n",
          "fn main():\n  bound:u64=18446744073709551615\n  return slice(\"a\",0,bound)\n",
          "fn main()->String:\n  return if true:\n    text=slice(\"a\",0,0)\n    text[0]\n  "
          "else:\n    "
          "\"ok\"\n"}) {
      auto value = analyze(source);
      bool bounds = false;
      for (const auto &d : value.diagnostics)
        bounds |= d.code == "S062";
      require(!value.ok() && bounds, "known invalid slice did not produce S062");
    }
    for (const auto *source :
         {"fn main():\n  return slice()\n", "fn main():\n  return slice(\"a\",0)\n",
          "fn main():\n  return slice(\"a\",0,1,1)\n",
          "fn main():\n  return slice(tensor([1,2]),0,1)\n", "fn main():\n  return slice(1,0,1)\n",
          "fn main():\n  return slice(\"a\",true,1)\n", "fn main():\n  return slice(\"a\",0,1.0)\n",
          "fn main():\n  return slice(\"a\",[0],1)\n",
          "fn main()->i64:\n  return slice(\"a\",0,1)\n"})
      require(!analyze(source).ok(), "invalid slice type/arity/result admitted");
    auto sliceDiagnostic =
        analyze("fn task()->i64 ! TaskError:\n  return 1\nfn main()->i64:\n  return task() catch "
                "fn(error):\n    return match error:\n      TaskError.Fault { message }:\n        "
                "part=slice(message,0,0)\n        1\n      _: 0\n");
    require(sliceDiagnostic.ok(), "owned diagnostic String slice rejected");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    require(bool(lowerNative(*sliceDiagnostic.module)), "diagnostic String slice lowering failed");
#endif
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    for (unsigned mode = 0; mode < 8; ++mode) {
      auto value = analyze("fn main():\n  return slice(\"éa\",0,1)\n");
      for (auto &expr : value.module->expressions)
        if (auto *part = std::get_if<StringSlice>(&expr.operation)) {
          if (mode == 0)
            part->input = value.module->expressions.size();
          if (mode == 1)
            part->start = value.module->expressions.size();
          if (mode == 2)
            part->end = value.module->expressions.size();
          if (mode == 3)
            expr.type.builtin = BuiltinValue::Bool;
          if (mode == 4)
            value.module->expressions[part->start].type.element = NumericType::Float64;
          if (mode == 5)
            value.module->expressions[part->end].type.builtin = BuiltinValue::Bool;
          if (mode == 6)
            expr.type.task = true;
          if (mode == 7)
            value.module->expressions[part->input].type.task = true;
        }
      require(!lowerNative(*value.module), "forged String slice admitted");
    }
#endif
    require(!analyze("fn String()->i64:\n  return 1\n").ok(), "reserved String name admitted");
    require(!analyze("fn main()->String:\n  \"" + std::string(4097, 'a') + "\"\n").ok(),
            "oversized literal admitted");
#ifndef KALANDRA_STRING_FRONTEND_ONLY
    for (unsigned mode = 0; mode < 5; ++mode) {
      auto value = analyze("fn main()->String:\n  return \"hello\"\n");
      auto &expr = value.module->expressions.front();
      if (mode == 0)
        expr.type.builtin = BuiltinValue::Bool;
      if (mode == 1)
        std::get<StringLiteral>(expr.operation).value = std::string(1, char(0x80));
      if (mode == 2)
        expr.type.tensor = true;
      if (mode == 3)
        expr.type.task = true;
      if (mode == 4)
        expr.type.device = 0;
      require(!lowerNative(*value.module), "forged String IR admitted");
    }
#endif
    std::cout << "String source typing, helpers, conditionals, unsupported operations and IR gates "
                 "passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
