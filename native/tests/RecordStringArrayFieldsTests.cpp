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
  return check(parse(std::make_shared<SourceFile>("record-string-arrays.k", text)), limits);
}
int main() {
  try {
    const std::string decl =
        "type Data:\n  names:[String]\n  label:String\ntype Box:\n  data:Data\n  copy:[String]\n";
    for (const auto *body :
         {"fn main()->[String]:\n  Box {data:Data {names:[],label:\"\"},copy:[]}.data.names\n",
          "fn change(p:Box)->Box:\n  p with {data:p.data with "
          "{names:map(p.data.names,fn(text):text+\"!\")}}\nfn main()->[String] ! TaskError:\n  "
          "p=Box {data:Data {names:[\"é\"],label:\"\"},copy:[]}\n  changed=try parallel:\n    try "
          "await spawn change(p)\n  [p.data.names[0],changed.data.names[0]]\n",
          "fn main():\n  p=Box {data:Data {names:[\"a\"],label:\"b\"},copy:[]}\n  "
          "map([0],fn(i):p.data.names[i]+p.data.label)\n",
          "fn main():\n  rows=[Box {data:Data {names:[\"a\"],label:\"b\"},copy:[]}]\n  "
          "length(map(rows,fn(p):p with {copy:p.data.names}))\n"}) {
      auto value = analyze(decl + body);
      if (!value.ok())
        for (const auto &d : value.diagnostics)
          std::cerr << renderDiagnostic(*value.source, d);
      require(value.ok(), "valid String-array field rejected");
      require(!numericCapabilityError(*value.module, NumericTarget::Cpu), "CPU fields gated");
      require(bool(numericCapabilityError(*value.module, NumericTarget::AppleGpu)),
              "GPU record enabled");
      require(bool(continuation::plan(*value.module)), "field plan failed");
#ifndef KALANDRA_RECORD_STRING_ARRAY_FRONTEND_ONLY
      auto lowered = lowerNative(*value.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *source : {"type Box:\n  names:[String]\nfn main():\n  Box {names:[1]}\n",
                               "type Box:\n  names:[String]\nfn main():\n  Box {names:\"a\"}\n",
                               "type Box:\n  names:[String]\nfn main():\n  p=Box {names:[\"a\"]}\n "
                               " p with {names:[false]}\n",
                               "type Box:\n  names:[[String]]\nfn main():\n  0\n",
                               "type Box:\n  names:Tensor[String,2]\nfn main():\n  0\n",
                               "type Box:\n  names:[String]\n  next:Box\nfn main():\n  0\n"})
      require(!analyze(source).ok(), "invalid String-array field admitted");
    const auto valid = analyze(
        "type Box:\n  names:[String]\nfn make(names:[String])->Box:\n  Box {names:names}\nfn "
        "main():\n  a=make([\"a\"])\n  b=make([\"b\",\"c\"])\n  length(a.names)+length(b.names)\n");
    require(valid.ok(), "field length fixture rejected");
    std::vector<ExtentId> reads;
    for (const auto &e : valid.module->expressions)
      if (std::holds_alternative<RecordFieldRead>(e.operation)) {
        require(!e.type.shape[0].constant, "field read inherited a constructor extent");
        reads.push_back(e.type.shape[0].identity);
      }
    require(reads.size() == 2 && reads[0] != reads[1],
            "unrelated field reads share extent identity");
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
        type.tensor = false;
      if (mode == 4)
        type.array = false;
      if (mode == 5)
        type.shape.clear();
      if (mode == 6)
        type.shape.resize(2);
      if (mode == 7)
        type.record = 0;
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)), "forged field admitted");
    }
    SemanticLimits budget;
    budget.maxValues = 4;
    const std::string minimal = "type Box:\n  names:[String]\nfn main():\n  0\n";
    require(!analyze(minimal, budget).ok(), "String array descriptor undercounted");
    budget.maxValues = 5;
    require(analyze(minimal, budget).ok(), "five-unit array descriptor rejected");
    std::cout << "String-array record fields: nested updates, arrays/maps/captures, worker plans, "
                 "fresh lengths and five-unit layouts passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
