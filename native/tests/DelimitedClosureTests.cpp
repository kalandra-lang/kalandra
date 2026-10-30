#include "kalandra/Semantics.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
auto source(std::string text) {
  return std::make_shared<SourceFile>("delimited-closures.k", std::move(text));
}
bool has(const std::vector<Diagnostic> &diagnostics, std::string_view code) {
  return std::ranges::any_of(diagnostics, [&](const auto &d) { return d.code == code; });
}
void spans(const LexResult &tokens, std::size_t size) {
  require(!tokens.tokens.empty() && tokens.tokens.back().kind == TokenKind::End,
          "lexer did not terminate");
  for (const auto &token : tokens.tokens)
    require(token.span.begin <= token.span.end && token.span.end <= size,
            "invalid layout token span");
  for (const auto &diagnostic : tokens.diagnostics)
    require(diagnostic.span.begin <= diagnostic.span.end && diagnostic.span.end <= size,
            "invalid lexical diagnostic span");
}
} // namespace
int main() {
  try {
    unsigned programs = 0;
    std::string hero, nested;
    for (const auto &entry : std::filesystem::directory_iterator(KALANDRA_DELIMITED_FIXTURES)) {
      if (entry.path().extension() != ".k")
        continue;
      std::ifstream file(entry.path());
      const std::string text{std::istreambuf_iterator<char>(file), {}};
      auto input = source(text);
      auto tokens = lex(input);
      spans(tokens, text.size());
      require(tokens.diagnostics.empty(), "valid delimited suite failed lexing");
      unsigned indentation = 0;
      for (const auto &token : tokens.tokens) {
        if (token.kind == TokenKind::Indent)
          ++indentation;
        if (token.kind == TokenKind::Dedent) {
          require(indentation != 0, "layout dedented past the outer scope");
          --indentation;
        }
      }
      require(indentation == 0, "layout failed to restore enclosing scope");
      auto syntax = parse(input);
      auto result = check(syntax);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid delimited suite failed checking");
      ++programs;
      if (entry.path().stem() == "hero") {
        hero = text;
        const auto &body = syntax.module.functions[0].body;
        const auto &call =
            std::get<CallExpr>(syntax.module.expressions[body.back().expression].value);
        const auto &expression = syntax.module.expressions[call.arguments[1]];
        const auto &lambda = std::get<LambdaExpr>(expression.value);
        require(lambda.body.size() == 2 && lambda.body[0].binding && !lambda.body[1].binding,
                "callback lost ordered statements");
        require(input->slice(*lambda.body[0].binding) == "shifted", "binding span changed");
        require(input->slice(expression.span).starts_with("fn(value):") &&
                    expression.span.end <
                        syntax.module.expressions[body.back().expression].span.end,
                "anonymous body span consumed the closing call");
        require(std::holds_alternative<ConditionalExpr>(
                    syntax.module.expressions[lambda.body[1].expression].value),
                "nested conditional escaped its callback");
        require(std::ranges::any_of(result.module->functions,
                                    [](const auto &fn) { return fn.captureCount == 1; }),
                "inline callback lost its lexical capture");
      }
      if (entry.path().stem() == "nested")
        nested = text;
      auto eof = text;
      if (eof.ends_with('\n'))
        eof.pop_back();
      require(check(parse(source(eof))).ok(), "EOF changed suite termination");
      std::string crlf;
      for (const auto c : text) {
        if (c == '\n')
          crlf += '\r';
        crlf += c;
      }
      require(check(parse(source(crlf))).ok(), "CRLF changed suite termination");
    }
    require(programs == 24 && !hero.empty() && !nested.empty(), "missing fixture catalog");
    for (const auto &[text, code] : std::vector<std::pair<std::string, std::string>>{
             {"fn main():\n  map([1],fn(v):\n  )\n", "P002"},
             {"fn main():\n  map([1],fn(v):\n    result=v+1\n  )\n", "P007"},
             {"fn main():\n  map([1],fn(v):\n    if v>0:\n      v\n  )\n", "P035"},
             {"fn main():\n  map([1],fn(v):\n    v)\n", "L016"},
             {"fn main():\n  map([1],fn(v):\n    v\n    )\n", "L016"},
             {"fn main():\n  map([1],fn(v):\n    x=v+1\n   x\n  )\n", "L008"},
             {"fn main():\n  map([1],fn(v):\n\tv\n  )\n", "L003"},
             {"fn main():\n  map([1],fn(v):\n    v\n  ]\n", "L011"},
             {"fn main():\n  map([1],fn(v):\n    v\n", "L005"}}) {
      const auto result = parse(source(text));
      require(!result.ok() && has(result.diagnostics, code), "suite diagnostic changed");
    }
    for (const auto &text :
         {"fn main()->int:\n  result=map([1],fn(v):\n    hidden=v+1\n    hidden\n  )\n  hidden\n",
          "fn main()->int:\n  result=map([1],fn(v):\n    v=v+1\n    v\n  )\n  result[0]\n"})
      require(!check(parse(source(text))).ok(), "callback scope or immutability escaped");
    SyntaxLimits limits;
    limits.maxTokens = 16;
    auto tokens = lex(source(hero), limits);
    require(has(tokens.diagnostics, "L006"), "layout ignored token budget");
    spans(tokens, hero.size());
    limits = {};
    limits.maxSourceBytes = hero.size() - 1;
    require(has(lex(source(hero), limits).diagnostics, "L001"), "layout ignored source budget");
    limits = {};
    limits.maxNodes = 4;
    require(has(parse(source(hero), limits).diagnostics, "P008"), "layout ignored node budget");
    limits = {};
    limits.maxDepth = 2;
    auto deep = lex(source(nested), limits);
    require(has(deep.diagnostics, "L010") || has(deep.diagnostics, "L007"),
            "nested suites ignored depth budget");
    limits = {};
    limits.maxDiagnostics = 2;
    unsigned prefixes = 0;
    for (const auto &text :
         {hero, nested, std::string("fn main():\n  (fn(v):\n    \"é🙂\"\n  )\n")})
      for (std::size_t length = 0; length <= text.size(); ++length) {
        const auto prefix = text.substr(0, length);
        const auto tokens = lex(source(prefix), limits);
        spans(tokens, length);
        require(tokens.diagnostics.size() <= limits.maxDiagnostics,
                "lexer exceeded diagnostic budget");
        const auto parsed = parse(source(prefix), limits);
        require(parsed.diagnostics.size() <= limits.maxDiagnostics,
                "parser exceeded diagnostic budget");
        for (const auto &d : parsed.diagnostics)
          require(d.span.begin <= d.span.end && d.span.end <= length,
                  "truncated suite diagnostic span escaped source");
        ++prefixes;
      }
    std::cout << "Delimited closures: " << programs
              << " source programs in LF/CRLF/EOF modes; 9 syntax and 2 semantic rejections; "
              << prefixes << " truncation prefixes; spans, scopes and budgets passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
