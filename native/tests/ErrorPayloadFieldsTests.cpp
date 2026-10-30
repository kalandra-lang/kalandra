#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("error-payload-fields.k", text)));
}
int main() {
  try {
    const std::string types =
        "type Both=A | B\ntype Mixed=Both | TaskError\ntype Only=TaskError\n"
        "error A:\n  Bad:\n    text:String\n    values:[int]\nerror B:\n  Missing\n";
    for (const auto *declaration :
         {"type Box:\n  cause:Both\n", "type Box:\n  cause:TaskError\n",
          "type Box:\n  cause:Mixed\n", "type Box:\n  cause:Only\n", "type Box:\n  causes:[Both]\n",
          "enum Box:\n  Empty\n  Wrapped:\n    cause:Both\n",
          "enum Box:\n  Empty\n  Wrapped:\n    cause:Only\n",
          "error Box:\n  Wrapped:\n    cause:Mixed\n",
          "type Box:\n  inner:Inner\ntype Inner:\n  causes:[Mixed]\n"}) {
      // Field resolution must not depend on placement of the alias declaration.
      for (bool forward : {false, true}) {
        auto result = analyze((forward ? std::string(declaration) + types : types + declaration) +
                              "fn echo(box:Box)->Box:\n  box\nfn main()->int:\n  0\n");
        if (!result.ok())
          for (const auto &d : result.diagnostics)
            std::cerr << renderDiagnostic(*result.source, d);
        require(result.ok(), "valid alias payload rejected");
        require(bool(continuation::plan(*result.module)), "payload continuation rejected");
#ifndef KALANDRA_ERROR_PAYLOAD_FRONTEND_ONLY
        auto lowered = lowerNative(*result.module);
        require(bool(lowered), lowered ? "" : lowered.error().c_str());
#endif
      }
    }
    for (const auto *program :
         {"type Box:\n  cause:E\ntype E=A | B\nerror A:\n  Bad:\n    box:Box\nerror B:\n  "
          "Missing\n",
          "type Box:\n  causes:[E]\ntype E=A | B\nerror A:\n  Bad:\n    boxes:[Box]\nerror B:\n  "
          "Missing\n"}) {
      auto checked = analyze(std::string(program) + "fn main()->int:\n  0\n");
      require(checked.ok() && bool(continuation::plan(*checked.module)),
              "finite recursive error-alias payload rejected");
#ifndef KALANDRA_ERROR_PAYLOAD_FRONTEND_ONLY
      require(bool(lowerNative(*checked.module)), "recursive error-alias lowering failed");
#endif
    }
    for (const auto *program :
         {"type Box:\n  cause:E\ntype E=Other\ntype Other=E\n",
          "type Box:\n  cause:E\ntype E=Missing\n",
          "type Box:\n  cause:E\ntype E=A | Ordinary\nerror A:\n  Missing\nenum Ordinary:\n  "
          "Value\n",
          "type Box:\n  cause:[Only]\ntype Only=TaskError\n",
          "enum Box:\n  Wrapped:\n    cause:Tensor[Both,2]\ntype Both=A | B\nerror A:\n  "
          "Missing\nerror B:\n  Other\n",
          "type Box:\n  value:Box\n", "type Box:\n  value:Inner\ntype Inner:\n  value:Box\n",
          "type Box:\n  cause:E\ntype E=A | B\nerror A:\n  Missing\nerror B:\n  Other\nfn "
          "bad()->Box:\n  Box {cause:A.Missing}\n"})
      require(!analyze(std::string(program) + "fn main()->int:\n  0\n").ok(),
              "invalid/cyclic alias payload admitted");
    // Error-value fields are completed data, never an implicit result channel.
    require(!analyze(types + "type Box:\n  cause:Both\nfn bad()->int ! Both:\n  fail A.Bad "
                             "{text:\"bad\",values:[]}\nfn main()->Box:\n  Box {cause:bad()}\n")
                 .ok(),
            "fallible result used as payload data");
    auto checked = analyze(types + "type Box:\n  cause:Only\nfn echo(box:Box)->Box:\n  box\n"
                                   "fn main()->int:\n  0\n");
    require(checked.ok(), "TaskError field metadata fixture rejected");
    for (int mode = 0; mode < 5; ++mode) {
      auto forged = *checked.module;
      for (auto &record : forged.records) {
        if (mode == 4 && record.taskErrorFamily)
          record.taskErrorFamily = false;
        if (record.name == "Box") {
          auto &field = record.fields.front().type;
          if (mode == 0)
            field.fallible = true;
          if (mode == 1)
            field.task = true;
          if (mode == 2)
            field.errorRecord = 0;
          if (mode == 3)
            field.device = 0;
        }
      }
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "malformed TaskError payload metadata admitted");
    }
    std::cout << "Error alias fields: forward references, nested fields, TaskError data, "
                 "strict typing and finite recursive dependencies passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
