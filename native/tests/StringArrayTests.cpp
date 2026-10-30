#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("string-arrays.k", source)));
}
int main() {
  try {
    for (const auto *source :
         {"fn main()->[String]:\n  []\n", "fn main():\n  [\"é\",\"\",\"👩\\0\"]\n",
          "fn append(x:String)->String:\n  x+\"!\"\nfn main()->[String]:\n  map([],append)\n",
          "fn main():\n  map([1,2],fn(x):\"a\")\n",
          "fn main():\n  values=[\"a\",\"b\"]\n  map([0,1],fn(i):values[i])\n",
          "fn main():\n  map([\"é\",\"a\"],fn(text):length(text))\n",
          "type Row:\n  text:String\nfn main():\n  map([Row {text:\"a\"}],fn(row):row.text)\n",
          "fn work(values:[String])->[String] ! TaskError:\n  try parallel:\n    work=spawn "
          "map(values,fn(text):text+\"!\")\n    first=try await work\n    again=try await work\n   "
          " again\nfn main()->[String] ! TaskError:\n  try work([\"é\"])\n"}) {
      auto result = analyze(source);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid String array rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU String arrays gated");
      require(bool(numericCapabilityError(*result.module, NumericTarget::AppleGpu)),
              "GPU String array enabled");
      require(bool(continuation::plan(*result.module)), "String array planning failed");
#ifndef KALANDRA_STRING_ARRAY_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *source :
         {"fn main():\n  []\n", "fn main():\n  [\"a\",1]\n", "fn main()->[String]:\n  [true]\n",
          "fn main():\n  [[\"a\"]]\n", "fn main():\n  tensor([\"a\"])\n",
          "fn main():\n  map([\"a\"],fn(x):[x])\n", "fn main():\n  [\"a\"]+\"b\"\n",
          "fn main():\n  [\"a\"]==[\"a\"]\n", "fn main():\n  byteLength([\"a\"])\n",
          "fn main():\n  slice([\"a\"],0,2)\n", "fn main():\n  [\"a\"][1]\n"})
      require(!analyze(source).ok(), "invalid String array admitted");
    for (unsigned mode = 0; mode < 7; ++mode) {
      auto value = analyze("fn main():\n  [\"a\"]\n");
      for (auto &e : value.module->expressions)
        if (std::holds_alternative<TensorLiteral>(e.operation)) {
          if (mode == 0)
            e.type.array = false;
          if (mode == 1)
            e.type.tensor = false;
          if (mode == 2)
            e.type.shape.resize(2);
          if (mode == 3)
            e.type.device = 0;
          if (mode == 4)
            e.type.record = 0;
          if (mode == 5)
            e.type.shape.clear();
          if (mode == 6) {
            auto leaf = std::get<TensorLiteral>(e.operation).elements[0];
            value.module->expressions[leaf].type.builtin = BuiltinValue::Bool;
          }
        }
      require(bool(numericCapabilityError(*value.module, NumericTarget::Cpu)),
              "forged String array admitted");
    }
    std::cout << "String arrays: literals, empty context, mixed maps/captures, worker plans and "
                 "strict IR gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
