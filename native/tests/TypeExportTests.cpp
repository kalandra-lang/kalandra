#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace fs = std::filesystem;
static fs::path root;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void write(const std::string &name, const std::string &text) {
  std::ofstream stream(root / name);
  stream << text;
  stream.close();
  require(bool(stream), "cannot write fixture");
}
static CheckResult analyze(const std::string &source) {
  write("main.k", source);
  auto loaded = loadModules((root / "main.k").string());
  require(bool(loaded), "loader failed");
  return check(std::move(*loaded));
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    root = fs::absolute(argv[1]);
    fs::create_directories(root);
    write("types.k", "export type Row:\n  names:[String]\n  value:int64\ntype Secret:\n  "
                            "flag:Bool\nexport fn make()->Row:\n  Row {names:[\"é\"],value:2}\n");
    write("other.k", "export type Row:\n  names:[String]\n  value:int64\n");
    write("wrapper.k", "from \"./types.k\" import Row\nexport type Box:\n  "
                              "row:Row\nexport fn box(row:Row)->Box:\n  Box {row:row}\n");
    write("alias.k", "from \"./types.k\" import Row as Renamed\n");
    const std::string imports = "from \"./types.k\" import Row as R, Row as Same, "
                                "make\nfrom \"./wrapper.k\" import Box, box\n";
    auto good = analyze(
        imports + "fn identity(row:Same)->R:\n  row\nfn update(value:Box)->Box:\n  value with "
                  "{row:identity(value.row) with {names:map(value.row.names,fn(s):s+\"!\")}}\nfn "
                  "main()->[String] ! TaskError:\n  result=try parallel:\n    work=spawn "
                  "update(box(make()))\n    first=try await work\n    again=try await work\n    "
                  "again\n  result.row.names\n");
    if (!good.ok())
      for (const auto &d : good.diagnostics)
        std::cerr << renderDiagnostic(*good.source, d);
    require(good.ok(), "exported aliases/fields/signatures rejected");
    require(good.module->records.size() == 3, "type aliases duplicated declarations");
    require(bool(continuation::plan(*good.module)), "type export task planning failed");
#ifndef KALANDRA_TYPE_EXPORT_FRONTEND_ONLY
    auto lowered = lowerNative(*good.module);
    require(bool(lowered), lowered ? "" : lowered.error());
#endif
    for (const auto &source :
         {"from \"./types.k\" import Secret\nfn main():\n  0\n",
          "from \"./alias.k\" import Renamed\nfn main():\n  0\n",
          "from \"./types.k\" import Row\ntype Row:\n  n:int\nfn main():\n  0\n",
          "from \"./types.k\" import Row\nfn Row():\n  0\nfn main():\n  0\n",
          "from \"./types.k\" import Row, Row\nfn main():\n  0\n",
          "from \"./types.k\" import Row, make as Row\nfn main():\n  0\n",
          "from \"./types.k\" import make as Row, Row\nfn main():\n  0\n",
          "from \"./types.k\" import Row as String\nfn main():\n  0\n",
          "from \"./types.k\" import Row as Tensor\nfn main():\n  0\n",
          "from \"./types.k\" import Row as int64\nfn main():\n  0\n",
          "from \"./types.k\" import Row as with\nfn main():\n  0\n",
          "from \"./types.k\" import Row as A, make\nfrom \"./other.k\" import Row "
          "as B\nfn use(value:B)->B:\n  value\nfn main():\n  use(make()).value\n",
          "from \"./types.k\" import make\nfn use(value:Row):\n  value\nfn main():\n  "
          "0\n"}) {
      auto bad = analyze(source);
      require(!bad.ok(), "invalid visibility/identity/collision admitted");
    }
    write("private-nested.k",
          "export type Outer:\n  inner:Inner\ntype Inner:\n  flag:Bool\nexport fn make()->Outer:\n "
          " Outer {inner:Inner {flag:true}}\n");
    auto privateField =
        analyze("from \"./private-nested.k\" import Outer, make\nfn "
                "use(value:Outer)->Bool:\n  value.inner.flag\nfn main():\n  use(make())\n");
    require(privateField.ok(), "private nested field lost its declaration context");
    // Type-only diamonds and symlinks still refer to the canonical declaration.
    std::error_code error;
    fs::remove(root / "symlink.k", error);
    fs::create_symlink(root / "types.k", root / "symlink.k");
    auto diamond =
        analyze("from \"./types.k\" import Row as A\nfrom \"./symlink.k\" import Row "
                "as B\nfn same(x:A)->B:\n  x\nfn main():\n  same(A {names:[],value:1}).value\n");
    require(diamond.ok() && diamond.module->records.size() == 2, "canonical type identity lost");
    auto parsed = parse(std::make_shared<SourceFile>(
        "export.k", "export type Row:\n  n:int\nfn main():\n  0\n"));
    require(parsed.ok() && parsed.module.records[0].exported, "export flag missing");
    write("cycle.k", "from \"./main.k\" import Box\nexport type Item:\n  n:int\n");
    auto cycle = analyze(
        "from \"./cycle.k\" import Item\nexport type Box:\n  item:Item\nfn main():\n  0\n");
    require(!cycle.ok(), "type-only cycle admitted");
    write("loop.k",
          "from \"./types.k\" import Row\nexport type Loop:\n  next:Loop\n");
    require(!analyze("from \"./loop.k\" import Loop\nfn main():\n  0\n").ok(),
            "exported recursive layout admitted");
    std::cout << "type exports: aliases, nominal identity, field/signature resolution, private "
                 "names, collision order, canonical graphs and cycles passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
