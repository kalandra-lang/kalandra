#include "Hover.h"
#include "SemanticQuery.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static ParseResult syntax(const std::string &text) {
  return parse(std::make_shared<SourceFile>("types.k", text));
}
static void definition(const std::string &text, std::size_t at, std::size_t target) {
  auto result = lsp::definition(syntax(text), "types.k", at);
  require(result && result->span.begin == target,
          "wrong type/field definition at " + std::to_string(at));
}
static void hover(const std::string &text, std::size_t at, const std::string &expected) {
  auto result = lsp::hoverInfo(syntax(text), "types.k", at);
  require(result && result->text == expected, "wrong type hover at " + std::to_string(at) +
                                                  " expected " + expected + ": " +
                                                  (result ? result->text : "null"));
}
int main() {
  try {
    const std::string code =
        "type Point:\n  x:int\ntype Other:\n  x:int\ntype Box:\n  point:Point\nfn "
        "move(point:Point)->Point:\n  point with {x:point.x+1}\nfn main()->Point:\n  "
        "start:Point=Point {x:2}\n  move(start)\n";
    auto checked = check(syntax(code));
    if (!checked.ok())
      for (const auto &d : checked.diagnostics)
        std::cerr << renderDiagnostic(*checked.source, d);
    require(checked.ok(), "type fixture failed");
    const auto declared = code.find("Point");
    auto target = lsp::definition(syntax(code), "types.k", declared);
    require(target.has_value(), "missing type declaration");
    std::size_t count = 0;
    for (auto at = code.find("Point"); at != code.npos; at = code.find("Point", at + 1)) {
      definition(code, at, declared);
      hover(code, at, "type Point");
      ++count;
    }
    auto refs = lsp::references(syntax(code), *target, true);
    require(refs && refs->size() == count && count == 7, "type occurrences lost");
    const auto field = code.find("x:int"), label = code.find("x:point"), read = code.find("x+1"),
               ctor = code.find("x:2");
    for (auto at : {label, read, ctor}) {
      definition(code, at, field);
      hover(code, at, "x: int64");
    }
    target = lsp::definition(syntax(code), "types.k", field);
    refs = lsp::references(syntax(code), *target, false);
    require(refs && refs->size() == 3, "field construction/update/read identities differ");
    target = lsp::definition(syntax(code), "types.k", code.find("x:int", field + 1));
    refs = lsp::references(syntax(code), *target, false);
    require(refs && refs->empty(), "same-spelled field stole occurrences");
    hover(code, code.find(":int") + 1, "int: int64");
    require(!lsp::definition(syntax(code), "types.k", code.find(":int") + 1),
            "invented builtin declaration");
    const std::string nested = "type Point:\n  x:int\nfn apply(callback:fn(Point)->Point, "
                               "points:[Point])->Point:\n  callback(points[0])\nfn main():\n  0\n";
    target = lsp::definition(syntax(nested), "types.k", nested.find("Point"));
    require(target.has_value(), "missing nested fixture type");
    refs = lsp::references(syntax(nested), *target, false);
    require(refs && refs->size() == 4, "nested callable/array/result annotations lost");
    const std::string tensor =
        "fn scale(values:Tensor[float32,?])->Tensor[float32,?]:\n  values*2\nfn main():\n  0\n";
    hover(tensor, tensor.find("float32"), "float32: float32");
    require(lsp::hoverInfo(syntax(tensor), "types.k", tensor.find("Tensor")).has_value(),
            "missing tensor annotation hover");
    const std::string errors = "error A:\n  Bad\ntype Both=A\nfn work()->int ! Both:\n  fail "
                               "A.Bad\nfn main()->int ! A:\n  try work()\n";
    target = lsp::definition(syntax(errors), "types.k", errors.find("Both:"));
    // The alias spelling is preserved rather than navigating to its canonical member.
    definition(errors, errors.find("Both:\n"), errors.find("Both="));
    definition(errors, errors.find("=A") + 1, errors.find("A:"));
    target = lsp::definition(syntax(errors), "types.k", errors.find("Both="));
    require(target.has_value(), "alias declaration unmapped");
    refs = lsp::references(syntax(errors), *target, true);
    require(refs && refs->size() == 2, "error alias channel identity lost");
    const std::string invalid = "type Point:\n  x:int\nfn main():\n  Point {missing:1}\n";
    require(!lsp::definition(syntax(invalid), "types.k", invalid.find("missing")),
            "invalid constructor leaked metadata");
    std::cout << "Native type tokens: annotations, nested callable/array types, constructors, "
                 "updates, field identity, builtins and error aliases passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
