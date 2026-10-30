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
  return check(parse(std::make_shared<SourceFile>("record-closures.k", text)));
}
int main() {
  try {
    for (auto element : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      const auto type = std::string(element);
      const auto declarations = "type Config:\n  inner:Inner\n  offset:" + type +
                                "\ntype Inner:\n  values:[" + type + "]\n  bias:Tensor[" + type +
                                ",2]\n  enabled:Bool\n";
      for (auto expression :
           {"map(input,fn(x):x+config.offset+config.offset)", "input -> map(fn(x):x+config.offset)",
            "map(input,fn(x):x+config.inner.values[0]+config.inner.bias[1])",
            "map(input,fn(config):config+1)",
            "map(input,fn(x):keep(map(input,fn(y):x+y+config.offset),x))"}) {
        auto checked = analyze(declarations + "fn keep(values:[" + type + "],value:" + type +
                               ")->" + type + ":\n  value\nfn mapped(input:[" + type +
                               "],config:Config):\n  " + expression + "\nfn main()->int:\n  0\n");
        require(checked.ok(), "valid record closure rejected: " + std::string(expression));
        require(!numericCapabilityError(*checked.module, NumericTarget::Cpu),
                "CPU capture validator rejected record");
        require(bool(continuation::plan(*checked.module)), "record callback import failed");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
        auto native = lowerNative(*checked.module);
        require(bool(native), native ? "" : native.error());
#endif
      }
    }
    auto captured = analyze(
        "type First:\n  x:int\ntype Second:\n  x:int\nfn mapped(values:[int],config:First):\n  "
        "map(values,fn(x):x+config.x+config.x)\nfn main()->int:\n  0\n");
    require(captured.ok(), "nominal fixture rejected");
    const auto &map = std::get<ArrayMap>(
        captured.module->expressions[captured.module->functions[0].result].operation);
    require(map.captures.size() == 1, "repeated record binding captured more than once");
    const auto parameter = captured.module->functions[map.function].parameters[1];
    require(captured.module->symbols[parameter].type.record == 0,
            "capture lost declaration identity");
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto forged = *captured.module;
      auto &actual = forged.expressions[map.captures[0]].type;
      auto &formal = forged.symbols[parameter].type;
      switch (mutation) {
      case 0:
        formal.record = 1;
        break; // Identical physical layout is still a different type.
      case 1:
        actual.record = 1;
        forged.symbols[std::get<SymbolRead>(forged.expressions[map.captures[0]].operation).symbol]
            .type.record = 1;
        break;
      case 2:
        formal.record.reset();
        break;
      case 3:
        formal.record = SIZE_MAX;
        break;
      case 4:
        actual.task = true;
        break;
      case 5:
        actual.fallible = true;
        break;
      case 6:
        formal.device = 0;
        break;
      case 7:
        actual.tensor = true;
        break;
      }
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
              "forged record capture accepted");
    }
    for (auto body : {"work=spawn make()\n    map([1],fn(x):work.x+x)",
                      "pending=make()\n    map([1],fn(x):pending.x+x)"}) {
      auto rejected = analyze("type Config:\n  x:int\nfn make()->Config ! TaskError:\n  Config "
                              "{x:2}\nfn main() ! TaskError:\n  try parallel:\n    " +
                              std::string(body) + "\n");
      require(!rejected.ok(), "task/fallible record captured before completion");
    }
    auto returned = analyze(
        "type Config:\n  x:int\nfn mapped():\n  config=Config {x:2}\n"
        "  map([1],fn(x):config)\nfn main():\n  0\n");
    require(returned.ok() && !numericCapabilityError(*returned.module, NumericTarget::Cpu),
            "captured record-valued map rejected");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
    require(bool(lowerNative(*returned.module)), "captured record map lowering failed");
#endif
    std::cout << "record closures: six numeric types, nested storage, transitive captures, "
                 "shadowing, deduplication and nominal/unfinished-value guards passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
