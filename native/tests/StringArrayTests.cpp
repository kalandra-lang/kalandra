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
         {"fn main()->[String]:\n  return []\n", "fn main():\n  return [\"é\",\"\",\"👩\\0\"]\n",
          "fn append(x:String)->String:\n  return x+\"!\"\nfn main()->[String]:\n  return map([],append)\n",
          "fn main():\n  return map([1,2],fn(x):return \"a\")\n",
          "fn main():\n  values=[\"a\",\"b\"]\n  return map([0,1],fn(i):return values[i])\n",
          "fn main():\n  return map([\"é\",\"a\"],fn(text):return length(text))\n",
          "type Row:\n  text:String\nfn main():\n  return map([Row {text:\"a\"}],fn(row):return row.text)\n",
          "fn work(values:[String])->[String] ! TaskError:\n  return try parallel:\n    work=spawn "
          "map(values,fn(text):return text+\"!\")\n    first=try await work\n    again=try await work\n   "
          " again\nfn main()->[String] ! TaskError:\n  return try work([\"é\"])\n"}) {
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
         {"fn main():\n  return []\n", "fn main():\n  return [\"a\",1]\n", "fn main()->[String]:\n  return [true]\n",
          "fn main():\n  return [[\"a\"]]\n", "fn main():\n  return tensor([\"a\"])\n",
          "fn main():\n  return map([\"a\"],fn(x):return [x])\n", "fn main():\n  return [\"a\"]+\"b\"\n",
          "fn main():\n  return [\"a\"]==[\"a\"]\n", "fn main():\n  return byteLength([\"a\"])\n",
          "fn main():\n  return slice([\"a\"],0,2)\n", "fn main():\n  return [\"a\"][1]\n"})
      require(!analyze(source).ok(), "invalid String array admitted");
    for (unsigned mode = 0; mode < 7; ++mode) {
      auto value = analyze("fn main():\n  return [\"a\"]\n");
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
