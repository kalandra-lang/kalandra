#include "kalandra/StandardLibrary.h"
#include "kalandra/Syntax.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected isolated output directory");
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "library");
    const auto write = [&](std::string_view file, std::string_view text) {
      std::ofstream stream(root / file);
      stream << text;
      require(bool(stream), "cannot write module fixture");
    };
    write("main.k",
          "from \"std/io\" import println, IoError, Line\nfrom \"std/fmt\" import format "
          "as fmt\nfrom \"std/process\" import ExitStatus\nfrom \"std/fs\" import File, "
          "open\nfrom \"std/parse\" import parseI64, ParseError\nfrom \"./helper.k\" import "
          "helper\nfn main()->unit:\n  return\n");
    write("helper.k", "from \"std/io\" import println\nexport fn helper()->unit:\n  return\n");
    auto loaded = loadModules((root / "main.k").string());
    require(loaded && loaded->ok(), "standard imports did not parse");
    const auto &module = loaded->module;
    require(module.standardModules.size() == 7, "standard modules were duplicated or omitted");
    require(!module.standardModules[0], "ordinary root acquired standard identity");
    for (const auto kind : {StandardModule::Io, StandardModule::Format, StandardModule::Process,
                            StandardModule::Filesystem, StandardModule::Parse}) {
      require(std::ranges::count(module.standardModules, std::optional(kind)) == 1,
              "standard module identity not canonical");
    }
    require(module.imports[0].target == module.imports.back().target,
            "repeat standard import did not reuse nominal identity");
    for (const auto &function : module.functions) {
      require(function.moduleIndex < module.standardModules.size(), "function lost module origin");
      const auto origin = module.source->origin(function.name.begin).first;
      if (const auto standard = module.standardModules[function.moduleIndex])
        require(origin->path() == standardModulePath(*standard),
                "standard origin did not survive flattening");
    }
    write("unknown.k", "from \"std/missing\" import absent\nfn main()->unit:\n  return\n");
    const auto missing = loadModules((root / "unknown.k").string());
    require(missing && !missing->ok() && missing->diagnostics.front().code == "M008",
            "unknown standard module not diagnosed");
    require(!parseModulePackage("std=other.k"),
            "standard namespace can be replaced with a package mapping");
    ModuleLimits limits;
    limits.maxFiles = 1;
    const auto bounded = loadModules((root / "main.k").string(), {}, limits);
    require(bounded && !bounded->ok() && bounded->diagnostics.front().code == "M004",
            "standard module bypassed graph resource limits");
    auto forged =
        parse(std::make_shared<SourceFile>(std::string(standardModulePath(StandardModule::Io)),
                                           std::string(standardModuleSource(StandardModule::Io))));
    require(forged.ok() && forged.module.standardModules.empty(),
            "filename granted standard-module authority");
    // Standard identities are shared even when reached through an offline
    // dependency with its own confined root and private package mappings.
    write("entry.k", "from \"library\" import helper\nfrom \"std/io\" import IoError\nfn "
                     "main()->unit:\n  return\n");
    write("library/lib.k", "from \"std/io\" import println\nexport fn helper()->unit:\n  return\n");
    std::vector<PackageNode> graph{
        {"workspace", root.string(), (root / "entry.k").string(), {{"library", 1}}, {}},
        {"library", (root / "library").string(), (root / "library/lib.k").string(), {}, {}}};
    const auto packaged = loadPackageModules(graph);
    require(packaged && packaged->ok(), "standard import from confined package failed");
    require(std::ranges::count(packaged->module.standardModules,
                               std::optional(StandardModule::Io)) == 1,
            "package-created standard error type lost canonical identity");
    std::filesystem::remove_all(root);
    std::cout << "PASS: standard module imports, aliases, origins, canonical identities and graph "
                 "limits\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
