#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("enums.k", source)));
}
static void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    const std::string prefix = "enum Reading:\n  Missing\n  Value:\n    measurement:float32\n  "
                               "Invalid:\n    reason:String\n";
    for (const auto &tail :
         {"fn main()->int:\n  match Reading.Value {measurement:21.5}:\n    Reading.Missing:0\n    "
          "Reading.Value {measurement:sample}:int(sample)\n    Reading.Invalid "
          "{reason}:length(reason)\n",
          "fn main()->int:\n  value=Reading.Invalid {reason:\"bad\"+\" input\"}\n  match value:\n  "
          "  Reading.Invalid {reason}:length(reason)\n    _:0\n",
          "fn main()->int:\n  match Reading.Missing:\n    _:\n      answer=42\n      answer\n",
          "fn echo(x:Reading)->Reading:\n  x\nfn main()->int ! TaskError:\n  value=try parallel:\n "
          "   work=spawn echo(Reading.Value {measurement:42})\n    one=try await work\n    "
          "again=try await work\n    again\n  match value:\n    Reading.Value "
          "{measurement}:int(measurement)\n    _:0\n",
          "type Box:\n  reading:Reading\nfn main()->int:\n  box=Box {reading:Reading.Missing}\n  "
          "match box.reading:\n    Reading.Missing:42\n    _:0\n",
          "fn main()->int:\n  values:[Reading]=[Reading.Missing,Reading.Value {measurement:2}]\n  "
          "match values[1]:\n    Reading.Value {measurement}:int(measurement)\n    _:0\n"}) {
      auto typed = analyze(prefix + tail);
      if (!typed.ok())
        for (const auto &d : typed.diagnostics)
          std::cerr << renderDiagnostic(*typed.source, d);
      require(typed.ok(), "valid enum program rejected");
      require(typed.module->records[0].variants.size() == 3, "enum alternatives lost");
      require(bool(continuation::plan(*typed.module)), "enum continuation plan rejected");
      require(bool(numericCapabilityError(*typed.module, NumericTarget::AppleGpu)),
              "GPU enum admitted");
#ifndef KALANDRA_ENUM_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto &description : numericTypes) {
      std::string type(description.name);
      auto typed = analyze("enum Sample:\n  Empty\n  Value:\n    item:" + type +
                           "\nfn main()->int:\n  value=Sample.Value {item:1}\n  match value:\n    "
                           "Sample.Empty:0\n    Sample.Value {item}:int(item)\n");
      require(typed.ok(), "numeric payload rejected: " + type);
#ifndef KALANDRA_ENUM_FRONTEND_ONLY
      require(bool(lowerNative(*typed.module)), "numeric enum lowering rejected");
#endif
    }
    for (const auto &fieldType : {"[int]", "[Bool]", "[String]", "Tensor[float32,2,2]",
                                  "Tensor[int]", "Tensor[uint64,?,?]", "Reading"}) {
      auto typed = analyze(prefix + "enum Envelope:\n  Empty\n  Value:\n    item:" + fieldType +
                           "\nfn main()->int:\n  match Envelope.Empty:\n    "
                           "Envelope.Empty:1\n    Envelope.Value:0\n");
      require(typed.ok(), "completed enum payload type rejected: " + std::string(fieldType));
      require(bool(continuation::plan(*typed.module)), "payload continuation plan rejected");
#ifndef KALANDRA_ENUM_FRONTEND_ONLY
      auto lowered = lowerNative(*typed.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    require(!analyze("enum Recursive:\n  Value:\n    next:Recursive\nfn main()->int:\n  0\n").ok(),
            "recursive inline enum admitted");
    require(!analyze("type Box:\n  next:Recursive\nenum Recursive:\n  Value:\n    box:Box\n"
                     "fn main()->int:\n  0\n")
                 .ok(),
            "mutually recursive enum/record admitted");
    auto rows = analyze(prefix + "enum Rows:\n  Empty\n  Value:\n    items:[Reading]\n"
                                 "fn echo(x:Rows)->Rows:\n  x\nfn main()->int ! TaskError:\n"
                                 "  value=try parallel:\n    try await spawn echo(Rows.Value "
                                 "{items:[Reading.Missing]})\n  match value:\n"
                                 "    Rows.Empty:0\n    Rows.Value {items}:length(items)\n");
    require(rows.ok(), "enum-array payload rejected");
    require(bool(continuation::plan(*rows.module)), "enum-array payload plan rejected");
#ifndef KALANDRA_ENUM_FRONTEND_ONLY
    auto loweredRows = lowerNative(*rows.module);
    require(bool(loweredRows), loweredRows ? "" : loweredRows.error());
#endif
    require(analyze("enum Recursive:\n  Empty\n  Value:\n    next:[Recursive]\n"
                     "fn main()->int:\n  0\n")
                 .ok(),
            "finite recursive enum-array payload rejected");
    for (const auto &tail :
         {"fn main():\n  Reading.Value\n",
          "fn main():\n  Reading.Unknown\n",
          "fn main():\n  Reading.Value {}\n",
          "fn main():\n  Reading.Value {measurement:1,measurement:2}\n",
          "fn main():\n  Reading.Value {unknown:1}\n",
          "fn main():\n  Reading.Value {measurement:true}\n",
          "fn main():\n  Reading.Invalid {reason:1}\n",
          "fn main():\n  Reading {measurement:1}\n",
          "fn main():\n  Reading.Missing with {}\n",
          "fn main():\n  Reading.Value {measurement:1}.measurement\n",
          "fn main():\n  match Reading.Missing:\n    Reading.Value {measurement}:measurement\n",
          "fn main():\n  match Reading.Missing:\n    Reading.Missing:1\n    Reading.Missing:2\n    "
          "_:3\n",
          "fn main():\n  match Reading.Missing:\n    _:1\n    Reading.Missing:2\n",
          "fn main():\n  match Reading.Missing:\n    Reading.Missing:1\n    Reading.Value:2\n    "
          "Reading.Invalid:3\n    _:4\n",
          "fn main():\n  match Reading.Missing:\n    Reading.Value {reason}:1\n    _:0\n",
          "fn main():\n  match Reading.Missing:\n    Reading.Value "
          "{measurement:a,measurement:b}:1\n    _:0\n",
          "fn main():\n  match Reading.Missing:\n    Reading.Value {measurement}:measurement\n    "
          "_:true\n",
          "enum Other:\n  Missing\nfn take(x:Reading)->int:\n  0\nfn main():\n  "
          "take(Other.Missing)\n",
          "fn main():\n  match Reading.Missing:\n    Other.Missing:0\n    _:1\n",
          "enum Duplicate:\n  Item\n  Item\nfn main()->int:\n  0\n",
          "enum Bad:\n  Item:\n    x:int\n    x:int\nfn main()->int:\n  0\n",
          "fn main():\n  Reading.Missing == Reading.Missing\n"})
      require(!analyze(prefix + tail).ok(), "invalid enum program admitted: " + std::string(tail));
    require(!analyze("enum Pair:\n  Value:\n    left:int\n    right:int\nfn main()->int:\n  match "
                     "Pair.Value {left:1,right:2}:\n    Pair.Value {left:x,right:x}:x\n")
                 .ok(),
            "duplicate field binding admitted");
    SemanticLimits small;
    small.maxDepth = 3;
    auto bounded =
        check(parse(std::make_shared<SourceFile>(
                  "budget.k",
                  prefix + "fn main()->int:\n  match Reading.Missing:\n    Reading.Missing:1\n    "
                           "Reading.Value:2\n    Reading.Invalid:3\n")),
              small);
    require(!bounded.ok(), "enum branch-depth budget was bypassed");
    auto valid = analyze(
        prefix + "fn main()->int:\n  match Reading.Missing:\n    Reading.Missing:42\n    _:0\n");
    auto tagForgery = *valid.module;
    for (auto &node : tagForgery.expressions)
      if (auto *record = std::get_if<RecordValue>(&node.operation))
        record->variant = 99;
    require(bool(numericCapabilityError(tagForgery, NumericTarget::Cpu)),
            "invalid enum discriminator admitted");
    auto incomplete =
        analyze(prefix + "fn main()->int:\n  match Reading.Value {measurement:1}:\n"
                         "    Reading.Value {measurement}:int(measurement)\n    _:0\n");
    for (unsigned mode = 0; mode < 2; ++mode) {
      auto forged = *incomplete.module;
      for (auto &node : forged.expressions)
        if (auto *value = std::get_if<RecordValue>(&node.operation)) {
          if (mode == 0)
            value->fields.clear();
          else
            value->variant = 0;
        }
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "missing active or populated inactive payload admitted");
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto forged = *valid.module;
      if (mode == 0)
        forged.records[0].variants[0].fields.push_back(999);
      if (mode == 1)
        forged.records[0].variants[1].name = forged.records[0].variants[0].name;
      if (mode == 2)
        forged.records[0].fields[0].type.element = NumericType::UInt8;
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "forged enum layout admitted");
    }
    std::cout << "Enums: scalar/collection/nested payloads, nominal identity, construction, "
                 "exhaustive branches, "
                 "scopes, workers, arrays and explicit GPU gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
