#include "Hover.h"
#include "SemanticQuery.h"
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
static ParseResult syntax(const std::string &text) {
  return parse(std::make_shared<SourceFile>("enum-tokens.k", text));
}
static lsp::SourceLocation definition(const std::string &text, std::size_t at,
                                      std::size_t expected) {
  auto result = lsp::definition(syntax(text), "enum-tokens.k", at);
  if (!result) {
    auto checked = check(syntax(text));
    for (const auto &d : checked.diagnostics)
      std::cerr << renderDiagnostic(*checked.source, d);
  }
  require(result && result->span.begin == expected,
          "wrong enum definition at " + std::to_string(at) + " expected " +
              std::to_string(expected) + " got " +
              (result ? std::to_string(result->span.begin) : "null"));
  return *result;
}
static void references(const std::string &text, std::size_t at, std::size_t count,
                       bool include = false) {
  auto target = lsp::definition(syntax(text), "enum-tokens.k", at);
  require(target.has_value(), "missing enum reference target");
  auto result = lsp::references(syntax(text), *target, include);
  require(result && result->size() == count,
          "enum reference count at " + std::to_string(at) + ": " +
              (result ? std::to_string(result->size()) : "null"));
}
static void hover(const std::string &text, std::size_t at, const std::string &expected) {
  auto result = lsp::hoverInfo(syntax(text), "enum-tokens.k", at);
  require(result && result->text == expected,
          "enum hover: " + (result ? result->text : "null") + " != " + expected);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const std::string code =
        "enum Choice:\n  Empty\n  Some:\n    value:int\n  Other:\n    value:int\nenum Separate:\n  "
        "Some:\n    value:int\nfn inspect(choice:Choice,value:int)->int:\n  match choice:\n    "
        "Choice.Some {value:item}:\n      read=fn():item\n      read()\n    Choice.Other "
        "{value}:value\n    Choice.Empty:value\nfn main():\n  extra=Choice.Other {value:1}\n  "
        "inspect(Choice.Some {value:42},0)\n";
    auto checked = check(syntax(code));
    if (!checked.ok())
      for (const auto &d : checked.diagnostics)
        std::cerr << renderDiagnostic(*checked.source, d);
    require(checked.ok(), "enum fixture invalid");
    const auto some = code.find("Some:"), other = code.find("Other:"), empty = code.find("Empty");
    const auto pattern = code.find("Choice.Some"), ctor = code.rfind("Choice.Some");
    definition(code, pattern + 7, some);
    definition(code, ctor + 7, some);
    hover(code, some, "Some: Choice");
    hover(code, pattern + 7, "Some: Choice");
    references(code, some, 2);
    references(code, some, 3, true);
    references(code, code.find("Some:", some + 1), 0);
    definition(code, code.find("Choice.Empty") + 7, empty);
    references(code, empty, 1);
    const auto field = code.find("value:int"), otherField = code.find("value:int", field + 1);
    const auto label = code.find("value:item"), binding = code.find("item}"),
               capture = code.find("item\n");
    definition(code, label, field);
    definition(code, code.rfind("value:42"), field);
    definition(code, binding, binding);
    definition(code, capture, binding);
    references(code, field, 2);
    references(code, binding, 1);
    references(code, binding, 2, true);
    const auto shorthand = code.find("{value}") + 1, use = code.find("}:value") + 2;
    definition(code, shorthand, shorthand);
    definition(code, use, shorthand);
    hover(code, shorthand, "value: int64");
    references(code, shorthand, 1);
    references(code, shorthand, 2, true);
    references(code, otherField, 2); // Includes the shorthand field role and constructor label.
    const auto outer = code.find("value:int", code.find("fn inspect"));
    definition(code, code.find("Empty:value") + 6, outer);
    references(code, outer, 1);
    references(code, other, 2);
    // A union copies layouts, but pattern cases/fields still identify original declarations.
    const std::string errors =
        "error A:\n  Bad:\n    text:String\nerror B:\n  Bad:\n    text:String\ntype Both=A | B\nfn "
        "inspect(error:Both)->int:\n  match error:\n    A.Bad {text:message}:length(message)\n    "
        "B.Bad {text}:length(text)\nfn main():\n  issue=A.Bad {text:\"bad\"}\n  0\n";
    definition(errors, errors.find("A.Bad") + 2, errors.find("Bad:"));
    definition(errors, errors.find("B.Bad") + 2, errors.find("Bad:", errors.find("Bad:") + 1));
    definition(errors, errors.find("text:message"), errors.find("text:String"));
    references(errors, errors.find("Bad:"), 2);
    references(errors, errors.find("text:String"), 2);
    const auto shortError = errors.find("{text}") + 1;
    definition(errors, errors.rfind("length(text)") + 7, shortError);
    references(errors, errors.find("text:String", errors.find("text:String") + 1), 1);
    const std::string builtins =
        "fn explain(error:TaskError)->int:\n  match error:\n    TaskError.Fault {kind,message}:\n  "
        "    match kind:\n        FaultKind.Bounds:length(message)\n        _:0\n    _:0\nfn "
        "main():\n  0\n";
    auto builtin = check(syntax(builtins));
    require(builtin.ok(), "builtin pattern fixture invalid");
    require(!lsp::definition(syntax(builtins), "enum-tokens.k", builtins.find("Fault {")),
            "invented builtin case declaration");
    hover(builtins, builtins.find("Fault {"), "Fault: TaskError");
    hover(builtins, builtins.find("Bounds:"), "Bounds: FaultKind");
    const auto message = builtins.find("message}");
    definition(builtins, builtins.find("message)"), message);
    references(builtins, message, 1);
    const std::string invalid = "enum E:\n  A\nfn main():\n  match E.A:\n    E.Missing:0\n";
    require(!lsp::definition(syntax(invalid), "enum-tokens.k", invalid.find("Missing")),
            "failed match leaked navigation");
    require(!lsp::definition(syntax(code), "enum-tokens.k", code.find("enum")),
            "keyword became definition");
    const auto root = fs::absolute(argv[1]);
    fs::create_directories(root);
    const auto write = [&](const char *file, const std::string &text) {
      std::ofstream out(root / file);
      out << text;
      out.close();
      require(bool(out), "fixture write");
    };
    const std::string library = "export enum Choice:\n  Empty\n  Some:\n    value:int\n";
    write("library.k", library);
    write("facade.k", "export from \"./library.k\" import Choice as PublicChoice\n");
    const std::string caller =
        "from \"./facade.k\" import PublicChoice as LocalChoice\nfn main()->int:\n  match "
        "LocalChoice.Some {value:7}:\n    LocalChoice.Some {value:bound}:bound\n    _:0\n";
    write("main.k", caller);
    const auto load = [&] {
      auto parsed = loadModules((root / "main.k").string());
      require(parsed.has_value(), "load enum modules");
      return std::move(*parsed);
    };
    auto target = lsp::definition(load(), (root / "main.k").string(), caller.find("Some"));
    require(target && target->source->path() == (root / "library.k").string() &&
                target->span.begin == library.find("Some"),
            "imported variant target");
    auto refs = lsp::references(load(), *target, true);
    require(refs && refs->size() == 3, "imported variant references");
    target = lsp::definition(load(), (root / "main.k").string(), caller.find("value:bound"));
    require(target && target->source->path() == (root / "library.k").string() &&
                target->span.begin == library.find("value:int"),
            "imported payload target");
    refs = lsp::references(load(), *target, false);
    require(refs && refs->size() == 2, "imported payload references");
    std::cout << "Native enum tokens: constructors, cases, payloads, explicit/shorthand patterns, "
                 "captured bindings, unions, builtins, imports and failure isolation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
