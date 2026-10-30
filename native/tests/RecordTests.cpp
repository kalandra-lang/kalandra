#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("records.k", text)));
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected scratch directory");
    const std::string declaration = "type Point:\n  x: int\n  y: int\n";
    for (auto element : {"int32", "uint32", "int64", "uint64", "float32", "float64", "Bool"}) {
      const std::string type(element), literal = type == "Bool" ? "true" : "1";
      auto result = analyze("type Box:\n  value:" + type + "\nfn make(value:" + type +
                            ")->Box:\n  Box {value:value}\nfn main():\n  first=make(" + literal +
                            ")\n  second=first with {value:" + literal + "}\n  second.value\n");
      require(result.ok(), "record field type rejected: " + type);
      require(result.module->records.size() == 1, "record declaration lost");
      require(dumpTypedIR(*result.module).find("record Box") != std::string::npos,
              "typed dump lost record construction");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
      require(lowered->mlir.find("llvm.insertvalue") != std::string::npos &&
                  lowered->mlir.find("llvm.extractvalue") != std::string::npos,
              "record was not lowered through aggregate MLIR");
#endif
    }
    for (auto source : {"type Point:\n  x:int\nfn main():\n  (Point {x:1} with {}).x\n",
                        "type Point:\n  x:int\nfn main():\n  p=Point {x:1}\n  q=if true: p with "
                        "{x:2}\n  else: p\n  q.x+p.x\n",
                        "type Point:\n  x:int\nfn project(x:int):\n  Point {x:x}.x\nfn main():\n  "
                        "map([1,2],project)\n",
                        "type Point:\n  x:int\nfn main():\n  map([1,2],fn(x):Point {x:x}.x)\n",
                        "fn main():\n  Point {x:1}.x\ntype Point:\n  x:int\n"}) {
      auto result = analyze(source);
      require(result.ok(), "valid record expression rejected");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      require(bool(lowerNative(*result.module)), "valid record lowering failed");
#endif
    }
    for (auto tail : {"fn main():\n  Point {x:1}\n", // missing y
                      "fn main():\n  Point {x:1,y:2,z:3}\n",
                      "fn main():\n  Point {x:1,x:2,y:3}\n",
                      "fn main():\n  Point {x:true,y:2}\n",
                      "fn main():\n  Point {x:int32(1),y:2}\n",
                      "fn main():\n  Unknown {x:1}\n",
                      "fn main():\n  Point {x:1,y:2}.z\n",
                      "fn main():\n  1.x\n",
                      "fn main():\n  1 with {x:2}\n",
                      "fn main():\n  Point {x:1,y:2} with {z:3}\n",
                      "fn main():\n  Point {x:1,y:2} with {x:3,x:4}\n",
                      "fn main():\n  Point {x:1,y:2} with {x:true}\n",
                      "fn main():\n  Point=1\n  Point {x:1,y:2}\n",
                      "type Point:\n  z:int\nfn main():\n  1\n",
                      "fn Point():\n  1\nfn main():\n  0\n",
                      "type Other:\n  x:int\n  y:int\nfn take(p:Point):\n  p.x\nfn main():\n  "
                      "take(Other {x:1,y:2})\n",
                      "type Other:\n  x:int\n  y:int\nfn main():\n  if true: Point {x:1,y:2}\n  "
                      "else: Other {x:1,y:2}\n",
                      "fn main():\n  p=Point {x:1,y:2}\n  p.x=3\n  p.x\n",
                      "fn main():\n  Point(1,2)\n",
                      "fn main():\n  Point {x:1,y:2}==Point {x:1,y:2}\n",
                      "fn main():\n  Point {x:1,y:2}+Point {x:1,y:2}\n",
                      "fn main():\n  parallel:\n    work=spawn read(Point {x:1,y:2})\n    "
                      "work.x\nfn read(p:Point):\n  p\n",
                      "fn make()->Point ! TaskError:\n  Point {x:1,y:2}\nfn main():\n  make().x\n"})
      require(!analyze(declaration + tail).ok(),
              "invalid record program admitted: " + std::string(tail));
    for (auto source :
         {"type Bad:\n  x:int\n  x:float\nfn main():\n  0\n",
          "type int:\n  x:int\nfn main():\n  0\n",
          "type Bad:\n  x:Tensor[Bool,2]\nfn main():\n  0\n",
          "type Bad:\n  x:Bad\nfn main():\n  0\n", "type Bad:\n  with:int\nfn main():\n  0\n",
          "type Bad:\n  x:Missing\nfn main():\n  0\n",
          "export type Bad\n  x:int\nfn main():\n  0\n", "type Bad:\n  x:int\nfn with():\n  0\n"})
      require(!analyze(source).ok(), "invalid declaration admitted");
    require(analyze("type Flags:\n  values:[Bool]\nfn main():\n  0\n").ok(),
            "Bool-array record declaration rejected");
    auto valid = analyze(declaration + "fn main():\n  p=Point {y:2,x:1}\n  p.x\n");
    require(valid.ok(), "valid IR fixture rejected");
    const auto construct =
        std::ranges::find_if(
            valid.module->expressions,
            [](const auto &e) { return std::holds_alternative<RecordValue>(e.operation); }) -
        valid.module->expressions.begin();
    const auto read = valid.module->functions[0].result;
    require(std::get<RecordValue>(valid.module->expressions[construct].operation).fields[0].field ==
                1,
            "initializer source order was reordered to declaration order");
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
      auto forged = *valid.module;
      auto &value = forged.expressions[construct];
      auto &op = std::get<RecordValue>(value.operation);
      auto &field = std::get<RecordFieldRead>(forged.expressions[read].operation);
      switch (mutation) {
      case 0:
        value.type.record = SIZE_MAX;
        break;
      case 1:
        value.type.record.reset();
        break;
      case 2:
        value.type.tensor = true;
        break;
      case 3:
        op.fields.pop_back();
        break;
      case 4:
        op.fields[0].field = SIZE_MAX;
        break;
      case 5:
        op.fields[0].field = op.fields[1].field;
        break;
      case 6:
        op.fields[0].value = SIZE_MAX;
        break;
      case 7:
        op.base = SIZE_MAX;
        break;
      case 8:
        field.input = SIZE_MAX;
        break;
      case 9:
        field.field = SIZE_MAX;
        break;
      case 10:
        forged.expressions[read].type.builtin = BuiltinValue::Bool;
        break;
      case 11:
        forged.records[0].fields[0].type.builtin = BuiltinValue::Record;
        break;
      }
      require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
              "malformed record IR admitted");
    }
    require(numericCapabilityError(*valid.module, NumericTarget::AppleGpu).has_value(),
            "record silently enabled for GPU");
    require(bool(continuation::plan(*valid.module)), "record continuation planning failed");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
    auto direct = analyze(declaration + "fn main():\n  Point {x:1,y:2}\n");
    require(direct.ok() && !lowerNative(*direct.module), "record CLI result silently enabled");
#endif
    namespace fs = std::filesystem;
    const fs::path root(argv[1]);
    fs::create_directories(root);
    const auto write = [&](const char *file, const std::string &text) {
      std::ofstream(root / file) << text;
    };
    const auto loaded = [&](const char *file) {
      auto parsed = loadModules((root / file).string());
      require(bool(parsed), "cannot load module fixture");
      return check(std::move(*parsed));
    };
    write("a.k",
          declaration + "export fn make():\n  Point {x:1,y:2}\nexport fn take(p:Point):\n  p.x\n");
    write("b.k", declaration + "export fn make():\n  Point {x:3,y:4}\n");
    write(
        "main.k",
        "from \"./a.k\" import make, take\nfn main():\n  p=make() with {x:4}\n  take(p)\n");
    auto module = loaded("main.k");
    require(module.ok(), "inferred imported record lost original type");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
    require(bool(lowerNative(*module.module)), "imported record lowering failed");
#endif
    write("main.k", "from \"./a.k\" import take\nfrom \"./b.k\" import "
                           "make\nfn main():\n  take(make())\n");
    require(!loaded("main.k").ok(), "distinct same-spelling module records unified");
    write("main.k", "from \"./a.k\" import Point\nfn main():\n  0\n");
    require(!loaded("main.k").ok(), "type export silently enabled");
    write("main.k",
          "from \"./a.k\" import make as Point\n" + declaration + "fn main():\n  0\n");
    require(!loaded("main.k").ok(), "type/import namespace collision admitted");
    std::cout << "records: six numeric field types and Bool, construction/read/update, nominal "
                 "identity, source order, immutable syntax, imported values, malformed IR and "
                 "task-handle/GPU/CLI gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
