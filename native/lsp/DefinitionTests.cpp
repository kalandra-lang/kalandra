#include "SemanticQuery.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::optional<lsp::SourceLocation> query(const std::string &code, std::size_t at) {
  return lsp::definition(parse(std::make_shared<SourceFile>("definition.k", code)), "definition.k",
                         at);
}
static void expect(const std::string &code, const std::string &use, const std::string &target,
                   std::size_t useWithin = 0, std::size_t targetWithin = 0) {
  const auto at = code.find(use), destination = code.find(target) + targetWithin;
  require(at != code.npos && code.find(target) != code.npos, "missing test marker");
  auto location = query(code, at + useWithin);
  require(location && location->source->path() == "definition.k" &&
              location->span.begin == destination,
          "definition mismatch for " + use + ": " +
              (location ? std::to_string(location->span.begin) : "null") +
              " != " + std::to_string(destination));
  require(!location->source->slice(location->span).empty(), "empty target span");
}
int main() {
  try {
    const std::string basic =
        "fn twice(value:int):\n  value*2\nfn main():\n  result=twice(21)\n  result\n";
    expect(basic, "value*2", "value:int");
    expect(basic, "twice(21)", "twice(value");
    expect(basic, "result\n", "result=");
    expect(basic, "twice(value", "twice(value");
    require(!query(basic, basic.find("21")), "literal has no source definition");
    require(!query(basic, basic.find("  result")), "whitespace has no definition");
    require(!query(basic, 0), "keyword has no definition");
    require(!query(basic, basic.size()), "EOF has no definition");
    const std::string alias =
        "fn twice(value:int):\n  value*2\nfn main():\n  transform=twice\n  transform(21)\n";
    expect(alias, "twice\n", "twice(value");
    expect(alias, "transform(21)", "transform=");
    const std::string pipeline = "fn twice(value:int):\n  value*2\nfn main():\n  21 -> twice\n";
    expect(pipeline, "twice\n", "twice(value");
    const std::string closure =
        "fn main():\n  offset=1\n  transform=fn(x:int):x+offset\n  transform(2)\n";
    expect(closure, "offset\n", "offset=");
    expect(closure, "x+offset", "x:int");
    const std::string nested = "fn outer(value:int):\n  factory=fn():fn(x:int):x+value\n  "
                               "factory()(2)\nfn main():\n  outer(1)\n";
    expect(nested, "value\n", "value:int");
    const std::string shadow =
        "fn main()->int:\n  value=1\n  if true:\n    value=2\n    value\n  else:\n    value\n";
    expect(shadow, "    value\n", "value=2", 4);
    auto outer = query(shadow, shadow.rfind("value"));
    require(outer && outer->span.begin == shadow.find("value=1"),
            "outer branch binding was shadowed");
    const std::string record =
        "type Box:\n  value:int\nfn main():\n  box=Box {value:42}\n  box.value\n";
    expect(record, "box.value", "value:int", 4);
    expect(record, "box.value", "box=", 0);
    const std::string builtin = "fn main():\n  length([1,2])\n";
    require(check(parse(std::make_shared<SourceFile>("definition.k", builtin))).ok(),
            "builtin fixture invalid");
    require(!query(builtin, builtin.find("length")), "builtin wrapper guessed a source definition");
    const std::string failed = "fn main():\n  missing\n";
    require(!query(failed, failed.find("main")), "failed graph leaked partial definition");
    std::cout
        << "Native definitions: resolved bindings, calls, references, pipelines, nested "
           "captures, shadowing, record fields, builtin rejection and failure isolation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
