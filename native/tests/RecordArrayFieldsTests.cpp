#include "../lib/RecordLayout.h"
#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <set>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("record-array-fields.k", source)));
}
int main() {
  try {
    const std::string decl = "type Row:\n  name:String\n  values:[int]\nenum Item:\n  Empty\n"
                             "  Value:\n    rows:[Row]\ntype Box:\n  items:[Item]\n  rows:[Row]\n";
    for (const auto *body :
         {"type Recursive:\n  rows:[Recursive]\nfn main()->int:\n  0\n",
          "enum Recursive:\n  Empty\n  Value:\n    rows:[Recursive]\nfn main()->int:\n  0\n",
          "type First:\n  rows:[Second]\ntype Second:\n  parent:First\nfn main()->int:\n  0\n",
          "fn main()->int:\n  p=Box {items:[],rows:[]}\n  length(p.rows)+length(p.items)\n",
          "fn main()->int:\n  p=Item.Value {rows:[Row {name:\"é\",values:[1,2]}]}\n"
          "  match p:\n    Item.Empty:0\n    Item.Value {rows}:rows[0].values[1]\n",
          "fn echo(p:Box)->Box:\n  p\nfn main()->int ! TaskError:\n"
          "  p=try parallel:\n    try await spawn echo(Box {items:[Item.Empty],rows:[]})\n"
          "  match p.items[0]:\n    Item.Empty:1\n    _:0\n",
          "fn main()->int:\n  row=Row {name:\"hi\",values:[2]}\n"
          "  a=Box {items:[Item.Value {rows:[row]}],rows:[row]}\n"
          "  b=a with {rows:map(a.rows,fn(r):r with {name:r.name+\"!\"})}\n"
          "  length(b.rows[0].name)+length(a.rows[0].name)\n"}) {
      auto checked = analyze(decl + body);
      if (!checked.ok())
        for (const auto &d : checked.diagnostics)
          std::cerr << renderDiagnostic(*checked.source, d);
      require(checked.ok(), "valid record/enum array field rejected");
      require(!numericCapabilityError(*checked.module, NumericTarget::Cpu), "CPU field gated");
      require(bool(numericCapabilityError(*checked.module, NumericTarget::AppleGpu)),
              "GPU field admitted");
      require(bool(continuation::plan(*checked.module)), "array-field plan rejected");
#ifndef KALANDRA_RECORD_ARRAY_FIELD_FRONTEND_ONLY
      auto lowered = lowerNative(*checked.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *body :
         {"fn main():\n  Item.Value {rows:[Item.Empty]}\n",
          "fn main():\n  Box {items:[Row {name:\"\",values:[]}],rows:[]}\n",
          "type Bad:\n  rows:Tensor[Row,2]\nfn main()->int:\n  0\n",
          "type Bad:\n  rows:[[Row]]\nfn main()->int:\n  0\n"})
      require(!analyze(decl + body).ok(), "invalid array-field program admitted");
    auto valid = analyze(decl + "fn main()->int:\n  length(Box {items:[],rows:[]}.rows)\n");
    require(valid.ok(), "field metadata fixture rejected");
    require(recordInlineUnits(*valid.module, 2) == 10,
            "record-array fields must count descriptors rather than inline elements");
    auto lengths =
        analyze(decl + "fn first(p:Item)->int:\n  match p:\n    Item.Empty:0\n"
                       "    Item.Value {rows}:length(rows)\nfn second(p:Item)->int:\n"
                       "  match p:\n    Item.Empty:0\n    Item.Value {rows}:length(rows)\n"
                       "fn main()->int:\n  0\n");
    require(lengths.ok(), "payload extent fixture rejected");
    std::set<ExtentId> extents;
    unsigned reads = 0;
    for (const auto &e : lengths.module->expressions)
      if (std::holds_alternative<RecordFieldRead>(e.operation) && e.type.array) {
        require(e.type.shape.size() == 1 && !e.type.shape[0].constant,
                "enum-array field inherited a constructor length");
        extents.insert(e.type.shape[0].identity);
        ++reads;
      }
    require(reads == 2 && extents.size() == reads,
            "independent enum pattern bindings share dynamic extents");
    for (unsigned mode = 0; mode < 9; ++mode) {
      auto forged = *valid.module;
      auto &field = forged.records[2].fields[1].type;
      if (mode == 0)
        field.task = true;
      if (mode == 1)
        field.fallible = true;
      if (mode == 2)
        field.device = 0;
      if (mode == 3)
        field.tensor = false;
      if (mode == 4)
        field.array = false;
      if (mode == 5)
        field.shape.clear();
      if (mode == 6)
        field.shape.resize(2);
      if (mode == 7)
        field.record = SIZE_MAX;
      if (mode == 8)
        field.record.reset();
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "forged array field admitted");
    }
    std::cout
        << "Record/enum array fields: construction, nested ownership, updates, nominal types, "
           "worker plans, finite layouts and malformed metadata checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
