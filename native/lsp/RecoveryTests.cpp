#include "Completion.h"
#include "Signature.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool value, const std::string &what) {
  if (!value)
    throw std::runtime_error(what);
}
const std::string path = (std::filesystem::temp_directory_path() / "editing-recovery.k").string();
const std::string add = "fn add(left:i64,right:i64)->i64:\n  return left+right\n";
std::pair<std::string, std::size_t> marked(std::string text) {
  const auto at = text.rfind('|');
  require(at != text.npos, "cursor required");
  text.erase(at, 1);
  return {text, at};
}
void completion(std::string source, bool expected = true, std::string label = "value") {
  auto [text, at] = marked(std::move(source));
  auto result = lsp::completeNames(path, text, at);
  const bool found =
      std::ranges::any_of(result.names, [&](const auto &name) { return name.name == label; });
  require(found == expected, "completion: " + text + " expected " + label);
  require(!check(parse(std::make_shared<const SourceFile>(path, text))).ok(),
          "incomplete source compiled");
}
void signature(std::string source, bool expected = true) {
  auto [text, at] = marked(std::move(source));
  auto result =
      lsp::signatureAt(path, text, at, {},
                       {{(std::filesystem::temp_directory_path() / "editing-helper.k").string(),
                         "export fn imported(left:i64,right:i64)->i64:\n  return left\n"}});
  require(bool(result) == expected, "signature: " + text);
  if (result)
    require(result->parameters.size() == 2 && result->activeParameter == 1,
            "wrong active signature");
}
} // namespace
int main() {
  try {
    const std::string prefix = add + "fn consume(input:[i64])->i64:\n  return length(input)\n" +
                               "fn main():\n  value=2\n  values=[1,2]\n";
    completion(prefix + "  values[va|\n");
    completion(prefix + "  [va|,\n");
    completion(prefix + "  if true and va|\n");
    completion(prefix + "  if true:\n    va|\n");
    completion(prefix + "  while value<3:\n    value=va|\n");
    completion(prefix + "  consume([va|\n");
    completion(prefix + "  value + va|]\n");
    completion(prefix + "  not va|\n");
    completion("enum Choice:\n  Yes\n  No\nfn inspect(choice:Choice):\n  value=2\n  match "
               "choice:\n    Choice.Yes: va|\n");
    completion(prefix + "  map(values,fn(item:int): va|\n");
    completion(prefix + "  map(values,\nfn (item:i64): va|\n");
    completion(prefix + "  if false: 0\n  else: va|\n");
    completion(prefix + "  tru|\n", true, "true");
    completion(prefix + "  if|\n", true, "if");
    completion("type Item:\n  value:i64\nfn main():\n  Item {va|\n", true);

    completion("fn broken():\n  add(1,\n" + prefix + "  va|\n");
    completion(prefix + "  va|\nfn broken():\n  value=\n");
    completion(prefix + "  missing+va|\n", false);
    completion(prefix + "  \"va|\n", false);
    signature(prefix + "  add(1,2+va|Suffix)\n");
    signature(prefix + "  add(1,2+va|Suffix\n");
    signature(prefix + "  add(1,values[va|\n");
    signature(prefix + "  add(1,2+|\n");
    signature(prefix + "  while value<3:\n    add(1,va|\n");
    signature("fn apply(left:i64,action:fn(i64)->i64)->i64:\n  return action(left)\n" + prefix +
              "  apply(1,fn(item:int): va|\n");
    signature("fn choose(left:i64,right:bool)->i64:\n  return left\n" + prefix +
              "  choose(1,true and va|\n");
    signature(
        "enum Choice:\n  Yes\n  No\n" + add +
        "fn inspect(choice:Choice):\n  value=2\n  match choice:\n    Choice.Yes: add(1,va|\n");
    signature("fn broken(left:i64,right:i64)->i64:\n  return missing\n" + prefix + "  broken(1,va|\n",
              false);

    signature(prefix + "  add(1,if true: 0\n  else: va|\n");

    signature(prefix + "  add(1,if true: va|\n");
    signature("from \"./editing-helper.k\" import imported\n" + prefix +
              "  imported(1,if true: va|\n");
    signature(prefix + "  add(1,missing+va|\n", false);
    signature(prefix + "  missing(1,va|\n", false);
    signature("fn broken():\n  value=\n" + prefix + "  add(1,va|\n");
    // Every recovery path must obey ordinary resource limits and stop making progress.
    const auto invalid = std::make_shared<const SourceFile>(path, prefix + "  add(1,values[va\n");
    for (int budget = 0; budget < 5; ++budget) {
      SyntaxLimits bounded;
      bounded.editorRecovery = true;
      if (budget == 0)
        bounded.maxSourceBytes = 8;
      if (budget == 1)
        bounded.maxTokens = 8;
      if (budget == 2)
        bounded.maxNodes = 1;
      if (budget == 3)
        bounded.maxDepth = 1;
      if (budget == 4)
        bounded.maxDiagnostics = 1;
      const auto parsed = parse(invalid, bounded);
      require(!parsed.module.editorOnly && !parsed.ok(), "recovery bypassed resource limit");
    }
    const std::vector<std::string> fragments = {"(",     ")", "[",  "]",    "{",  "}",
                                                ",",     ":", "if", "else", "fn", "match",
                                                "value", "=", "+",  "\n",   "  "};
    unsigned seed = 0x5245434f;
    for (std::size_t sample = 0; sample < 512; ++sample) {
      std::string source = "fn main():\n  ";
      for (int token = 0; token < 16; ++token) {
        seed = seed * 1664525u + 1013904223u;
        source += fragments[seed % fragments.size()] + " ";
      }
      source += "\n";
      SyntaxLimits bounded;
      bounded.editorRecovery = true;
      bounded.maxDiagnostics = 8;
      bounded.maxDepth = 16;
      auto parsed = parse(std::make_shared<const SourceFile>(path, source), bounded);
      require(parsed.diagnostics.size() <= bounded.maxDiagnostics,
              "unbounded recovery diagnostics");
      require(!check(std::move(parsed)).ok(), "editor parse compiled malformed corpus");
    }
    SyntaxLimits limits;
    limits.editorRecovery = true;
    auto syntax = parse(std::make_shared<const SourceFile>(path, prefix + "  add(1,2)\n"), limits);
    require(syntax.module.editorOnly, "missing editor-only marker");
    require(!check(syntax).ok(), "editor syntax escaped ordinary checker");
    ScopeCompletion query{{prefix.size(), prefix.size()}, "", {}, false, false};
    CheckOptions options;
    options.completion = &query;
    require(!check(std::move(syntax), {}, options).module, "editor query published typed IR");
    std::cout << "Editor recovery: structural holes, control/collections, declaration islands, "
                 "checked prefixes and no executable IR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
