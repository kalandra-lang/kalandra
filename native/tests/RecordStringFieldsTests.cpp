#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("record-strings.k", text)), limits);
}
int main() {
  try {
    const std::string decl =
        "type Inner:\n  text:String\n  count:int\ntype Box:\n  inner:Inner\n  copy:String\n";
    for (const auto *body :
         {"fn echo(box:Box)->Box:\n  box\nfn main():\n  echo(Box {inner:Inner "
          "{text:\"é\",count:1},copy:\"\"}).inner.text\n",
          "fn make()->Box:\n  Box {inner:Inner {text:\"سلام\",count:1},copy:\"👩\\0\"}\nfn "
          "change(box:Box)->Box:\n  box with {inner:box.inner with "
          "{text:box.inner.text+\"!\"}}\nfn main()->String ! TaskError:\n  box=try parallel:\n    "
          "try await spawn change(make())\n  box.inner.text\n",
          "fn main():\n  box=Box {inner:Inner {text:\"a\",count:1},copy:\"b\"}\n  "
          "map([1,2],fn(x):length(box.inner.text)+x)\n",
          "fn echo(rows:[Box])->[Box]:\n  map(rows,fn(box):box with {copy:box.inner.text})\nfn "
          "main()->int ! TaskError:\n  rows=try parallel:\n    try await spawn echo([Box "
          "{inner:Inner {text:\"é\",count:1},copy:\"\"}])\n  length(rows[0].copy)\n"}) {
      auto value = analyze(decl + body);
      if (!value.ok())
        for (const auto &d : value.diagnostics)
          std::cerr << renderDiagnostic(*value.source, d);
      require(value.ok(), "valid String record rejected");
      require(!numericCapabilityError(*value.module, NumericTarget::Cpu),
              "CPU String record gated");
      require(numericCapabilityError(*value.module, NumericTarget::AppleGpu).has_value(),
              "GPU String record enabled");
      require(hasOrdinaryStrings(*value.module), "record schema failed to identify String storage");
      require(bool(continuation::plan(*value.module)), "String record continuation failed");
#ifndef KALANDRA_RECORD_STRING_FRONTEND_ONLY
      const auto output = lowerNative(*value.module);
      require(bool(output), output ? "" : output.error());
#endif
    }
    for (const auto *source :
         {"type Row:\n  text:String\nfn main():\n  Row {text:1}\n",
          "type Row:\n  text:String\nfn main():\n  Row {text:true}\n",
          "type Row:\n  text:String\nfn main():\n  row=Row {text:\"a\"}\n  row with {text:[1]}\n",
          "type Row:\n  text:String\nfn main():\n  Row {}\n",
          "type Row:\n  text:Tensor[String,2]\nfn main():\n  0\n",
          "type Row:\n  text:String\n  next:Row\nfn main():\n  0\n"})
      require(!analyze(source).ok(), "invalid String field or String-element array admitted");
    const std::string simple = "type Row:\n  text:String\nfn main():\n  0\n";
    auto valid = analyze(simple);
    require(valid.ok() && hasOrdinaryStrings(*valid.module), "unused String schema not tracked");
    for (unsigned mode = 0; mode < 8; ++mode) {
      auto forged = *valid.module;
      auto &type = forged.records[0].fields[0].type;
      if (mode == 0)
        type.task = true;
      if (mode == 1)
        type.fallible = true;
      if (mode == 2)
        type.device = 0;
      if (mode == 3)
        type.tensor = true;
      if (mode == 4)
        type.array = true;
      if (mode == 5)
        type.shape = {{2, 0}};
      if (mode == 6)
        type.record = 0;
      if (mode == 7)
        // TaskError is now valid field data; FaultKind remains unsupported here.
        type.builtin = BuiltinValue::FaultKind;
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
              "forged String field admitted");
    }
    SemanticLimits budget;
    budget.maxValues = 2;
    require(!analyze(simple, budget).ok(), "String descriptor ignored expanded layout budget");
    budget.maxValues = 3;
    require(analyze(simple, budget).ok(), "String descriptor exceeded its three-unit budget");
    std::cout << "String record fields: nested updates, maps/captures, worker plans, strict field "
                 "types, finite layouts and forged IR checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
