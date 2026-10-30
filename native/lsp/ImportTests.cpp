#include "Hover.h"
#include "SemanticQuery.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace fs = std::filesystem;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto root = fs::absolute(argv[1]);
    fs::create_directories(root);
    const auto write = [&](const char *file, const std::string &text) {
      std::ofstream out(root / file);
      out << text;
      out.close();
      require(bool(out), "fixture write failed");
    };
    const std::string library =
        "export fn scale(value:int):\n  value*2\nexport type Box:\n  value:int\nexport error "
        "Problem:\n  Bad\nexport type Trouble=Problem\n";
    write("library.k", library);
    const std::string facade = "export from \"./library.k\" import scale as publicScale, Box as "
                               "PublicBox, Trouble as PublicError\n";
    write("facade.k", facade);
    const std::string caller = "from \"./facade.k\" import publicScale as twice, PublicBox as "
                               "LocalBox, PublicError as LocalError\nfn main():\n  twice(21)\n";
    write("main.k", caller);
    const auto path = (root / "main.k").string();
    const auto syntax = [&] {
      auto loaded = loadModules(path);
      require(loaded.has_value(), "load failed");
      return std::move(*loaded);
    };
    auto checked = check(syntax());
    if (!checked.ok())
      for (const auto &d : checked.diagnostics)
        std::cerr << renderDiagnostic(*checked.source, d);
    require(checked.ok(), "import fixture failed checking");
    require(checked.module->imports.size() == 6, "missing resolved imports");
    for (const auto &[word, original] :
         std::vector<std::pair<std::string, std::string>>{{"publicScale", "scale"},
                                                          {"twice", "scale"},
                                                          {"PublicBox", "Box"},
                                                          {"LocalBox", "Box"},
                                                          {"PublicError", "Trouble"},
                                                          {"LocalError", "Trouble"}}) {
      auto target = lsp::definition(syntax(), path, caller.find(word));
      require(target && target->source->path() == (root / "library.k").string() &&
                  target->source->slice(target->span) == original,
              "wrong imported definition for " + word);
      auto refs = lsp::references(syntax(), *target, false);
      require(refs && refs->size() == (original == "scale" ? 5 : 4),
              "wrong imported reference count for " + word);
      auto withDeclaration = lsp::references(syntax(), *target, true);
      require(withDeclaration && withDeclaration->size() == refs->size() + 1,
              "missing original import declaration for " + word);
    }
    auto hover = lsp::hoverInfo(syntax(), path, caller.find("LocalBox"));
    require(hover && hover->text == "type LocalBox", "record alias hover");
    hover = lsp::hoverInfo(syntax(), path, caller.find("twice"));
    require(hover && hover->text == "fn twice(value: int64) -> int64", "function alias hover");
    // Union aliases have declaration identity, but do not pretend to be nominal records.
    require(!lsp::hoverInfo(syntax(), path, caller.find("LocalError")), "invented alias hover");
    auto imported = lsp::definition(syntax(), path, caller.find("twice"));
    const auto alias = std::ranges::find_if(checked.module->imports, [&](const auto &entry) {
      return checked.source->slice(entry.name) == "publicScale";
    });
    require(alias != checked.module->imports.end() && alias->alias && alias->function &&
                !alias->record && checked.source->slice(*alias->alias) == "twice",
            "alias metadata lost");
    require(imported.has_value(), "function target absent");
    // Import declarations without uses still remain references, including re-exports.
    write("main.k", caller.substr(0, caller.find("fn main")) + "fn main():\n  0\n");
    auto refs = lsp::references(syntax(), *imported, false);
    require(refs && refs->size() == 4, "unused import origins lost");
    const std::string typed = "from \"./facade.k\" import PublicBox as LocalBox\nfn "
                              "identity(value:LocalBox)->LocalBox:\n  value with "
                              "{value:value.value}\nfn main():\n  identity(LocalBox {value:2})\n";
    write("main.k", typed);
    const auto importedType = lsp::definition(syntax(), path, typed.find("value:LocalBox") + 6);
    require(importedType && importedType->source->slice(importedType->span) == "Box",
            "imported annotation target");
    refs = lsp::references(syntax(), *importedType, false);
    require(refs && refs->size() == 7, "imported annotation/constructor references");
    auto field = lsp::definition(syntax(), path, typed.find("{value:") + 1);
    require(field && field->source->path() == (root / "library.k").string() &&
                field->span.begin == library.find("value:int", library.find("type Box")),
            "imported update field target");
    write("main.k", "from \"./library.k\" import missing\nfn main():\n  0\n");
    require(!lsp::definition(syntax(), path, 27), "invalid import leaked partial navigation");
    std::cout << "Native import origins: functions, records, error aliases, re-exports, alias "
                 "metadata, unused imports and failure isolation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
