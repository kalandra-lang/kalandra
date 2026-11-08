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
  return check(parse(std::make_shared<SourceFile>("generate.k", text)));
}
static void valid(const std::string &text) {
  auto typed = analyze(text);
  std::string details;
  for (const auto &diagnostic : typed.diagnostics)
    details += diagnostic.message + "\n";
  require(typed.ok(), "generator rejected: " + text + details);
  require(bool(continuation::plan(*typed.module)), "generator continuation import failed");
#ifndef KALANDRA_GENERATE_FRONTEND_ONLY
  auto lowered = lowerNative(*typed.module);
  require(bool(lowered), "generator lowering failed: " + (lowered ? "" : lowered.error()));
#endif
}
int main() {
  try {
    for (auto type : {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
                      "f32", "f64"}) {
      const auto convert = std::string(type);
      valid("fn make(n:i64):\n  bias=3\n  return generate(n,fn(i): return " + convert +
            "(i%17+bias))\nfn main()->i64:\n  return length(make(37))\n");
      valid("fn cell(i:i64)->" + convert + ":\n  return " + convert +
            "(i%17)\nfn main()->i64:\n  return length(12 -> generate(cell))\n");
      valid("fn main()->i64:\n  offset=3\n  callback=fn(i:i64): return " + convert +
            "(i+offset)\n  return length(generate(0,callback))\n");
    }
    for (auto body : {"return generate(4,fn(i): return i*2)", "return generate(4,fn(i:i64)->bool: return i<2)",
                      "return generate(3,fn(i): return \"value\")", "return generate(3,fn(i): return Item {value:i})",
                      "return generate(3,fn(i): return fn(x:i64): return x+i)", "return generate(0,fn(i): return 1/i)",
                      "return generate(3,fn(i): return length(generate(i,fn(j): return i+j)))",
                      "callback=make(2)\n  return generate(4,callback)", "return generate(3,callbacks[0])"})
      valid("type Item:\n  value:i64\nfn make(offset:i64)->fn(i64)->i64:\n  return fn(i:i64): return i+offset\n"
            "fn identity(i:i64)->i64:\n  return i\nfn run():\n  callbacks=[identity]\n  " +
            std::string(body) + "\nfn main()->i64:\n  return 0\n");
    valid("fn run(n:i64)->[i64]:\n  return generate(n,fn(i): return i*3)\n"
          "fn main()->i64 ! TaskError:\n  return try parallel:\n    pending=spawn run(5)\n"
          "    values=try await pending\n    values[4]\n");
    valid(
        "fn main()->i64 ! TaskError:\n  return try parallel:\n"
        "    pending=spawn generate(5,fn(i): return i*3)\n    values=try await pending\n    values[4]\n");
    for (auto body :
         {"return generate(-1,fn(i): return i)", "return generate(1.0,fn(i): return i)", "return generate(uint32(2),fn(i): return i)",
          "return generate(true,fn(i): return i)", "return generate([1],fn(i): return i)", "return generate(2,fn(): return 0)",
          "return generate(2,fn(i,j): return i+j)", "return generate(2,fn(i:i32): return i)", "return generate(2,fn(i:bool): return i)",
          "return generate(2,fn(i): return [i])", "return generate(2,fn(i): return missing)", "return generate(2)",
          "return generate(2,fn(i): return i,fn(i): return i)", "return generate(2,fn(i)->i64 ! TaskError: return i)",
          "return generate(2,fn(i): return spawn identity(i))"})
      require(
          !analyze("fn identity(i:i64)->i64:\n  return i\nfn main():\n  return " + std::string(body) + "\n").ok(),
          "invalid generator accepted: " + std::string(body));
    require(!analyze("fn identity(i:i64)->i64:\n  return i\n"
                     "fn suspend(i:i64)->i64 ! TaskError:\n  return try parallel:\n"
                     "    try await spawn identity(i)\nfn main()->i64:\n"
                     "  return length(generate(3,fn(i): return suspend(i) catch fn(e): return 0))\n")
                 .ok(),
            "transitively suspending generator callback accepted");
    valid("fn main()->[i64]:\n  values=[1,2,3]\n  return copy(values)\n");
    valid("fn main()->[i64]:\n  return copy([])\n");
    valid("fn main()->[bool] ! TaskError:\n  return try parallel:\n"
          "    try await spawn copy([true,false])\n");
    valid("fn main()->[String]:\n  return copy([\"hello\",\"world\"])\n");
    for (auto body : {"copy(1)", "copy(\"hello\")", "copy(tensor([1,2]))", "copy()",
                      "copy([1],[2])", "copy([Item {value:1}])", "copy([fn(i:i64): i])"})
      require(!analyze("type Item:\n  value:i64\nfn main():\n  return " + std::string(body) + "\n").ok(),
              "unsupported copy accepted");
    auto typed = analyze("fn main()->i64:\n  return length(generate(4,fn(i): return i+1))\n");
    require(typed.ok(), "generator metadata fixture failed");
    ValueId generator = 0;
    for (ValueId i = 0; i < typed.module->expressions.size(); ++i)
      if (auto *map = std::get_if<ArrayMap>(&typed.module->expressions[i].operation);
          map && map->generate)
        generator = i;
    require(generator && typed.module->expressions[generator].type.shape[0].constant == 4,
            "constant generator count not retained");
    require(dumpTypedIR(*typed.module).find("array.generate sequential") != std::string::npos,
            "typed dump does not distinguish generator from map");
#ifndef KALANDRA_GENERATE_FRONTEND_ONLY
    for (unsigned fault = 0; fault < 7; ++fault) {
      auto forged = *typed.module;
      auto &expr = forged.expressions[generator];
      auto &map = std::get<ArrayMap>(expr.operation);
      if (fault == 0)
        map.parallel = true;
      if (fault == 1)
        map.function = SIZE_MAX;
      if (fault == 2)
        map.input = SIZE_MAX;
      if (fault == 3)
        forged.expressions[map.input].type.element = NumericType::Int32;
      if (fault == 4)
        expr.type.shape.clear();
      if (fault == 5)
        map.captures.push_back(SIZE_MAX);
      if (fault == 6)
        expr.type.element = NumericType::Float64;
      require(!lowerNative(forged), "forged generator metadata accepted");
    }
#endif
    std::cout
        << "Array generation: ten numeric types, named/anonymous/indirect callbacks, captures, "
           "owned elements, worker import and rejection gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
