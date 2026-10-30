#include "Rename.h"
#include "Workspace.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
using namespace kalandra;
namespace fs = std::filesystem;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Fixture {
  fs::path root;
  std::map<std::string, std::string> text;
  Fixture(fs::path base, std::string name, std::map<std::string, std::string> files)
      : root(base / name), text(std::move(files)) {
    fs::remove_all(root);
    fs::create_directories(root);
    for (const auto &[file, source] : text) {
      fs::create_directories((root / file).parent_path());
      std::ofstream out(root / file);
      out << source;
      out.close();
      require(bool(out), "fixture write");
    }
    root = fs::canonical(root);
  }
  auto plan(std::string file, std::size_t at, std::optional<std::string> name) {
    auto roots = lsp::discoverSources(root.string());
    require(roots.has_value(), "discovery");
    return lsp::planRename(*roots, root.string(), {}, {}, (root / file).string(), at,
                           std::move(name));
  }
  std::map<std::string, std::string> apply(const lsp::RenameResult &result) {
    auto output = text;
    for (auto edit = result.edits.rbegin(); edit != result.edits.rend(); ++edit) {
      const auto file = fs::path(edit->location.source->path()).lexically_relative(root).string();
      auto &source = output.at(file);
      source.replace(edit->location.span.begin, edit->location.span.end - edit->location.span.begin,
                     edit->text);
    }
    for (const auto &[file, source] : text) {
      auto disk = loadSource((root / file).string());
      require(disk && (*disk)->text() == source, "rename wrote a source file");
    }
    return output;
  }
  auto success(std::string file, std::size_t at, std::string name) {
    auto result = plan(file, at, name);
    require(result.has_value(), result ? "" : result.error());
    require(result->selection.has_value(), "missing rename selection");
    return apply(*result);
  }
};
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto base = fs::absolute(argv[1]);
    fs::create_directories(base);
    const std::string local = "fn main():\n  value=1\n  read=fn():value\n  read()+value\n";
    Fixture locals(base, "locals", {{"main.k", local}});
    auto prepared = locals.plan("main.k", local.find("value"), std::nullopt);
    require(prepared && prepared->selection && prepared->placeholder == "value" &&
                prepared->edits.empty(),
            "prepare rename");
    auto result = locals.success("main.k", local.find("value"), "amount");
    require(result["main.k"] == "fn main():\n  amount=1\n  read=fn():amount\n  read()+amount\n",
            "captured local edits");
    auto noChange = locals.plan("main.k", local.find("value"), "value");
    require(noChange && noChange->edits.empty(), "same-name rename");
    require(!locals.plan("main.k", local.find("main"), "entry")->selection,
            "main should not be renamed");
    require(!locals.plan("main.k", 0, "entry")->selection, "keyword rename");
    for (auto name : {"", "fn", "float32", "main", "_", "9name", "a b", "é"})
      require(!lsp::validRenameName(name), "invalid name admitted");
    require(lsp::validRenameName("_value2"), "valid identifier rejected");
    const std::string keywordBinding =
        "error Input:\n  Bad\nfn inspect(error:int)->int:\n  error\nfn main():\n  inspect(1)\n";
    Fixture keywordName(base, "keyword-binding", {{"main.k", keywordBinding}});
    result = keywordName.success("main.k", keywordBinding.find("error:int"), "problem");
    require(result["main.k"].find("error Input:") != std::string::npos &&
                result["main.k"].find("inspect(problem:int)") != std::string::npos &&
                result["main.k"].find("  problem\n") != std::string::npos,
            "keyword spelling confused a lexical binding");
    Fixture collision(base, "collision",
                      {{"main.k", "fn main():\n  value=1\n  other=2\n  value+other\n"}});
    require(!collision.plan("main.k", collision.text["main.k"].find("value"), "other"),
            "duplicate binding admitted");
    const std::string capture =
        "fn main():\n  value=1\n  read=fn(other:int):value+other\n  read(2)\n";
    Fixture captures(base, "capture", {{"main.k", capture}});
    auto failed = captures.plan("main.k", capture.find("value"), "other");
    require(!failed && failed.error().find("resolution") != std::string::npos,
            "silent capture admitted");
    const std::string reverse = "fn outer(other:int)->int:\n  read=fn():\n    value=1\n    "
                                "value+other\n  read()\nfn main():\n  outer(2)\n";
    Fixture reverseCapture(base, "reverse-capture", {{"main.k", reverse}});
    require(!reverseCapture.plan("main.k", reverse.find("value"), "other"),
            "capture of unrelated reference admitted");
    const std::string pattern =
        "enum Box:\n  Some:\n    value:int\nfn read(box:Box)->int:\n  match box:\n    Box.Some "
        "{value}:value\nfn main():\n  read(Box.Some {value:2})\n";
    Fixture patterns(base, "patterns", {{"main.k", pattern}});
    result = patterns.success("main.k", pattern.find("value:int"), "amount");
    require(result["main.k"].find("{amount:value}:value") != std::string::npos &&
                result["main.k"].find("{amount:2}") != std::string::npos,
            "field rename failed to preserve shorthand binding");
    result = patterns.success("main.k", pattern.find("{value}") + 1, "amount");
    require(result["main.k"].find("{value:amount}:amount") != std::string::npos &&
                result["main.k"].find("{value:2}") != std::string::npos,
            "binding rename changed shorthand field");
    result = patterns.success("main.k", pattern.find("Some:"), "Item");
    require(result["main.k"].find("Box.Item {value}:value") != std::string::npos, "case rename");
    const std::string builtin =
        "fn explain(error:TaskError)->int:\n  match error:\n    TaskError.Fault "
        "{message}:length(message)\n    _:0\nfn main():\n  0\n";
    Fixture builtins(base, "builtins", {{"main.k", builtin}});
    result = builtins.success("main.k", builtin.find("{message}") + 1, "detail");
    require(result["main.k"].find("{message:detail}:length(detail)") != std::string::npos,
            "builtin shorthand binding rename");
    const std::string library = "export fn scale(value:int):\n  value*2\n";
    const std::string facade = "export from \"./lib.k\" import scale as publicScale\n";
    const std::string relay = "export from \"./facade.k\" import publicScale\n";
    const std::string caller =
        "from \"./relay.k\" import publicScale as localScale\nfn main():\n  localScale(2)\n";
    const std::string direct =
        "from \"./relay.k\" import publicScale\nfn main():\n  publicScale(2)\n";
    const std::string sameAlias =
        "from \"./lib.k\" import scale as scale\nfn main():\n  scale(2)\n";
    Fixture aliases(base, "aliases",
                    {{"lib.k", library},
                     {"facade.k", facade},
                     {"relay.k", relay},
                     {"main.k", caller},
                     {"direct.k", direct},
                     {"same.k", sameAlias}});
    result = aliases.success("lib.k", library.find("scale"), "amplify");
    require(result["lib.k"].find("fn amplify(") != std::string::npos &&
                result["facade.k"].find("import amplify as publicScale") != std::string::npos,
            "upstream export rename");
    require(result["main.k"] == caller && result["direct.k"] == direct &&
                result["relay.k"] == relay,
            "explicit re-export alias not preserved");
    require(result["same.k"].find("import amplify as scale") != std::string::npos &&
                result["same.k"].find("  scale(2)") != std::string::npos,
            "identical-spelling explicit alias not preserved");
    result = aliases.success("facade.k", facade.find("publicScale"), "published");
    require(result["lib.k"] == library &&
                result["relay.k"].find("import published") != std::string::npos &&
                result["main.k"].find("import published as localScale") != std::string::npos &&
                result["direct.k"].find("  published(2)") != std::string::npos,
            "re-export alias propagation");
    result = aliases.success("main.k", caller.rfind("localScale"), "transform");
    require(result["main.k"].find("publicScale as transform") != std::string::npos &&
                result["main.k"].find("  transform(2)") != std::string::npos &&
                result["lib.k"] == library && result["direct.k"] == direct,
            "local alias rename");
    Fixture records(base, "records",
                    {{"lib.k", "export type Box:\n  value:int\n"},
                     {"main.k", "from \"./lib.k\" import Box as B\nfn read(box:B)->int:\n  "
                                "box.value\nfn main():\n  read(B {value:2})\n"}});
    result = records.success("lib.k", records.text["lib.k"].find("Box"), "Container");
    require(result["main.k"].find("import Container as B") != std::string::npos &&
                result["main.k"].find("box:B") != std::string::npos,
            "nominal alias preservation");
    result = records.success("lib.k", records.text["lib.k"].find("value"), "amount");
    require(result["main.k"].find("box.amount") != std::string::npos &&
                result["main.k"].find("B {amount:2}") != std::string::npos,
            "imported field rename");
    std::string repeated = "fn main():\n  value=1\n  [";
    for (int i = 0; i < 2000; ++i)
      repeated += (i ? ",value" : "value");
    repeated += "]\n";
    Fixture many(base, "many", {{"main.k", repeated}});
    auto large = many.plan("main.k", repeated.find("value"), "v");
    require(large && large->edits.size() == 2001, "many-reference rename offsets");
    result = many.apply(*large);
    require(result["main.k"].find("value") == std::string::npos,
            "many-reference rename incomplete");
    Fixture directExports(
        base, "direct-exports",
        {{"lib.k", library},
         {"wrapper.k", "export from \"./lib.k\" import scale\n"},
         {"main.k", "from \"./wrapper.k\" import scale\nfn main():\n  scale(2)\n"}});
    result =
        directExports.success("main.k", directExports.text["main.k"].rfind("scale"), "amplify");
    require(result["wrapper.k"].find("import amplify") != std::string::npos &&
                result["main.k"].find("  amplify(2)") != std::string::npos &&
                result["lib.k"].find("fn amplify(") != std::string::npos,
            "transparent export chain rename");
    std::map<std::string, std::string> projectFiles{{"lib.k", library}};
    for (int i = 0; i < 130; ++i)
      projectFiles.emplace("caller" + std::to_string(i) + ".k",
                           "from \"./lib.k\" import scale\nfn main():\n  scale(2)\n");
    Fixture manyFiles(base, "many-files", std::move(projectFiles));
    auto acrossFiles = manyFiles.plan("lib.k", library.find("scale"), "amplify");
    require(acrossFiles && acrossFiles->edits.size() == 261,
            "workspace rename incorrectly used a single graph overlay budget");
    const std::string grouped =
        "fn double(value:int):\n  value*2\nfn main():\n  transform=(double)\n  transform(2)\n";
    Fixture unmapped(base, "unmapped", {{"main.k", grouped}});
    failed = unmapped.plan("main.k", grouped.find("double"), "twice");
    require(!failed && failed.error().find("unmapped") != std::string::npos,
            "unmapped affected identifier silently ignored");
    Fixture external(base, "external", {{"lib.k", library}});
    Fixture importer(
        base, "importer",
        {{"main.k", "from \"../external/lib.k\" import scale\nfn main():\n  scale(2)\n"}});
    failed = importer.plan("main.k", importer.text["main.k"].rfind("scale"), "twice");
    require(!failed && failed.error().find("outside") != std::string::npos,
            "external declaration edited");
    std::cout << "Native rename: lexical identities, capture rejection, fields/cases, shorthand "
                 "expansion, alias barriers, re-export propagation, unmapped-token rejection and "
                 "source isolation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
