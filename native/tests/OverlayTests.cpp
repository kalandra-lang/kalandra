#include "kalandra/Diagnostics.h"
#include "kalandra/Semantics.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
    const auto main = (root / "main.k").string();
    const auto lib = (root / "café.k").string();
    const std::string caller = "from \"./café.k\" import value\nfn main()->int:\n  value()\n";
    const std::string good = "export fn value()->int:\n  42\n";
    std::ofstream(main) << caller;
    std::ofstream(lib) << good;
    const auto analyze = [&](std::span<const SourceOverlay> overlays, SyntaxLimits syntax = {},
                             ModuleLimits modules = {},
                             std::span<const ModulePackage> packages = {}) {
      auto parsed = loadModules(main, syntax, modules, packages, overlays);
      require(bool(parsed), "loader failed without structured diagnostics");
      return check(std::move(*parsed));
    };
    const auto rejects = [&](std::span<const SourceOverlay> overlays, const char *code,
                             SyntaxLimits syntax = {}, ModuleLimits modules = {}) {
      auto result = analyze(overlays, syntax, modules);
      require(!result.ok() && result.diagnostics[0].code == code, std::string("expected ") + code);
    };
    require(analyze({}).ok(), "disk baseline failed");
    std::vector<SourceOverlay> overlays{{lib, "export fn value()->String:\n  \"😀é\"\n"}};
    require(!analyze(overlays).ok(), "unsaved export signature did not recheck importer");
    overlays.push_back({main, "from \"./café.k\" import value\nfn main()->String:\n  value()\n"});
    auto snapshot = analyze(overlays);
    require(snapshot.ok(), "multiple unsaved buffers did not compose");
    overlays.clear();
    require(snapshot.source->text().find("😀é") != std::string::npos,
            "analysis did not own replacement source lifetime");
    auto disk = analyze({});
    require(disk.ok() && disk.source->text().find("😀é") == std::string::npos,
            "unsaved contents leaked into subsequent analysis");
    std::ifstream originalMain(main), originalLib(lib);
    require(std::string(std::istreambuf_iterator<char>(originalMain), {}) == caller &&
                std::string(std::istreambuf_iterator<char>(originalLib), {}) == good,
            "unsaved contents were written to disk");
    overlays = {{lib, "export fn value()->String:\n  \"😀é\" + missing\n"}};
    auto broken = analyze(overlays);
    auto json = renderDiagnosticsJson(broken.source.get(), broken.diagnostics);
    require(!broken.ok() && json.find("café.k") != std::string::npos &&
                json.find("\"character\":10") != std::string::npos,
            "unsaved dependency lost original Unicode position");
    overlays = {{main, "from \"./new/sub.k\" import value\nfn main()->int:\n  value()\n"},
                {(root / "new/sub.k").string(), good}};
    require(analyze(overlays).ok(), "new unsaved dependency failed");
    require(!fs::exists(root / "new/sub.k"), "overlay wrote an unsaved file");
    const auto newRoot = (root / "unsaved.k").string();
    const SourceOverlay virtualRoot{newRoot, "fn main()->int:\n  1\n"};
    auto virtualParsed = loadModules(newRoot, {}, {}, {}, {&virtualRoot, 1});
    require(virtualParsed && check(std::move(*virtualParsed)).ok(), "new unsaved root failed");
    require(!fs::exists(newRoot), "unsaved root was written");
    overlays = {{main, "from \"math\" import value\nfn main()->int:\n  value()\n"},
                {(root / "package.k").string(), good}};
    const ModulePackage package{"math", (root / "package.k").string()};
    require(analyze(overlays, {}, {}, {&package, 1}).ok(), "unsaved package entry failed");
    overlays = {{lib, good}, {(root / "./café.k").string(), good}};
    rejects(overlays, "M006");
    std::error_code error;
    fs::remove(root / "alias.k", error);
    fs::create_symlink(lib, root / "alias.k");
    overlays = {{(root / "alias.k").string(), good}, {lib, good}};
    rejects(overlays, "M006");
    overlays = {{(root / "alias.k").string(), "export fn value()->int:\n  missing\n"}};
    rejects(overlays, "S016");
    overlays = {{main, "from \"./café.k\" import value\nexport fn main()->int:\n  value()\n"},
                {lib, "from \"./main.k\" import main\nexport fn value()->int:\n  main()\n"}};
    rejects(overlays, "M003");
    rejects(overlays, "M004", {}, ModuleLimits{1, 32});
    rejects(overlays, "M004", {}, ModuleLimits{128, 0});
    SyntaxLimits small;
    small.maxSourceBytes = good.size();
    overlays = {{lib, good}};
    rejects(overlays, "M004", small); // Includes the graph separator byte.
    overlays = {{lib, "export fn value()->int:\n  1\n"}};
    small.maxSourceBytes = overlays[0].text.size() + 1;
    rejects(overlays, "M004", small); // Overlay fits, full disk/overlay graph does not.
    overlays = {{(root / "unused.k").string(), good}};
    rejects(overlays, "M004", small); // Unreachable buffers still obey snapshot budgets.
    overlays = {{"bad.txt", good}};
    rejects(overlays, "M006");
    overlays = {{std::string("bad\0.k", 6), good}};
    rejects(overlays, "M006");
    overlays = {{lib, ""}};
    rejects(overlays, "P001");
    overlays = {{lib, std::string(1, char(255))}};
    rejects(overlays, "L002");
    require(analyze({}).ok(), "failed snapshot poisoned subsequent disk check");
    std::cout << "Source overlays: unsaved roots/dependencies/packages, signatures, ownership, "
                 "Unicode ranges, aliases, cycles and limits passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
