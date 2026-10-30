#include "kalandra/Semantics.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace kalandra;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
ParseResult syntax(std::string text, SyntaxLimits limits = {}) {
  return parse(std::make_shared<SourceFile>("function-types.k", std::move(text)), limits);
}
bool has(const ParseResult &result, std::string_view code) {
  return std::ranges::any_of(result.diagnostics,
                             [&](const auto &diagnostic) { return diagnostic.code == code; });
}
void spans(const TypeSyntax &type, const SourceFile &source) {
  const auto contains = [](SourceSpan parent, SourceSpan child) {
    return parent.begin <= child.begin && child.begin <= child.end && child.end <= parent.end;
  };
  require(type.span.end <= source.text().size() && contains(type.span, type.name),
          "type span lost its source");
  if (type.function) {
    require(!type.tensor && !type.element && type.dimensions.empty(),
            "function syntax acquired a collection layout");
    for (const auto &parameter : type.function->parameters) {
      require(contains(type.span, parameter.span), "parameter outside function type");
      spans(parameter, source);
    }
    require(contains(type.span, type.function->result.span), "result outside function type");
    spans(type.function->result, source);
    for (auto error : type.function->errorTypes)
      require(contains(type.span, error), "error family outside function type");
  }
}
void signatures() {
  auto parsed = syntax(
      "# π before the signatures\n"
      "fn apply(value:int, transform:fn(int)->int)->int:\n  0\n"
      "fn factory(offset:int)->fn(int)->int:\n  0\n"
      "fn failingFactory()->(fn(int)->int) ! BuildError:\n  0\n"
      "fn failingInvocation()->fn(int)->int ! ValidationError:\n  0\n"
      "fn nested()->fn()->fn(int)->int ! InnerError:\n  0\n"
      "fn grouped()->fn()->(fn(int)->int) ! OuterError:\n  0\n"
      "fn both()->(fn(int)->int ! CallError) ! FactoryError:\n  0\n"
      "fn combined()->fn()->int ! (A | B | TaskError):\n  0\n"
      "fn unionFactory()->(fn()->int ! (A | B)) ! (C | TaskError):\n  0\n"
      "fn complex(callback:fn(fn(int)->int, [String], Tensor[float32,?], Bool, Record,)->String,"
      " other:int)->int:\n  0\n"
      "fn groupedNumeric(value:(((int))))->(int):\n  value\n"
      "fn expressions()->int:\n"
      "  alias:fn(int)->int=double\n"
      "  creator=fn(value:int)->fn(int)->int:fn(other:int):value+other\n"
      "  consumer=fn(callback:fn(int)->int):callback(1)\n  0\n");
  require(parsed.ok(), "valid function type rejected");
  const auto &functions = parsed.module.functions;
  require(functions.size() == 12, "declarations consumed by recursive type parsing");
  const auto &apply = functions[0];
  require(apply.parameters.size() == 2 && apply.parameters[1].type.function &&
              apply.parameters[1].type.function->parameters.size() == 1 && !apply.taskError,
          "function parameter lost its signature");
  require(parsed.module.source->slice(apply.parameters[1].type.span) == "fn(int)->int",
          "callable parameter span changed");
  require(functions[1].result->function && !functions[1].taskError,
          "returned callable parsed as an outer result");
  require(functions[2].taskError && functions[2].result->function->errorTypes.empty() &&
              parsed.module.source->slice(*functions[2].errorType) == "BuildError",
          "factory error attached to returned function");
  require(!functions[3].taskError && functions[3].result->function->errorTypes.size() == 1 &&
              parsed.module.source->slice(functions[3].result->function->errorTypes[0]) ==
                  "ValidationError",
          "invocation error attached to factory");
  const auto &nested = *functions[4].result->function;
  require(nested.parameters.empty() && nested.errorTypes.empty() && nested.result.function &&
              nested.result.function->errorTypes.size() == 1,
          "function arrows are not right associative");
  const auto &grouped = *functions[5].result->function;
  require(grouped.parameters.empty() && grouped.errorTypes.size() == 1 && grouped.result.function &&
              grouped.result.function->errorTypes.empty(),
          "grouping failed to close the inner callable's error channel");
  require(functions[6].taskError && functions[6].result->function->errorTypes.size() == 1,
          "factory and invocation channels did not remain separate");
  require(functions[7].result->function->errorTypes.size() == 3 && !functions[7].taskError,
          "invocation union consumed incorrectly");
  require(functions[8].result->function->errorTypes.size() == 2 &&
              functions[8].errorTypes.size() == 2,
          "grouped invocation and factory unions consumed incorrectly");
  const auto &complex = *functions[9].parameters[0].type.function;
  require(complex.parameters.size() == 5 && complex.parameters[0].function &&
              complex.parameters[1].array && complex.parameters[2].tensor &&
              functions[9].parameters.size() == 2,
          "nested parameters or trailing comma consumed the enclosing parameter");
  require(parsed.module.source->slice(functions[10].parameters[0].type.span) == "(((int)))",
          "grouped type lost outer span");
  const auto &expressions = functions[11].body;
  require(expressions[0].annotation->function != nullptr, "binding annotation lost callable");
  const auto &creator =
      std::get<LambdaExpr>(parsed.module.expressions[expressions[1].expression].value);
  const auto &consumer =
      std::get<LambdaExpr>(parsed.module.expressions[expressions[2].expression].value);
  require(creator.result->function && consumer.parameters[0].annotation->function,
          "anonymous signature lost function types");
  for (const auto &function : functions) {
    for (const auto &parameter : function.parameters)
      spans(parameter.type, *parsed.module.source);
    if (function.result)
      spans(*function.result, *parsed.module.source);
  }
  auto saved = *functions[9].parameters[0].type.function;
  parsed = {};
  require(saved.parameters[0].function->parameters.size() == 1,
          "copied recursive syntax borrowed the destroyed parser tree");
}
void limitsAndRecovery() {
  std::string many = "fn many(callback:fn(";
  for (unsigned i = 0; i < 40; ++i) {
    if (i)
      many += ',';
    many += i % 2 ? "String" : "int";
  }
  many += ")->int)->int:\n  0\n";
  auto wide = syntax(many);
  require(wide.ok() &&
              wide.module.functions[0].parameters[0].type.function->parameters.size() == 40,
          "arbitrary small parameter cap introduced");
  SyntaxLimits limits;
  limits.maxDepth = 8;
  std::string deep = "fn deep()->";
  for (unsigned i = 0; i < 12; ++i)
    deep += "fn()->";
  deep += "int:\n  0\n";
  require(has(syntax(deep, limits), "P036"), "undelimited function nesting is not bounded");
  limits = {};
  limits.maxNodes = 4;
  require(has(syntax("fn wide(callback:fn(int,int,int)->int):\n  0\n", limits), "P036"),
          "type node budget is not enforced");
  limits = {};
  limits.maxDepth = 2;
  require(syntax("fn f()->fn()->int:\n  0\n", limits).ok(), "type depth boundary rejected");
  limits.maxDepth = 1;
  require(has(syntax("fn f()->fn()->int:\n  0\n", limits), "P036"), "type depth boundary accepted");
  for (const auto *text :
       {"fn f()->fn()->int ! A ! B:\n  0\n", "fn f(callback:fn()->fn()->int ! A ! B):\n  0\n"})
    require(has(syntax(text), "P006"), "ambiguous double error suffix accepted");
  for (const auto *type :
       {"fn", "fn(int)", "fn(int) int", "fn()->", "fn(,int)->int", "fn(int,,int)->int",
        "fn(value:int)->int", "fn()->()", "fn()->int !", "fn()->int ! A | B", "fn()->(int ! A)",
        "fn()->int ! (A |)", "fn()->int ! A ! B", "(fn()->int,int)"}) {
    auto malformed =
        syntax(std::string("fn bad(callback:") + type + "):\n  0\nfn good()->int:\n  7\n");
    require(!malformed.ok(), "malformed callable type accepted");
    require(std::ranges::any_of(malformed.module.functions,
                                [&](const auto &function) {
                                  return malformed.module.source->slice(function.name) == "good";
                                }),
            "type diagnostic prevented declaration recovery");
    for (const auto &diagnostic : malformed.diagnostics)
      require(diagnostic.span.begin <= diagnostic.span.end &&
                  diagnostic.span.end <= malformed.module.source->text().size(),
              "invalid diagnostic span");
  }
  // Every truncated prefix must terminate with either a valid tree or bounded diagnostics.
  const std::string seed = "fn use(callback:fn(fn(int)->int,)->(fn()->int) ! (A | B)):\n  0\n";
  limits = {};
  limits.maxDiagnostics = 1;
  for (std::size_t i = 0; i <= seed.size(); ++i) {
    auto prefix = syntax(seed.substr(0, i), limits);
    require(prefix.diagnostics.size() <= 1, "type recovery exceeded diagnostic budget");
  }
}
void semanticBoundary() {
  for (const auto *text :
       {"fn factory()->fn(int)->int:\n  0\n", "fn main()->int:\n  value:fn(int)->int=0\n  0\n",
        "type Holder:\n  callback:fn(int)->int\nfn main()->int:\n  holder=Holder {callback:0}\n  "
        "0\n",
        "enum Choice:\n  Some:\n    callback:fn()->int\nfn main()->int:\n  value=Choice.Some "
        "{callback:0}\n  0\n",
        "fn main()->[int]:\n  map([1],fn(value)->fn(int)->int:0)\n"}) {
    auto parsed = syntax(text);
    require(parsed.ok(), "semantic boundary input failed to parse");
    auto checked = check(std::move(parsed));
    require(!checked.ok(), "invalid or unsupported callable use accepted");
  }
  auto ordinary = check(syntax("fn identity(value:(((int))))->(int):\n  value\n"
                               "fn main()->int:\n  identity(7)\n"));
  require(ordinary.ok(), "grouped ordinary types changed semantic identity");
}
void moduleOrigins() {
  auto root = std::make_shared<const SourceFile>("root.k", "fn main()->int:\n  0\n");
  auto helper = std::make_shared<const SourceFile>(
      "helper.k", "fn make()->(fn(int)->int ! CallError) ! BuildError:\n  0\n");
  auto combined = std::make_shared<const SourceFile>(
      "root.k", root->text() + helper->text(),
      std::vector<SourceFile::Part>{{0, root}, {root->text().size(), helper}});
  auto parsed = parse(combined);
  require(parsed.ok(), "combined module signatures rejected");
  const auto &factory = parsed.module.functions[1];
  const auto callError = factory.result->function->errorTypes[0];
  const auto [origin, offset] = parsed.module.source->origin(callError.begin);
  require(origin == helper.get() && offset == helper->text().find("CallError"),
          "nested callable channel lost its module origin");
  require(parsed.module.source->slice(*factory.errorType) == "BuildError",
          "enclosing module channel changed");
  auto checked = check(std::move(parsed));
  require(!checked.ok(), "module function type escaped semantic rejection");
  require(std::ranges::any_of(
              checked.diagnostics,
              [&](const auto &diagnostic) {
                return diagnostic.code == "S072" &&
                       renderDiagnostic(*checked.source, diagnostic).find("helper.k:1:") !=
                           std::string::npos;
              }),
          "function type diagnostic lost original module location");
}
} // namespace
int main() {
  try {
    signatures();
    limitsAndRecovery();
    semanticBoundary();
    moduleOrigins();
    std::cout << "Function type syntax, errors, bounds and semantic boundary passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
