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
  fs::create_directories((root / file).parent_path());
  std::ofstream out(root / file);
  out << text;
  out.close();
  require(bool(out), "write failed");
}
static ParseResult load(const std::string &text, const std::vector<ModulePackage> &packages,
                        ModuleLimits limits = {}) {
  write("app/main.k", text);
  auto result = loadModules((root / "app/main.k").string(), {}, limits, packages);
  require(bool(result), "loader returned no diagnostics");
  return std::move(*result);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    root = fs::absolute(argv[1]);
    fs::create_directories(root);
    write("lib/types.k", "export type Row:\n  names:[String]\n  value:int64\n");
    write("base/math.k", "export fn next(n:int64)->int64:\n  n+1\n");
    write("lib/public.k",
          "export from \"./types.k\" import Row as Item\nfrom \"numbers\" import next\nfn "
          "helper(text:String)->String:\n  text+\"!\"\nexport fn update(item:Item)->Item:\n  item "
          "with {names:map(item.names,fn(text):helper(text)),value:next(item.value)}\n");
    std::vector<ModulePackage> packages = {
        {"library", (root / "lib/public.k").string()},
        {"numbers", fs::relative(root / "base/math.k", fs::current_path()).string()}};
    auto parsed = load("from \"library\" import Item, update\nfn main()->[String] ! TaskError:\n  "
                       "original=Item {names:[\"é\"],value:2}\n  changed=try parallel:\n    try "
                       "await spawn update(original)\n  changed.names\n",
                       packages);
    require(parsed.ok() && parsed.module.source->parts().size() == 4,
            "package graph loading failed");
    auto typed = check(std::move(parsed));
    if (!typed.ok())
      for (const auto &d : typed.diagnostics)
        std::cerr << renderDiagnostic(*typed.source, d);
    require(typed.ok() && typed.module->records.size() == 1, "package type identity lost");
    require(bool(continuation::plan(*typed.module)), "package task planning failed");
#ifndef KALANDRA_LOCAL_PACKAGE_FRONTEND_ONLY
    auto lowered = lowerNative(*typed.module);
    require(bool(lowered), lowered ? "" : lowered.error());
#endif
    std::error_code symlinkError;
    fs::remove(root / "retired.kalandra", symlinkError);
    fs::create_symlink(root / "app/main.k", root / "retired.kalandra");
    auto retired = loadModules((root / "retired.kalandra").string(), {}, {}, packages);
    require(bool(retired) && !retired->ok(), "retired root extension bypassed through symlink");
    auto more = packages;
    more.push_back({"alias", (root / "lib/public.k").string()});
    auto diamond = check(load("from \"library\" import Item as A\nfrom \"alias\" import Item as "
                              "B\nfrom \"../lib/types.k\" import Row\nfn same(a:A)->B:\n  a\nfn "
                              "main():\n  same(Row {names:[],value:1}).value\n",
                              more));
    require(diamond.ok() && diamond.module->records.size() == 1,
            "package aliases/relative diamond cloned identity");
    auto missing = load("from \"unknown\" import anything\nfn main():\n  0\n", packages);
    require(!missing.ok() && missing.diagnostics[0].code == "M005",
            "unknown package guessed a filesystem path");
    for (auto config : {std::vector<ModulePackage>{{"bad.name", "file.k"}},
                        std::vector<ModulePackage>{{"valid", "file.kalandra"}},
                        std::vector<ModulePackage>{{"same", "a.k"}, {"same", "b.k"}}}) {
      auto bad = load("fn main():\n  0\n", config);
      require(!bad.ok() && bad.diagnostics[0].code == "M005", "invalid mapping admitted");
    }
    require(load("fn main():\n  0\n", {{"unused", (root / "missing.k").string()}}).ok(),
            "unused mapping loaded a file");
    auto used = load("from \"missing\" import value\nfn main():\n  0\n",
                     {{"missing", (root / "missing.k").string()}});
    require(!used.ok() && used.diagnostics[0].code == "M002", "missing used entry admitted");
    auto cycle = load("from \"self\" import main\nexport fn main():\n  0\n",
                      {{"self", (root / "app/main.k").string()}});
    require(!cycle.ok() && cycle.diagnostics[0].code == "M003", "package cycle admitted");
    auto limited = load("from \"library\" import Item\nfn main():\n  0\n", packages, {128, 0});
    require(!limited.ok() && limited.diagnostics[0].code == "M004", "package depth ignored");
    auto count = load("fn main():\n  0\n", more, {2, 32});
    require(!count.ok() && count.diagnostics[0].code == "M004", "mapping count unbounded");
    auto files = load("from \"library\" import Item\nfn main():\n  0\n", packages, {2, 32});
    require(!files.ok() && files.diagnostics[0].code == "M004", "package graph file limit ignored");
    auto placed = load("from \"library\" import Item\nfn main():\n  0\n", packages);
    require(!check(std::move(placed), {}, {.placement = true}).ok(),
            "packages enabled GPU modules");
    write("bad.k", "export fn bad():\n  missing()\n");
    auto bad = check(load("from \"broken\" import bad\nfn main():\n  bad()\n",
                          {{"broken", (root / "bad.k").string()}}));
    require(!bad.ok() && renderDiagnostic(*bad.source, bad.diagnostics[0])
                                 .find((root / "bad.k").string() + ":2:") != std::string::npos,
            "package diagnostic lost original location");
    for (const auto *value : {"math=file.k", "_lib-2=/tmp/file=other.k"})
      require(bool(parseModulePackage(value)), "valid CLI mapping rejected");
    for (const auto *value :
         {"math", "=file.k", "x=", "x=file.kalandra", "a.b=file.k", "1x=file.k", "x=file.k\n"})
      require(!parseModulePackage(value), "invalid CLI mapping accepted");
    std::cout
        << "local packages: explicit entries, relative dependencies, transitive mappings, nominal "
           "diamonds, original diagnostics, limits, cycles and no implicit lookup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
