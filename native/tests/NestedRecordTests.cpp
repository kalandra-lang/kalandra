#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("nested-records.k", text)), limits);
}
int main() {
  try {
    const std::string declaration =
        "type Scene:\n  position:Point\n  enabled:Bool\ntype Point:\n  x:int\n  y:int\n";
    for (auto element : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      const std::string type(element);
      auto source =
          "type Envelope:\n  data:Reading\n  generation:int32\ntype Reading:\n  position:Point\n  "
          "enabled:Bool\ntype Point:\n  x:" +
          type + "\n  y:" + type +
          "\nfn move(p:Envelope)->Envelope:\n  p with {data:p.data with {position:p.data.position "
          "with {x:p.data.position.x+10}}}\nfn probe(p:Envelope)->Envelope ! TaskError:\n  try "
          "parallel:\n    work=spawn move(p)\n    first=try await work\n    again=try await work\n "
          "   again\nfn main()->" +
          type +
          " ! TaskError:\n  original=Envelope {generation:1,data:Reading "
          "{enabled:true,position:Point {x:2,y:3}}}\n  moved=try probe(original)\n  "
          "moved.data.position.x+original.data.position.x\n";
      auto result = analyze(source);
      require(result.ok(), "nested worker rejected: " + type);
      auto planned = continuation::plan(*result.module);
      require(bool(planned) && planned->kernels.records.size() == 3,
              "nested metadata lost in plan");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto body : {"fn main():\n  p=Scene {position:Point {x:2,y:3},enabled:true}\n  q=p with "
                      "{position:p.position with {x:12}}\n  q.position.x+p.position.x\n",
                      "fn make(x:int)->Scene:\n  Scene {position:Point {x:x,y:1},enabled:true}\nfn "
                      "main():\n  map([1,2,3],fn(x):make(x).position.x)\n",
                      "fn choose(flag:Bool)->Scene:\n  if flag: Scene {position:Point "
                      "{x:1,y:2},enabled:true}\n  else: Scene {position:Point "
                      "{x:3,y:4},enabled:false}\nfn main():\n  choose(false).position.x\n"})
      require(analyze(declaration + body).ok(), "valid nested record expression rejected");
    for (auto body :
         {"type Other:\n  x:int\n  y:int\nfn main():\n  Scene {position:Other "
          "{x:1,y:2},enabled:true}\n",
          "fn main():\n  Scene {position:1,enabled:true}\n",
          "fn main():\n  Scene {position:Point {x:1},enabled:true}\n",
          "fn main():\n  p=Scene {position:Point {x:1,y:2},enabled:true}\n  p with "
          "{position:p.enabled}\n",
          "fn main():\n  p=Scene {position:Point {x:1,y:2},enabled:true}\n  p with "
          "{position.x:4}\n",
          "fn main():\n  p=Scene {position:Point {x:1,y:2},enabled:true}\n  p.position.x=4\n  p\n",
          "fn main():\n  Scene {position:Point {x:1,y:2},enabled:true}.position.missing\n"})
      require(!analyze(declaration + body).ok(), "invalid nested construction/update admitted");
    for (auto cycle : {"type A:\n  child:A\n", "type A:\n  child:B\ntype B:\n  child:A\n",
                       "type Leaf:\n  value:int\ntype A:\n  leaf:Leaf\n  child:B\ntype B:\n  "
                       "child:C\ntype C:\n  child:A\n"}) {
      auto result = analyze(std::string(cycle) + "fn main():\n  0\n");
      require(!result.ok() && result.diagnostics[0].code == "S068" &&
                  result.diagnostics[0].message.find("cycle") != std::string::npos,
              "record storage cycle not diagnosed");
      require(renderDiagnostic(*result.source, result.diagnostics[0]).find("child:") !=
                  std::string::npos,
              "cycle diagnostic lost field location");
    }
    // Reuse of a completed child graph must still count its height and expanded
    // leaves at each embedding, even though type construction itself is cached.
    std::string chain = "type R0:\n  value:int\n";
    for (unsigned depth = 1; depth <= 128; ++depth) {
      chain += "type R" + std::to_string(depth) + ":\n  child:R" + std::to_string(depth - 1) + "\n";
      if (depth == 2 || depth == 3) {
        auto limits = SemanticLimits{};
        limits.maxDepth = 3;
        require(analyze(chain + "fn main():\n  0\n", limits).ok() == (depth == 2),
                "cached nesting depth miscounted");
      }
    }
    require(!analyze(chain + "fn main():\n  0\n").ok(), "default nesting resource bound ignored");
    std::string dag = "type R0:\n  value:int\n";
    for (unsigned depth = 1; depth <= 19; ++depth) {
      const auto child = "R" + std::to_string(depth - 1);
      dag += "type R" + std::to_string(depth) + ":\n  left:" + child + "\n  right:" + child + "\n";
      if (depth == 3 || depth == 4) {
        auto limits = SemanticLimits{};
        limits.maxValues = 8;
        require(analyze(dag + "fn main():\n  0\n", limits).ok() == (depth == 3),
                "shared child expanded-leaf budget miscounted");
      }
    }
    require(!analyze(dag + "fn main():\n  0\n").ok(),
            "default expanded-field resource bound ignored");
    auto valid = analyze(
        declaration + "fn main():\n  Scene {position:Point {x:1,y:2},enabled:true}.position.x\n");
    require(valid.ok(), "nested IR fixture rejected");
    for (unsigned mutation = 0; mutation < 10; ++mutation) {
      auto forged = *valid.module;
      auto &field = forged.records[0].fields[0].type;
      switch (mutation) {
      case 0:
        field.record = SIZE_MAX;
        break;
      case 1:
        field.record.reset();
        break;
      case 2:
        field.record = 0;
        break;
      case 3:
        forged.records[1].fields[0].type = field;
        break;
      case 4:
        field.task = true;
        break;
      case 5:
        field.fallible = true;
        break;
      case 6:
        field.tensor = true;
        break;
      case 7:
        field.device = 0;
        break;
      case 8:
        field.shape = {{1, 0}};
        break;
      case 9:
        field.builtin = BuiltinValue::Numeric;
        break;
      }
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value() &&
                  !continuation::plan(forged),
              "malformed nested layout reached planning");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      require(!lowerNative(forged), "malformed nested layout reached native type construction");
#endif
    }
    require(numericCapabilityError(*valid.module, NumericTarget::AppleGpu).has_value(),
            "nested GPU records enabled");
    std::cout << "nested records: numeric/Bool leaves, immutable nested updates, nominal fields, "
                 "forward declarations, worker metadata, cycle/depth/expanded-layout guards and "
                 "malformed IR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
