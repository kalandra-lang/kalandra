#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
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
static fs::path root;
static void write(const std::string &name, const std::string &text) {
  fs::create_directories((root / name).parent_path());
  std::ofstream stream(root / name);
  stream << text;
  stream.close();
  require(bool(stream), "cannot write test source");
}
static CheckResult analyze(const std::string &name = "main.k", SyntaxLimits syntax = {},
                           ModuleLimits modules = {}) {
  auto loaded = loadModules((root / name).string(), syntax, modules);
  require(bool(loaded), "module loader failed without diagnostics");
  return check(std::move(*loaded));
}
static void reject(const std::string &code, const std::string &file = "main.k") {
  auto result = analyze(file);
  require(!result.ok(), "invalid module graph admitted");
  bool found = false;
  for (const auto &diagnostic : result.diagnostics)
    found |= diagnostic.code == code;
  require(found, "expected module diagnostic " + code);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  root = fs::absolute(argv[1]);
  try {
    write("lib/numbers.k",
          "fn helper(value:int):\n  value*2\nexport fn double(value:int):\n  helper(value)\n");
    write("lib/math.k", "from \"./numbers.k\" import double as twice\n"
                        "fn helper(value:int):\n  value+1\nexport fn total(value:int):\n  "
                        "twice(value)+helper(value)\n");
    write("main.k", "from \"./lib/math.k\" import total\n"
                    "from \"./lib/numbers.k\" import double as twice, double as other\n"
                    "fn helper(value:int):\n  value+100\nfn main():\n"
                    "  values=map([1,2],twice)\n"
                    "  captured=map(values,fn(x):other(x)+total(x))\n"
                    "  captured[0]+captured[1]+helper(3)\n");
    auto loaded = loadModules((root / "main.k").string());
    require(bool(loaded) && loaded->ok() && loaded->module.source->parts().size() == 3 &&
                loaded->module.functions.size() == 6,
            "shared dependency was duplicated");
    auto typed = analyze();
    require(typed.ok(), "valid module graph failed checking");
    require(bool(continuation::plan(*typed.module)),
            "imported callbacks failed continuation planning");
#ifndef KALANDRA_MODULE_FRONTEND_ONLY
    auto lowered = lowerNative(*typed.module);
    require(bool(lowered), lowered ? "" : lowered.error());
    require(lowered->mlir.find((root / "lib/numbers.k").string()) != std::string::npos,
            "MLIR lost original module file location");
#endif
    // Canonical identity deduplicates symlink spellings as well as diamonds.
    std::error_code error;
    fs::remove(root / "alias.k", error);
    fs::create_symlink(root / "lib/numbers.k", root / "alias.k");
    write("alias-main.k", "from \"./alias.k\" import double as a\n"
                          "from \"./lib/numbers.k\" import double as b\nfn main():\n  a(1)+b(2)\n");
    auto aliased = loadModules((root / "alias-main.k").string());
    require(bool(aliased) && aliased->ok() && aliased->module.source->parts().size() == 2,
            "canonical source identity lost");
    require(check(std::move(*aliased)).ok(), "symlink aliases failed checking");
    write("nested/parent-main.k",
          "from \"../lib/numbers.k\" import double\nfn main():\n  double(3)\n");
    require(analyze("nested/parent-main.k").ok(), "parent-relative module path failed");
    write("café.k", "export fn increment(value:int):\n  value+1\n");
    write("unicode-main.k", "from \"./café.k\" import increment\nfn main():\n  increment(1)\n");
    require(analyze("unicode-main.k").ok(), "UTF-8 module path failed");
    for (auto limits : {ModuleLimits{1, 32}, ModuleLimits{128, 0}}) {
      auto limited = analyze("main.k", {}, limits);
      require(!limited.ok() && limited.diagnostics[0].code == "M004",
              "module graph limit bypassed");
    }
    SyntaxLimits syntax;
    syntax.maxSourceBytes = 20;
    require(!analyze("main.k", syntax).ok(), "module byte limit bypassed");
    syntax = {};
    syntax.maxTokens = 140;
    for (auto file : {"main.k", "lib/math.k", "lib/numbers.k"}) {
      auto single = loadSource((root / file).string());
      require(bool(single) && parse(*single, syntax).ok(),
              "token-limit fixture exceeds per-file limit");
    }
    require(!analyze("main.k", syntax).ok(), "aggregate token limit bypassed");
    // Cached subgraphs must still respect depth through a longer diamond path.
    write("shared-leaf.k", "export fn leaf():\n  1\n");
    write("shared.k", "from \"./shared-leaf.k\" import leaf\nexport fn shared():\n  leaf()\n");
    write("deep.k", "from \"./shared.k\" import shared\nexport fn deep():\n  shared()\n");
    write("deep-main.k", "from \"./shared.k\" import shared\nfrom "
                         "\"./deep.k\" import deep\nfn main():\n  deep()\n");
    require(!analyze("deep-main.k", {}, {128, 2}).ok(), "cached subgraph bypassed depth bound");
    write("bad-private.k", "from \"./lib/numbers.k\" import helper\nfn main():\n  helper(1)\n");
    reject("S067", "bad-private.k");
    write("bad-leak.k", "from \"./lib/numbers.k\" import double\nfn main():\n  helper(1)\n");
    reject("S031", "bad-leak.k");
    for (auto imports : {"double, double", "double as a, double as a", "double as int", "missing",
                         "double as main"}) {
      write("bad-binding.k",
            "from \"./lib/numbers.k\" import " + std::string(imports) + "\nfn main():\n  1\n");
      require(!analyze("bad-binding.k").ok(), "invalid import binding admitted");
    }
    write("reexport.k", "from \"./lib/numbers.k\" import double\n");
    write("bad-reexport.k", "from \"./reexport.k\" import double\nfn main():\n  double(1)\n");
    reject("S067", "bad-reexport.k");
    write("bad-type.k",
          "from \"./lib/numbers.k\" import double\nfn main():\n  double(float32(1))\n");
    reject("S010", "bad-type.k");
    write("shape-library.k", "export fn four(values:Tensor[int,2,2]):\n  reduceSum(values)\n");
    write("bad-shape.k",
          "from \"./shape-library.k\" import four\nfn main():\n  four(tensor([[1,2,3]]))\n");
    reject("S011", "bad-shape.k");
    write("broken.k", "export fn bad(value:int):\n  missing(value)\n");
    write("bad-diagnostic.k", "from \"./broken.k\" import bad\nfn main():\n  bad(1)\n");
    auto broken = analyze("bad-diagnostic.k");
    require(!broken.ok(), "broken dependency admitted");
    auto rendered = renderDiagnostic(*broken.source, broken.diagnostics[0]);
    require(rendered.find((root / "broken.k").string() + ":2:3:") != std::string::npos &&
                rendered.find("missing(value)") != std::string::npos,
            "dependency semantic diagnostic lost original file/line/excerpt");
    write("broken.k", "export fn bad(value:int):\n  tensor([1,2)\n");
    auto parseFailure = analyze("bad-diagnostic.k");
    require(!parseFailure.ok() &&
                renderDiagnostic(*parseFailure.source, parseFailure.diagnostics[0])
                        .find((root / "broken.k").string() + ":2:") != std::string::npos,
            "dependency syntax diagnostic lost original file");
    for (auto path : {"numbers.k", "/tmp/numbers.k", "./numbers", "./bad\\t.k"}) {
      write("bad-path.k", "from \"" + std::string(path) + "\" import double\nfn main():\n  0\n");
      reject("M001", "bad-path.k");
    }
    write("missing.k", "from \"./absent.k\" import double\nfn main():\n  0\n");
    reject("M002", "missing.k");
    write("cycle-a.k", "from \"./cycle-b.k\" import b\nexport fn a():\n  1\n");
    write("cycle-b.k", "from \"./cycle-a.k\" import a\nexport fn b():\n  2\n");
    reject("M003", "cycle-a.k");
    fs::remove(root / "self-alias.k", error);
    fs::create_symlink(root / "self.k", root / "self-alias.k");
    write("self.k", "from \"./self-alias.k\" import main\nexport fn main():\n  1\n");
    reject("M003", "self.k");
    auto unresolved = check(parse(std::make_shared<SourceFile>(
        "memory.k", "from \"./math.k\" import double\nfn main():\n  0\n")));
    require(!unresolved.ok() && unresolved.diagnostics[0].code == "S067",
            "unresolved imports bypassed module loader");
    write("placed-lib.k", "export fn double(value:float32)->float32:\n  value*2\n");
    write("placed-main.k", "from \"./placed-lib.k\" import double\n"
                           "fn main()->float32 ! TaskError:\n  try parallel:\n"
                           "    try await spawn double(2) on devices.gpu()\n");
    auto placed = loadModules((root / "placed-main.k").string());
    require(bool(placed) && check(std::move(*placed), {}, {.placement = true}).ok(),
            "resolved function imports rejected by placement checking");
    write("library-main.k", "export fn main():\n  7\n");
    write("no-entry.k", "from \"./library-main.k\" import main as importedMain\n");
    auto noEntry = analyze("no-entry.k");
    require(noEntry.ok(), "import-only module rejected");
#ifndef KALANDRA_MODULE_FRONTEND_ONLY
    require(!lowerNative(*noEntry.module), "imported main became program entry");
#endif
    write("old.kalandra", "fn main():\n  0\n");
    require(!loadSource((root / "old.kalandra").string()), "retired source extension accepted");
    write("old-import.k", "from \"./old.kalandra\" import main\nfn main():\n  0\n");
    reject("M001", "old-import.k");
    // Standalone exports keep existing source offsets and work without a loader.
    require(
        check(parse(std::make_shared<SourceFile>("standalone.k", "export fn main():\n  1\n"))).ok(),
        "standalone export rejected");
    for (auto source : {"from \"./x.k\" import\n", "from \"./x.k\" import x as\n",
                        "from \"./x.k\" import x,\n", "export type Point\n  x:int\n"})
      require(!parse(std::make_shared<SourceFile>("invalid.k", source)).ok(),
              "malformed import/export syntax accepted");
    std::cout << "modules: named exports/import aliases, scoped callbacks/helpers, canonical "
                 "diamond/symlink identity, graph limits, cycles, private names, exact types, "
                 "original source diagnostics, placement gate and entry isolation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
