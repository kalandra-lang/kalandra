#include "SemanticQuery.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void expect(const std::string &code, std::size_t at, std::vector<std::size_t> expected,
                   bool include = false) {
  const auto syntax = [&] { return parse(std::make_shared<SourceFile>("references.k", code)); };
  auto target = lsp::definition(syntax(), "references.k", at);
  require(target.has_value(), "missing definition at " + std::to_string(at));
  auto result = lsp::references(syntax(), *target, include);
  require(result.has_value(), "reference graph failed");
  std::vector<std::size_t> offsets;
  for (const auto &location : *result) {
    require(location.source->path() == "references.k", "wrong source");
    offsets.push_back(location.span.begin);
  }
  if (offsets != expected) {
    std::string message = "reference offsets differ: ";
    for (auto offset : offsets)
      message += std::to_string(offset) + ",";
    throw std::runtime_error(message);
  }
}
int main() {
  try {
    const std::string basic =
        "fn twice(value:int):\n  value*2\nfn main():\n  transform=twice\n  transform(21)\n";
    expect(basic, basic.find("value:int"), {basic.find("value*2")});
    expect(basic, basic.find("value*2"), {basic.find("value:int"), basic.find("value*2")}, true);
    expect(basic, basic.find("twice(value"), {basic.find("twice\n")});
    expect(basic, basic.find("transform="), {basic.find("transform(21)")});
    const std::string calls = "fn twice(value:int):\n  value*2\nfn main():\n  twice(21) -> twice\n";
    expect(calls, calls.find("twice(value"), {calls.find("twice(21)"), calls.rfind("twice")});
    const std::string closure = "fn outer(value:int):\n  factory=fn():fn(x:int):x+value\n  "
                                "factory()(2)+value\nfn main():\n  outer(1)\n";
    expect(closure, closure.find("value:int"), {closure.find("value\n"), closure.rfind("value")});
    const std::string shadow =
        "fn main()->int:\n  value=1\n  if true:\n    value=2\n    value\n  else:\n    value\n";
    expect(shadow, shadow.find("value=1"), {shadow.rfind("value")});
    expect(shadow, shadow.find("value=2"), {shadow.find("value\n")});
    const std::string field =
        "type Box:\n  value:int\nfn main():\n  value=Box {value:42}\n  value.value\n";
    expect(field, field.find("value:int"), {field.find("value:42"), field.rfind("value")});
    expect(field, field.find("value=Box"), {field.find("value.value")});
    const std::string unused = "fn unused(value:int):\n  42\nfn main():\n  0\n";
    expect(unused, unused.find("unused"), {});
    expect(unused, unused.find("unused"), {unused.find("unused")}, true);
    auto source = std::make_shared<SourceFile>("references.k", basic);
    const lsp::SourceLocation target{source, {basic.find("twice"), basic.find("twice") + 5}};
    require(
        !lsp::references(parse(std::make_shared<SourceFile>("bad.k", "fn main():\n  unknown\n")),
                         target, true),
        "failed graph exposed partial references");
    std::cout << "Native references: bindings, declarations, direct/pipeline calls, function "
                 "values, nested captures, shadowing, fields and failure isolation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
