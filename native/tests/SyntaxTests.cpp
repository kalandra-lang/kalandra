#include "kalandra/Syntax.h"
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace kalandra;
namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::shared_ptr<const SourceFile> source(std::string text) {
  return std::make_shared<SourceFile>("test.k", std::move(text));
}
ParseResult syntax(std::string text, SyntaxLimits limits = {}) {
  return parse(source(std::move(text)), limits);
}
bool has(const std::vector<Diagnostic> &diagnostics, std::string_view code) {
  return std::ranges::any_of(diagnostics, [code](const auto &d) { return d.code == code; });
}
void validTree(const ParseResult &result) {
  const auto size = result.module.source->text().size();
  const auto span = [size](SourceSpan value) {
    require(value.begin <= value.end && value.end <= size, "invalid source span");
  };
  const auto id = [&](ExprId value) {
    require(value < result.module.expressions.size(), "invalid expression ID");
  };
  for (const auto &expression : result.module.expressions) {
    span(expression.span);
    std::visit(
        [&](const auto &value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, NameExpr>)
            span(value.name);
          else if constexpr (std::is_same_v<T, NumberExpr>)
            span(value.literal);
          else if constexpr (std::is_same_v<T, UnaryExpr>)
            id(value.operand);
          else if constexpr (std::is_same_v<T, BinaryExpr>) {
            id(value.left);
            id(value.right);
          } else if constexpr (std::is_same_v<T, CallExpr>) {
            id(value.callee);
            for (auto argument : value.arguments)
              id(argument);
          } else if constexpr (std::is_same_v<T, TaskExpr>) {
            id(value.operand);
            if (value.placement)
              id(*value.placement);
          } else if constexpr (std::is_same_v<T, ParallelExpr>) {
            for (const auto &statement : value.body) {
              span(statement.span);
              id(statement.expression);
            }
          } else if constexpr (std::is_same_v<T, CatchExpr>) {
            id(value.operand);
            span(value.parameter);
            for (const auto &statement : value.body) {
              span(statement.span);
              id(statement.expression);
            }
          } else if constexpr (std::is_same_v<T, MatchExpr>) {
            id(value.subject);
            for (const auto &arm : value.arms) {
              span(arm.span);
              if (arm.family)
                span(*arm.family);
              if (arm.variant)
                span(*arm.variant);
              for (const auto &field : arm.fields) {
                span(field.name);
                span(field.binding);
              }
              for (const auto &statement : arm.body) {
                span(statement.span);
                id(statement.expression);
              }
            }
          } else if constexpr (std::is_same_v<T, FailExpr>)
            id(value.operand);
          else if constexpr (std::is_same_v<T, LambdaExpr>) {
            for (const auto &statement : value.body) {
              span(statement.span);
              id(statement.expression);
              if (statement.binding)
                span(*statement.binding);
            }
            for (const auto &parameter : value.parameters)
              span(parameter.name);
            for (auto error : value.errorTypes)
              span(error);
          } else if constexpr (std::is_same_v<T, IndexExpr>) {
            id(value.input);
            for (auto index : value.indices)
              id(index);
          } else if constexpr (std::is_same_v<T, ArrayExpr>) {
            for (auto element : value.elements)
              id(element);
          }
        },
        expression.value);
  }
  for (const auto &function : result.module.functions) {
    span(function.span);
    span(function.name);
    for (const auto &statement : function.body) {
      span(statement.span);
      id(statement.expression);
    }
  }
  for (const auto &diagnostic : result.diagnostics)
    span(diagnostic.span);
}
void lexerLines() {
  auto input = source("# π comment\r\nfn f(\r\n    a: float32, # continuation\r\n):\r\n  # blank "
                      "body comment\r\n  a\r\n\r\nfn g():\r\n    2");
  auto result = lex(input);
  require(result.diagnostics.empty(), "valid CRLF/comment input rejected");
  const auto count = [&](TokenKind kind) {
    return std::ranges::count_if(result.tokens, [kind](const auto &t) { return t.kind == kind; });
  };
  require(count(TokenKind::Indent) == 2 && count(TokenKind::Dedent) == 2,
          "wrong indentation tokens");
  require(count(TokenKind::Newline) == 4, "continuation created a statement boundary");
  require(result.tokens.back().kind == TokenKind::End, "missing terminal token");
  require(input->location(input->text().find("a: float32")) ==
              std::pair<std::size_t, std::size_t>{3, 5},
          "wrong byte location");
  require(syntax(input->text()).ok(), "valid continuation did not parse");
}
void operatorSpans() {
  const std::vector<std::pair<std::string, TokenKind>> operators{
      {"->", TokenKind::Arrow}, {"=>", TokenKind::FatArrow},
      {"==", TokenKind::Comparison}, {"!=", TokenKind::Comparison},
      {"<=", TokenKind::Comparison}, {">=", TokenKind::Comparison},
      {"<", TokenKind::Comparison}, {">", TokenKind::Comparison},
      {"-", TokenKind::Minus}, {"=", TokenKind::Equal}, {"!", TokenKind::Bang}};
  for (const auto &[spelling, kind] : operators) {
    auto input = source(spelling);
    auto result = lex(input);
    require(result.diagnostics.empty() && result.tokens[0].kind == kind,
            "operator token kind lost: " + spelling);
    require(input->slice(result.tokens[0].span) == spelling,
            "operator span lost a consumed character: " + spelling);
  }
  const std::vector<std::pair<std::string, ComparisonKind>> comparisons{
      {"==", ComparisonKind::Equal}, {"!=", ComparisonKind::NotEqual},
      {"<", ComparisonKind::Less}, {"<=", ComparisonKind::LessEqual},
      {">", ComparisonKind::Greater}, {">=", ComparisonKind::GreaterEqual}};
  for (const auto &[spelling, kind] : comparisons)
    for (const auto *space : {"", " "})
      for (const auto *newline : {"\n", "\r\n"}) {
        auto result = syntax(std::string("fn f(a:int,b:int)->Bool:") + newline +
                             "  a" + space + spelling + space + "b" + newline);
        require(result.ok(), "comparison syntax rejected: " + spelling);
        const auto id = result.module.functions[0].body[0].expression;
        require(std::get<ComparisonExpr>(result.module.expressions[id].value).operation == kind,
                "comparison changed meaning: " + spelling);
      }
}
void lexerFailures() {
  const std::vector<std::pair<std::string, std::string>> examples{
      {"fn f():\n\tx\n", "L003"},
      {"fn f():\n    1\n  2\n", "L008"},
      {"fn f():\n  f([1)\n", "L011"},
      {"fn f():\n  (1\n", "L005"},
      {"fn f():\n  1e+\n", "L013"},
      {"fn f():\n  1.\n", "L012"},
      {"fn f():\n  0xff\n", "L014"},
      {"fn f():\n  \"oops\n", "L015"},
      {std::string("\xc0\xaf"), "L002"},
      {std::string("\xed\xa0\x80"), "L002"},
      {std::string("\xf4\x90\x80\x80"), "L002"},
      {std::string("\xe2\x82"), "L002"},
      {std::string("fn f():\n  ") + '\0', "L004"},
  };
  for (const auto &[text, code] : examples) {
    auto result = syntax(text);
    require(!result.ok() && has(result.diagnostics, code), "missing lexical diagnostic: " + code);
    validTree(result);
  }
  auto stringTokens = lex(source("\"escaped \\\"quote\\\" and π\"\n"));
  require(stringTokens.diagnostics.empty() && stringTokens.tokens[0].kind == TokenKind::String,
          "escaped string tokenization failed");
}
void precedenceAndSpans() {
  auto result =
      syntax("fn f(value: float32) -> float32:\n  value + 2 * -3 -> scale(4) -> finish\n");
  require(result.ok(), "precedence example rejected");
  const auto &nodes = result.module.expressions;
  const auto &body = result.module.functions[0].body;
  const auto &last = std::get<BinaryExpr>(nodes[body[0].expression].value);
  require(last.operation == TokenKind::Arrow &&
              std::holds_alternative<NameExpr>(nodes[last.right].value),
          "pipeline associativity wrong");
  const auto &first = std::get<BinaryExpr>(nodes[last.left].value);
  require(first.operation == TokenKind::Arrow &&
              std::holds_alternative<CallExpr>(nodes[first.right].value),
          "pipeline/call precedence wrong");
  const auto &add = std::get<BinaryExpr>(nodes[first.left].value);
  const auto &multiply = std::get<BinaryExpr>(nodes[add.right].value);
  require(add.operation == TokenKind::Plus && multiply.operation == TokenKind::Star,
          "arithmetic precedence wrong");
  require(std::get<UnaryExpr>(nodes[multiply.right].value).operation == TokenKind::Minus,
          "unary precedence wrong");
  require(result.module.source->slice(nodes[body[0].expression].span) ==
              "value + 2 * -3 -> scale(4) -> finish",
          "expression span wrong");
  validTree(result);
  auto nested = syntax("fn f():\n  -(factory()(1 + 2)) * 3\n");
  require(nested.ok(), "group/call chain rejected");
  validTree(nested);
  auto associativity = syntax("fn f():\n  8 - 3 - 1\n");
  auto &root = std::get<BinaryExpr>(
      associativity.module.expressions[associativity.module.functions[0].body[0].expression].value);
  require(std::holds_alternative<BinaryExpr>(associativity.module.expressions[root.left].value),
          "subtraction is not left associative");
}
void typesAndBindings() {
  auto result =
      syntax("fn f(a: Tensor[float32, ?, 3], b: Tensor[float32]) -> Tensor[float32, ?, 2]:\n  x: "
             "Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6],])\n  matmul(a, x)\n");
  require(result.ok(), "tensor/binding syntax rejected");
  const auto &function = result.module.functions[0];
  const auto &parameter = function.parameters[0].type;
  require(parameter.tensor && parameter.dimensions.size() == 2 && !parameter.dimensions[0].extent &&
              parameter.dimensions[1].extent == 3,
          "shape syntax lost");
  require(parameter.element && result.module.source->slice(*parameter.element) == "float32",
          "tensor element type lost");
  require(function.parameters[1].type.dimensions.empty(), "rank zero tensor rejected");
  require(function.body[0].binding && function.body[0].annotation, "binding/annotation missing");
  require(!function.body[1].binding, "return expression became a binding");
  auto noFinalNewline = syntax("fn f():\n  1");
  require(noFinalNewline.ok(), "EOF did not close the body");
  // Parsing must not impersonate semantic analysis.
  require(syntax("fn f():\n  x = [[1], [2, 3]]\n  x = missing\n  x\n").ok(),
          "parser performed undeclared type/scope analysis");
  validTree(result);
}
void arraysAfterBlocks() {
  for (const auto *text : {"fn f():\n  result=parallel:\n    1\n  [result]\n",
                           "fn f():\n  result=if true:\n    1\n  else:\n    2\n  [result]\n",
                           "fn f():\n  result=work() catch fn(error):\n    1\n  [result]\n"}) {
    auto result = syntax(text);
    require(result.ok(), "array after block parsed as a postfix");
    const auto &body = result.module.functions[0].body;
    require(
        body.size() == 2 && body[0].binding && !body[1].binding &&
            std::holds_alternative<ArrayExpr>(result.module.expressions[body[1].expression].value),
        "block and following array did not form separate statements");
    validTree(result);
  }
}
void anonymousFunctions() {
  auto result = syntax("fn typed(values:[int]):\n"
                       "  map(values,fn(value:int)->float: float(value))\n"
                       "fn nested(values:[int]):\n"
                       "  map(values,fn(x): map(values,fn(y): y+x))\n");
  require(result.ok(), "anonymous callback syntax rejected");
  validTree(result);
  std::size_t count = 0;
  for (const auto &expression : result.module.expressions)
    if (const auto *lambda = std::get_if<LambdaExpr>(&expression.value)) {
      require(lambda->parameters.size() == 1, "anonymous parameter list lost");
      if (!count)
        require(lambda->parameters[0].annotation && lambda->result,
                "anonymous type annotations lost");
      ++count;
    }
  require(count == 3, "nested anonymous function nodes lost");
  require(syntax("fn f():\n  fn(x): x\n").ok(),
          "parser performed callback-context semantic checking");
}
void parserFailuresAndRecovery() {
  require(syntax("fn f():\n  \"text\"\n").ok(), "ordinary String literal did not parse");
  const std::vector<std::pair<std::string, std::string>> examples{
      {"fn f(x):\n  x\n", "P002"},
      {"fn f():\n  x = 1\n", "P007"},
      {"fn f():\n  foo(1 2)\n", "P002"},
      {"fn f():\n  foo(x = 1)\n", "P002"},
      {"fn f():\n  1 => x\n", "P010"},
      {"fn f():\n  catch task\n", "P011"},
      {"fn f():\n  map([1],fn x: x)\n", "P002"},
      {"fn f():\n  map([1],fn(x) x)\n", "P002"},
      {"fn f():\n  map([1],fn(1): 1)\n", "P002"},
      {"type Point:\n  x:\n", "P002"},
      {"fn f() -> float32 ! Error | TaskError:\n  1\n", "P006"},
      {"fn f(a: Tensor[float32, 1.5]):\n  1\n", "P004"},
      {"fn f(a: Tensor[float32, 18446744073709551616]):\n  1\n", "P004"},
      {"", "P001"},
  };
  for (const auto &[text, code] : examples) {
    auto result = syntax(text);
    require(!result.ok() && has(result.diagnostics, code), "missing parser diagnostic: " + code);
    validTree(result);
  }
  auto recovered = syntax("fn first():\n  a =\n  b =\n  1\nfn broken(x):\n  x\nfn last():\n  2\n");
  require(recovered.diagnostics.size() >= 3 && recovered.module.functions.size() == 2,
          "did not recover across statements/functions");
  require(recovered.module.source->slice(recovered.module.functions.back().name) == "last",
          "recovery swallowed last function");
  const auto &diagnostic = recovered.diagnostics[0];
  require(recovered.module.source->location(diagnostic.span.begin) ==
              std::pair<std::size_t, std::size_t>{2, 6},
          "error location wrong");
  const auto printed = renderDiagnostic(*recovered.module.source, diagnostic);
  require(printed.find("test.k:2:6: error[P012]") != std::string::npos &&
              printed.find('^') != std::string::npos,
          "missing rendered location/caret");
  validTree(recovered);
}
void limitsAndOwnership() {
  auto limits = SyntaxLimits{};
  limits.maxSourceBytes = 4;
  require(has(syntax("fn f():\n  1\n", limits).diagnostics, "L001"), "source limit missing");
  limits = {};
  limits.maxTokens = 5;
  require(has(syntax("fn f():\n  1\n", limits).diagnostics, "L006"), "token limit missing");
  limits = {};
  limits.maxNodes = 2;
  require(has(syntax("fn f():\n  1 + 2\n", limits).diagnostics, "P008"), "node limit missing");
  limits = {};
  limits.maxDepth = 8;
  require(has(syntax("fn f():\n  " + std::string(20, '(') + "1" + std::string(20, ')'), limits)
                  .diagnostics,
              "L010"),
          "delimiter limit missing");
  require(has(syntax("fn f():\n  " + std::string(20, '-') + "1", limits).diagnostics, "P009"),
          "expression limit missing");
  std::string indentation = "fn f():\n";
  for (int i = 1; i < 20; ++i)
    indentation += std::string(static_cast<std::size_t>(i), ' ') + "1\n";
  require(has(syntax(indentation, limits).diagnostics, "L007"), "indentation limit missing");
  require(
      has(syntax("fn f(a: Tensor[float32,1,1,1,1,1,1,1,1,1]):\n  1", limits).diagnostics, "P003"),
      "tensor rank limit missing");
  limits = {};
  limits.maxDiagnostics = 2;
  require(syntax("fn f():\n  a =\n  b =\n  c =\n  1\n", limits).diagnostics.size() == 2,
          "diagnostic budget ignored");
  limits.maxDiagnostics = 0;
  require(!syntax("@", limits).ok(), "zero diagnostic budget accepted invalid source");
  std::weak_ptr<const SourceFile> weak;
  auto held = [&] {
    auto owned = source("fn f():\n  1\n");
    weak = owned;
    return parse(owned);
  }();
  require(!weak.expired() && held.module.source->slice(held.module.functions[0].name) == "f",
          "AST lost source ownership");
  std::string chain = "fn f():\n  1";
  for (int i = 0; i < 20000; ++i)
    chain += " + 1";
  auto longChain = syntax(std::move(chain));
  require(longChain.ok() && longChain.module.expressions.size() == 40001, "long flat chain failed");
  validTree(longChain);
}
void placementSyntax() {
  auto result =
      syntax("fn main():\n  try parallel:\n    try await spawn project(a, b) on devices.gpu(1)\n");
  require(result.ok(), "placement syntax failed");
  validTree(result);
  auto &m = result.module;
  auto outer = std::get<TaskExpr>(m.expressions[m.functions[0].body[0].expression].value);
  auto block = std::get<ParallelExpr>(m.expressions[outer.operand].value);
  auto attempt = std::get<TaskExpr>(m.expressions[block.body[0].expression].value);
  auto wait = std::get<TaskExpr>(m.expressions[attempt.operand].value);
  auto spawn = std::get<TaskExpr>(m.expressions[wait.operand].value);
  require(outer.prefix == TaskPrefix::Try && attempt.prefix == TaskPrefix::Try &&
              wait.prefix == TaskPrefix::Await && spawn.prefix == TaskPrefix::Spawn &&
              spawn.placement,
          "on must attach to spawn inside try await");
  auto target = std::get<CallExpr>(m.expressions[*spawn.placement].value);
  auto name = std::get<NameExpr>(m.expressions[target.callee].value);
  require(m.source->slice(name.name) == "devices.gpu" && target.arguments.size() == 1,
          "device selector lost qualification or index");
  require(!syntax("fn main():\n  spawn f() on\n").ok(), "missing placement target accepted");
}
void malformedCorpus() {
  constexpr std::string_view alphabet = "fn():[]{} +-*/?,=\n\t#\".123ab!>@";
  std::uint32_t state = 0x4b414c41;
  for (int i = 0; i < 3000; ++i) {
    std::string text;
    const auto length = (state >> 16) % 200;
    for (std::size_t n = 0; n < length; ++n) {
      state = state * 1664525u + 1013904223u;
      text += alphabet[state % alphabet.size()];
    }
    state = state * 1664525u + 1013904223u;
    // Half the corpus is lexically balanced, exercising parser recovery rather
    // than stopping every random sample at the lexer boundary.
    if (i % 2 == 0) {
      constexpr std::string_view fragments[] = {"1",  "x",   "+",     "-",   "*", "/", "%",
                                                "->", "f()", "[1,2]", "(x)", "=", ",", "?"};
      text = "fn generated():\n  ";
      for (std::size_t n = 0; n < length / 4 + 1; ++n) {
        state = state * 1664525u + 1013904223u;
        text += fragments[state % std::size(fragments)];
        text += ' ';
      }
      text += "\nfn after():\n  0\n";
    }
    auto result = syntax(std::move(text));
    validTree(result);
    require(result.diagnostics.size() <= 32, "unbounded diagnostics");
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"lexer lines", lexerLines},
        {"lexer failures", lexerFailures},
        {"operator spans and meaning", operatorSpans},
        {"precedence and spans", precedenceAndSpans},
        {"types and bindings", typesAndBindings},
        {"anonymous functions", anonymousFunctions},
        {"arrays after blocks", arraysAfterBlocks},
        {"recovery", parserFailuresAndRecovery},
        {"limits and source ownership", limitsAndOwnership},
        {"placement grouping", placementSyntax},
        {"malformed corpus", malformedCorpus}};
    for (const auto &[name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
    require(argc == 2, "expected the canonical fixture path");
    std::ifstream input(argv[1], std::ios::binary);
    require(static_cast<bool>(input), "cannot read canonical fixture");
    std::string text((std::istreambuf_iterator<char>(input)), {});
    auto result = syntax(std::move(text));
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << renderDiagnostic(*result.module.source, diagnostic);
    require(result.ok() && result.module.functions.size() == 4,
            "canonical projection fixture failed");
    require(result.module.source->slice(result.module.functions[0].name) == "project" &&
                result.module.source->slice(result.module.functions[3].name) == "main",
            "canonical declarations lost");
    validTree(result);
    std::cout << "PASS canonical projection fixture\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
