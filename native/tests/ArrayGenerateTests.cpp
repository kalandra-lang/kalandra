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
    for (auto type : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64",
                      "float32", "float64"}) {
      const auto convert = std::string(type);
      valid("fn make(n:int):\n  bias=3\n  generate(n,fn(i): " + convert +
            "(i%17+bias))\nfn main()->int:\n  length(make(37))\n");
      valid("fn cell(i:int)->" + convert + ":\n  " + convert +
            "(i%17)\nfn main()->int:\n  length(12 -> generate(cell))\n");
      valid("fn main()->int:\n  offset=3\n  callback=fn(i:int): " + convert +
            "(i+offset)\n  length(generate(0,callback))\n");
    }
    for (auto body : {"generate(4,fn(i): i*2)", "generate(4,fn(i:int)->Bool: i<2)",
                      "generate(3,fn(i): \"value\")", "generate(3,fn(i): Item {value:i})",
                      "generate(3,fn(i): fn(x:int): x+i)", "generate(0,fn(i): 1/i)",
                      "generate(3,fn(i): length(generate(i,fn(j): i+j)))",
                      "callback=make(2)\n  generate(4,callback)", "generate(3,callbacks[0])"})
      valid("type Item:\n  value:int\nfn make(offset:int)->fn(int)->int:\n  fn(i:int): i+offset\n"
            "fn identity(i:int)->int:\n  i\nfn run():\n  callbacks=[identity]\n  " +
            std::string(body) + "\nfn main()->int:\n  0\n");
    valid("fn run(n:int)->[int]:\n  generate(n,fn(i): i*3)\n"
          "fn main()->int ! TaskError:\n  try parallel:\n    pending=spawn run(5)\n"
          "    values=try await pending\n    values[4]\n");
    valid(
        "fn main()->int ! TaskError:\n  try parallel:\n"
        "    pending=spawn generate(5,fn(i): i*3)\n    values=try await pending\n    values[4]\n");
    for (auto body :
         {"generate(-1,fn(i): i)", "generate(1.0,fn(i): i)", "generate(uint32(2),fn(i): i)",
          "generate(true,fn(i): i)", "generate([1],fn(i): i)", "generate(2,fn(): 0)",
          "generate(2,fn(i,j): i+j)", "generate(2,fn(i:int32): i)", "generate(2,fn(i:Bool): i)",
          "generate(2,fn(i): [i])", "generate(2,fn(i): missing)", "generate(2)",
          "generate(2,fn(i): i,fn(i): i)", "generate(2,fn(i)->int ! TaskError: i)",
          "generate(2,fn(i): spawn identity(i))"})
      require(
          !analyze("fn identity(i:int)->int:\n  i\nfn main():\n  " + std::string(body) + "\n").ok(),
          "invalid generator accepted: " + std::string(body));
    require(!analyze("fn identity(i:int)->int:\n  i\n"
                     "fn suspend(i:int)->int ! TaskError:\n  try parallel:\n"
                     "    try await spawn identity(i)\nfn main()->int:\n"
                     "  length(generate(3,fn(i): suspend(i) catch fn(e): 0))\n")
                 .ok(),
            "transitively suspending generator callback accepted");
    valid("fn main()->[int]:\n  values=[1,2,3]\n  copy(values)\n");
    valid("fn main()->[int]:\n  copy([])\n");
    valid("fn main()->[Bool] ! TaskError:\n  try parallel:\n"
          "    try await spawn copy([true,false])\n");
    valid("fn main()->[String]:\n  copy([\"hello\",\"world\"])\n");
    for (auto body : {"copy(1)", "copy(\"hello\")", "copy(tensor([1,2]))", "copy()",
                      "copy([1],[2])", "copy([Item {value:1}])", "copy([fn(i:int): i])"})
      require(!analyze("type Item:\n  value:int\nfn main():\n  " + std::string(body) + "\n").ok(),
              "unsupported copy accepted");
    auto typed = analyze("fn main()->int:\n  length(generate(4,fn(i): i+1))\n");
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
