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
  return check(parse(std::make_shared<SourceFile>("record-bool-fields.k", text)));
}
int main() {
  try {
    const std::string decl =
        "type Box:\n  data:Data\n  numbers:[int]\ntype Data:\n  flags:[Bool]\n  enabled:Bool\n";
    for (const auto *body :
         {"fn main()->[Bool]:\n  Box {data:Data "
          "{flags:[true,false],enabled:true},numbers:[1]}.data.flags\n",
          "fn main()->[Bool]:\n  Box {data:Data {flags:[],enabled:false},numbers:[]}.data.flags\n",
          "fn change(p:Box)->Box:\n  p with {data:p.data with {flags:map(p.data.flags,fn(flag):not "
          "flag)}}\nfn main()->[Bool] ! TaskError:\n  p=Box {data:Data "
          "{flags:[true,false],enabled:true},numbers:[1]}\n  result=try parallel:\n    try await "
          "spawn change(p)\n  result.data.flags\n",
          "fn make()->[Box]:\n  [Box {data:Data {flags:[true],enabled:true},numbers:[1]}]\nfn "
          "main()->[Bool] ! TaskError:\n  rows=try parallel:\n    try await spawn make()\n  "
          "map(rows,fn(row):row.data.flags[0])\n",
          "fn selected(flag:Bool,n:int)->int:\n  if flag:n\n  else:0\nfn main()->[int] ! "
          "TaskError:\n  p=Box {data:Data {flags:[true],enabled:true},numbers:[1]}\n  try "
          "parallel:\n    try await spawn "
          "map(p.numbers,fn(value):selected(p.data.flags[0],value))\n"}) {
      auto result = analyze(decl + body);
      require(result.ok(), "valid Bool field rejected: " + std::string(body));
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "Bool field capability rejected");
      require(numericCapabilityError(*result.module, NumericTarget::AppleGpu).has_value(),
              "GPU Bool field enabled");
      require(bool(continuation::plan(*result.module)), "Bool field plan rejected");
#ifndef KALANDRA_RECORD_BOOL_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *body : {"type Bad:\n  values:Tensor[Bool,2]\nfn main():\n  0\n",
                             "type Bad:\n  values:[[Bool]]\nfn main():\n  0\n",
                             "type Box:\n  values:[Bool]\nfn main():\n  Box {values:[1]}\n",
                             "type Box:\n  values:[Bool]\nfn main():\n  Box {values:true}\n",
                             "type Box:\n  values:[Bool]\nfn main():\n  p=Box {values:[true]}\n  p "
                             "with {values:[1]}\n",
                             "type Box:\n  values:[Box]\nfn main():\n  Box {values:[true]}\n"})
      require(!analyze(body).ok(), "invalid Bool field admitted");
    auto valid = analyze(
        "type Box:\n  flags:[Bool]\nfn make(flags:[Bool])->Box:\n  Box {flags:flags}\nfn main():\n "
        " p=make([true])\n  q=make([false,true])\n  length(p.flags)+length(q.flags)\n");
    require(valid.ok(), "Bool field freshness fixture rejected");
    std::vector<ExtentId> reads;
    for (const auto &e : valid.module->expressions)
      if (std::holds_alternative<RecordFieldRead>(e.operation)) {
        require(!e.type.shape[0].constant, "Bool field inherited constructor length");
        reads.push_back(e.type.shape[0].identity);
      }
    require(reads.size() == 2 && reads[0] != reads[1],
            "Bool field reads share a false length identity");
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
      auto forged = *valid.module;
      auto &field = forged.records[0].fields[0].type;
      if (mutation == 0)
        field.device = 0;
      if (mutation == 1)
        field.task = true;
      if (mutation == 2)
        field.fallible = true;
      if (mutation == 3)
        field.array = false;
      if (mutation == 4)
        field.tensor = false;
      if (mutation == 5)
        field.shape.clear();
      if (mutation == 6)
        field.shape.resize(2);
      if (mutation == 7)
        field.record = 0;
      if (mutation == 8) {
        field.builtin = BuiltinValue::Record;
        field.record = 0;
      }
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
              "forged Bool field admitted");
    }
    std::cout << "record Bool fields: nested values, updates, maps, captures, workers, freshness "
                 "and malformed IR passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
