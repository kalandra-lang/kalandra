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
static void write(const std::string &file, const std::string &text) {
  std::ofstream out(root / file);
  out << text;
  out.close();
  require(bool(out), "write failed");
}
static CheckResult analyze(const std::string &text, SemanticLimits limits = {}) {
  write("main.k", text);
  auto loaded = loadModules((root / "main.k").string());
  require(bool(loaded), "loader failed");
  return check(std::move(*loaded), limits);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    root = fs::absolute(argv[1]);
    fs::create_directories(root);
    write("core.k", "export type Row:\n  names:[String]\n  value:int64\ntype Secret:\n  "
                           "flag:Bool\nfn helper(n:int64)->int64:\n  n+1\nexport fn make()->Row:\n "
                           " Row {names:[\"é\"],value:2}\nexport fn step(row:Row)->Row:\n  row "
                           "with {value:helper(row.value)}\n");
    write("facade.k",
          "export from \"./core.k\" import Row as Item, make as create, step as advance\n");
    write("public.k",
          "export from \"./facade.k\" import Item as Entry, create, advance\n");
    write("private.k", "from \"./core.k\" import Row as Item, make\n");
    write("other.k", "export type Row:\n  names:[String]\n  value:int64\n");
    const std::string imports = "from \"./public.k\" import Entry, create, advance\nfrom "
                                "\"./core.k\" import Row\n";
    auto good = analyze(
        imports + "fn local(value:Entry)->Row:\n  advance(value)\nfn main()->int64 ! TaskError:\n  "
                  "changed=try parallel:\n    work=spawn local(create())\n    first=try await "
                  "work\n    again=try await work\n    again\n  changed.value\n");
    if (!good.ok())
      for (const auto &d : good.diagnostics)
        std::cerr << renderDiagnostic(*good.source, d);
    require(good.ok() && good.module->records.size() == 2 && good.module->functions.size() == 5,
            "reexports changed nominal/function identity");
    require(bool(continuation::plan(*good.module)), "reexport planning failed");
#ifndef KALANDRA_REEXPORT_FRONTEND_ONLY
    auto lowered = lowerNative(*good.module);
    require(bool(lowered), lowered ? "" : lowered.error());
#endif
    auto constructors =
        analyze("from \"./public.k\" import Entry\nexport type Box:\n  row:Entry\nfn "
                "main()->[String]:\n  Box {row:Entry {names:[\"kept\"],value:4}}.row.names\n");
    require(constructors.ok(), "reexported constructors/fields rejected");
    for (const auto &source :
         {"from \"./facade.k\" import Row\nfn main():\n  0\n",
          "from \"./private.k\" import Item\nfn main():\n  0\n",
          "from \"./private.k\" import make\nfn main():\n  0\n",
          "export from \"./core.k\" import Secret\nfn main():\n  0\n",
          "export from \"./core.k\" import helper\nfn main():\n  0\n",
          "export from \"./core.k\" import missing\nfn main():\n  0\n",
          "export from \"./core.k\" import Row, Row\nfn main():\n  0\n",
          "export from \"./core.k\" import make, make\nfn main():\n  0\n",
          "export from \"./core.k\" import Row as Name, make as Name\nfn main():\n  0\n",
          "export from \"./core.k\" import make as Name, Row as Name\nfn main():\n  0\n",
          "export from \"./core.k\" import Row\ntype Row:\n  value:int\nfn main():\n  0\n",
          "export from \"./core.k\" import make\nfn make():\n  0\nfn main():\n  0\n",
          "export from \"./core.k\" import Row as int64\nfn main():\n  0\n",
          "export from \"./core.k\" import make as Bool\nfn main():\n  0\n",
          "from \"./public.k\" import Entry, create\nfrom \"./other.k\" import "
          "Row\nfn accept(value:Row)->Row:\n  value\nfn main():\n  accept(create()).value\n"})
      require(!analyze(source).ok(), "invalid/private/colliding reexport admitted");
    auto local = analyze("type Box:\n  row:Entry\nexport from \"./public.k\" import Entry, "
                         "create, advance\nfn main():\n  Box {row:advance(create())}.row.value\n");
    require(local.ok(), "local reexport use or order-independent field lookup failed");
    write("entry.k", "export fn main():\n  7\n");
    write("entry-public.k", "export from \"./entry.k\" import main\n");
    auto noEntry = analyze("from \"./entry-public.k\" import main\n");
    require(noEntry.ok(), "reexport-only entry fixture rejected");
#ifndef KALANDRA_REEXPORT_FRONTEND_ONLY
    require(!lowerNative(*noEntry.module), "reexported dependency main became program entry");
#endif
    auto placed = loadModules((root / "main.k").string());
    // Placement-aware checking also accepts ordinary CPU module graphs. This
    // fixture contains no GPU dispatch and does not qualify records on device.
    require(bool(placed) && check(std::move(*placed), {}, {.placement = true}).ok(),
            "placement-aware checking rejected an ordinary CPU reexport graph");
    // A diamond through two public facades still ends at one original declaration.
    write("second.k",
          "export from \"./core.k\" import Row as Other, make as build\n");
    auto diamond =
        analyze("from \"./second.k\" import Other, build\nfrom \"./public.k\" import "
                "Entry\nfn same(row:Other)->Entry:\n  row\nfn main():\n  same(build()).value\n");
    require(diamond.ok() && diamond.module->records.size() == 2, "diamond cloned original type");
    write("cycle.k", "export from \"./main.k\" import main\n");
    require(!analyze("export from \"./cycle.k\" import main\n").ok(),
            "reexport cycle admitted");
    for (const auto &text : {"export from \"./core.k\" import\n",
                             "export from \"./core.k\" import make as\n",
                             "export from \"./core.k\" import make,\n"})
      require(!parse(std::make_shared<SourceFile>("bad.k", text)).ok(),
              "malformed reexport parsed");
    auto parsed = parse(std::make_shared<SourceFile>(
        "memory.k", "export from \"./core.k\" import Row\n"));
    require(parsed.ok() && parsed.module.imports[0].exported, "export import flag lost");
    require(!check(std::move(parsed)).ok(), "unloaded reexport admitted");
    // Even a forged cyclic graph must stop rather than recurse indefinitely.
    auto forged = parse(std::make_shared<SourceFile>(
        "cycle.k", "export from \"./self.k\" import X\nfn main():\n  0\n"));
    forged.module.imports[0].target = 0;
    require(!check(std::move(forged)).ok(), "forged reexport cycle admitted");
    std::cout << "reexports: functions/types/aliases, nominal identity, local helper scope, "
                 "collisions, privacy, diamonds and bounded cycles passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
