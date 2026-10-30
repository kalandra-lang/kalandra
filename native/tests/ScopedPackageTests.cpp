#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include "kalandra/Project.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef KALANDRA_SCOPED_FRONTEND_ONLY
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#endif
using namespace kalandra;
namespace fs = std::filesystem;
static fs::path root;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void write(const std::string &path, const std::string &text) {
  fs::create_directories((root / path).parent_path());
  std::ofstream out(root / path);
  out << text;
  out.close();
  require(bool(out), "fixture write failed");
}
static ParseResult load(const std::vector<PackageNode> &graph, ModuleLimits limits = {},
                        std::span<const SourceOverlay> overlays = {}, SyntaxLimits syntax = {}) {
  auto parsed = loadPackageModules(graph, syntax, limits, overlays);
  require(bool(parsed), "loader unexpectedly returned infrastructure error");
  return std::move(*parsed);
}
static void reject(const std::vector<PackageNode> &graph, const std::string &code,
                   ModuleLimits limits = {}) {
  auto parsed = load(graph, limits);
  require(!parsed.ok() && parsed.diagnostics[0].code == code, "expected " + code + " rejection");
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    root = fs::absolute(argv[1]);
    fs::create_directories(root);
    for (int version : {1, 2}) {
      const auto dir = "geometry" + std::to_string(version);
      write(dir + "/types.k", "export type Item:\n  value:int64\n");
      write(dir + "/public.k", "export from \"./types.k\" import Item\n"
                               "export fn next(item:Item)->Item:\n  item with {value:item.value+" +
                                   std::to_string(version) + "}\n");
    }
    const std::string telemetry =
        "from \"geometry\" import Item, next\nexport fn compute(n:int64)->int64:\n  "
        "next(Item {value:n}).value\nexport fn take(item:Item)->int64:\n  item.value\n";
    write("telemetry/public.k", telemetry);
    const std::string main =
        "from \"geometry\" import Item, next\nfrom \"telemetry\" import compute\n"
        "fn main()->[int64] ! TaskError:\n  old=try parallel:\n    try await spawn compute(10)\n"
        "  [next(Item {value:10}).value,old]\n";
    write("app/main.k", main);
    write("app/kalandra.toml", "format=1\n[project]\nentry='main.k'\n");
    auto project = loadNativeProject((root / "app/kalandra.toml").string());
    require(bool(project), "workspace manifest failed");
    auto node = [](const std::string &identity, const std::string &dir, const std::string &name,
                   const std::string &version, std::vector<PackageDependency> deps) {
      write(dir + "/kalandra.toml", "format=1\n[package]\nname='" + name + "'\nversion='" +
                                        version + "'\nentry='public.k'\n");
      auto library = loadNativePackage((root / dir / "kalandra.toml").string());
      require(bool(library), library ? "" : library.error());
      return library->node(identity, std::move(deps));
    };
    std::vector<PackageNode> graph = {
        {"workspace",
         (root / "app").string(),
         project->entry,
         {{"geometry", 1}, {"telemetry", 2}},
         project->packages},
        node("geometry@commit2", "geometry2", "geometry", "0.2.0", {}),
        node("telemetry@commit1", "telemetry", "telemetry", "1.0.0", {{"geometry", 3}}),
        node("geometry@commit1", "geometry1", "geometry", "0.1.0", {})};
    auto typed = check(load(graph));
    if (!typed.ok())
      for (const auto &d : typed.diagnostics)
        std::cerr << renderDiagnostic(*typed.source, d);
    require(typed.ok() && typed.module->records.size() == 2,
            "package scopes failed to separate versions");
    require(bool(continuation::plan(*typed.module)), "scoped worker planning failed");
#ifndef KALANDRA_SCOPED_FRONTEND_ONLY
    auto lowered = lowerNative(*typed.module);
    require(bool(lowered), lowered ? "" : lowered.error());
    write("scoped.ll", lowered->llvm);
    for (bool thread : {false, true}) {
      llvm::LLVMContext context;
      llvm::SMDiagnostic diagnostic;
      auto module = llvm::parseAssemblyString(lowered->llvm, diagnostic, context);
      require(bool(module), "generated LLVM parse failed");
      for (auto &function : *module)
        if (!function.isDeclaration())
          function.addFnAttr(thread ? llvm::Attribute::SanitizeThread
                                    : llvm::Attribute::SanitizeAddress);
      require(!llvm::verifyModule(*module, &llvm::errs()), "sanitized LLVM verification failed");
      std::string ir;
      llvm::raw_string_ostream stream(ir);
      module->print(stream, nullptr);
      write(thread ? "scoped-thread-sanitized.ll" : "scoped-sanitized.ll", ir);
    }
#endif
    // Different aliases of one graph node must share both facade and record identity.
    auto aliases = graph;
    aliases[0].dependencies.push_back({"alias", 1});
    write("app/main.k", "from \"geometry\" import Item as A\nfrom \"alias\" import Item as B\n"
                        "fn same(a:A)->B:\n  a\nfn main():\n  same(A {value:1}).value\n");
    auto shared = check(load(aliases));
    require(shared.ok() && shared.module->records.size() == 1, "shared node cloned types");
    // Identical physical source files with different owners must NOT merge.
    auto distinct = graph;
    distinct.push_back(graph[1]);
    distinct.back().identity = "other-repository@commit2";
    distinct[0].dependencies.push_back({"alias", 4});
    const std::string same = "from \"geometry\" import Item as A\nfrom \"alias\" import Item as B\n"
                             "fn same(a:A)->B:\n  a\nfn main():\n  same(A {value:1}).value\n";
    write("app/main.k", "from \"geometry\" import Item as A\nfrom \"alias\" import Item as B\n"
                        "fn main():\n  [A {value:1}.value,B {value:2}.value]\n");
    auto separate = load(distinct);
    require(separate.ok() && separate.module.source->parts().size() == 5,
            "same-path owners failed to coexist");
    for (const auto &part : separate.module.source->parts())
      require(fs::exists(part.source->path()), "source part lost its physical filename");
    auto separateTypes = check(std::move(separate));
    require(separateTypes.ok() && separateTypes.module->records.size() == 2,
            "same-path owners did not produce separate records");
    write("app/main.k", same);
    auto mismatch = check(load(distinct));
    require(!mismatch.ok(), "different owners sharing a physical path merged nominal types");
    write("app/main.k", "from \"geometry\" import Item\nfrom \"telemetry\" import take\n"
                        "fn main():\n  take(Item {value:1})\n");
    require(!check(load(graph)).ok(), "cross-version record passed to library");
    write("app/main.k", main);
    // Only the declaring owner's aliases are visible, including local mappings.
    auto privateAlias = graph;
    privateAlias[2].dependencies.clear();
    auto missing = load(privateAlias);
    require(!missing.ok() && missing.diagnostics[0].code == "M005" &&
                renderDiagnostic(*missing.module.source, missing.diagnostics[0])
                        .find((root / "telemetry/public.k").string() + ":1:") != std::string::npos,
            "caller alias leaked or diagnostic lost original location");
    privateAlias[2].localPackages.push_back({"geometry", (root / "geometry1/public.k").string()});
    reject(privateAlias, "M007"); // Local maps cannot bypass a managed root.
    write("telemetry/helper.k", "export fn value()->int64:\n  7\n");
    write("telemetry/public.k",
          "from \"private\" import value\nexport fn compute(n:int64)->int64:\n  value()+n\n");
    privateAlias[2].localPackages = {{"private", (root / "telemetry/helper.k").string()}};
    require(check(load(privateAlias)).ok(), "package-local file mapping failed");
    write("telemetry/public.k", telemetry);
    // Enum families are nominal too, including when two owners reuse one path.
    write("geometry2/enum.k", "export enum Flag:\n  On\n  Off\n");
    auto enumGraph = aliases;
    enumGraph[1].entry = (root / "geometry2/enum.k").string();
    write("app/main.k", "from \"geometry\" import Flag as A\nfrom \"alias\" import Flag as B\n"
                        "fn same(a:A)->B:\n  a\nfn main():\n  same(A.On)\n");
    require(check(load(enumGraph)).ok(), "shared enum family split");
    enumGraph.push_back(enumGraph[1]);
    enumGraph.back().identity = "other-enum-repository";
    enumGraph[0].dependencies.back().target = 4;
    require(!check(load(enumGraph)).ok(), "distinct enum families merged");
    write("app/main.k", main);
    // Scope escape checks use canonical paths, covering symlink escapes too.
    write("telemetry/public.k", "export from \"../geometry1/public.k\" import next\n");
    reject(graph, "M007");
    std::error_code error;
    fs::remove(root / "telemetry/escape.k", error);
    fs::create_symlink(root / "geometry1/public.k", root / "telemetry/escape.k");
    write("telemetry/public.k", "export from \"./escape.k\" import next\n");
    reject(graph, "M007");
    write("telemetry/public.k", telemetry);
    // Physical overlays apply to every instance, without collapsing owner identity.
    std::vector<SourceOverlay> overlays = {
        {(root / "geometry1/types.k").string(), "export type Item:\n  value:int64\n"}};
    require(check(load(graph, {}, overlays)).ok(), "scoped overlay failed");
    write("geometry1/public.k", "export from \"./unsaved.k\" import Item\nexport fn "
                                "next(item:Item)->Item:\n  item with {value:item.value+1}\n");
    overlays = {{(root / "geometry1/unsaved.k").string(), "export type Item:\n  value:int64\n"}};
    require(check(load(graph, {}, overlays)).ok(), "unsaved package overlay failed");
    fs::remove(root / "geometry1/unsaved.k", error);
    write("geometry1/public.k",
          "export from \"./types.k\" import Item\nexport fn next(item:Item)->Item:\n"
          "  item with {value:item.value+1}\n");
    auto malformed = graph;
    malformed[3].dependencies = {{"cycle", 2}};
    reject(malformed, "M007");
    malformed = graph;
    malformed[0].dependencies.push_back({"geometry", 3});
    reject(malformed, "M007");
    malformed = graph;
    malformed[2].localPackages = {{"geometry", "unused.k"}};
    reject(malformed, "M007");
    malformed = graph;
    malformed[1].identity = malformed[3].identity;
    reject(malformed, "M007");
    malformed = graph;
    malformed[1].dependencies = {{"missing", 99}};
    reject(malformed, "M007");
    malformed[1].dependencies = {{"workspace", 0}};
    reject(malformed, "M007");
    malformed = graph;
    malformed[3].entry = (root / "app/main.k").string();
    reject(malformed, "M007");
    malformed = graph;
    malformed[1].dependencies = {{"unused-cycle", 1}};
    reject(malformed, "M007");
    // A shared leaf first visited on a shorter path cannot hide a deeper route.
    malformed = graph;
    malformed[0].dependencies = {{"leaf", 3}, {"geometry", 1}};
    malformed[1].dependencies = {{"telemetry", 2}};
    reject(malformed, "M004", {128, 2});
    malformed = graph;
    malformed[0].dependencies.clear();
    for (int alias = 0; alias < 129; ++alias)
      malformed[0].dependencies.push_back({"alias" + std::to_string(alias), 1});
    reject(malformed, "M004");
    reject(graph, "M004", {3, 32});
    reject(graph, "M004", {128, 1});
    auto limited = load(graph, {}, {}, {.maxSourceBytes = 30});
    require(!limited.ok() && limited.diagnostics[0].code == "M004", "byte limit ignored");
    require(!loadPackageModules({}), "empty graph accepted");
    std::cout << "scoped packages: private aliases, version identity, shared nodes, workers, "
                 "re-exports, overlays, confinement, malformed graphs and limits passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
