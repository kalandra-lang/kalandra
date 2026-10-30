#include "kalandra/Syntax.h"
#include <algorithm>
#include <charconv>

namespace kalandra {
namespace {
int precedence(TokenKind kind) {
  switch (kind) {
  case TokenKind::Comparison:
    return 7;
  case TokenKind::Arrow:
    return 10;
  case TokenKind::Plus:
  case TokenKind::Minus:
    return 20;
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
    return 30;
  default:
    return -1;
  }
}
class Parser {
public:
  Parser(LexResult input, SyntaxLimits limits)
      : source_(std::move(input.source)), tokens_(std::move(input.tokens)), limits_(limits) {
    result_.module.source = source_;
    result_.module.editorOnly = limits_.editorRecovery && input.diagnostics.size() <= 8 &&
                                std::ranges::all_of(input.diagnostics, [](const auto &d) {
                                  return d.code == "L005" || d.code == "L011";
                                });
    result_.diagnostics = std::move(input.diagnostics);
  }
  ParseResult run() {
    if (!result_.diagnostics.empty() && !result_.module.editorOnly)
      return std::move(result_);
    while (!at(TokenKind::End) && result_.diagnostics.size() < limits_.maxDiagnostics) {
      if (consume(TokenKind::Newline))
        continue;
      if (word("from") || (word("export") && source_->slice(peek().span) == "from")) {
        auto imported = parseImport();
        if (imported)
          result_.module.imports.push_back(std::move(*imported));
        else
          synchronizeFunction();
        continue;
      }
      if (word("type") || word("enum") || word("error") ||
          (word("export") &&
           (source_->slice(peek().span) == "type" || source_->slice(peek().span) == "enum" ||
            source_->slice(peek().span) == "error"))) {
        auto record = parseRecord();
        if (record)
          result_.module.records.push_back(std::move(*record));
        else
          synchronizeFunction();
        continue;
      }
      auto function = parseFunction();
      if (function)
        result_.module.functions.push_back(std::move(*function));
      else
        synchronizeFunction();
    }
    if (result_.module.functions.empty() && result_.module.imports.empty() &&
        result_.module.records.empty() && result_.diagnostics.empty())
      error("P001", "expected at least one named function", current().span);
    return std::move(result_);
  }

private:
  const Token &current() const { return tokens_[position_]; }
  const Token &peek(std::size_t offset = 1) const {
    return tokens_[std::min(position_ + offset, tokens_.size() - 1)];
  }
  bool at(TokenKind kind) const { return current().kind == kind; }
  bool word(std::string_view name) const {
    return at(TokenKind::Identifier) && source_->slice(current().span) == name;
  }
  Token advance() {
    const auto token = current();
    if (!at(TokenKind::End))
      ++position_;
    return token;
  }
  bool consume(TokenKind kind) {
    if (!at(kind))
      return false;
    advance();
    return true;
  }
  void error(std::string code, std::string message, SourceSpan span) {
    if (result_.diagnostics.size() < limits_.maxDiagnostics)
      result_.diagnostics.push_back({std::move(code), std::move(message), span});
  }
  std::optional<Token> expect(TokenKind kind, std::string message) {
    if (at(kind))
      return advance();
    if (at(TokenKind::Equal))
      message = "bindings are statements, not expressions";
    error("P002", std::move(message), current().span);
    const bool boundary = at(TokenKind::End) || at(TokenKind::Dedent) ||
                          at(TokenKind::RightParen) || at(TokenKind::RightBracket) ||
                          at(TokenKind::RightBrace);
    const bool layout =
        (kind == TokenKind::Newline || kind == TokenKind::Indent || kind == TokenKind::Dedent) &&
        (boundary || (kind == TokenKind::Newline && (at(TokenKind::Comma) || word("else"))));
    if (result_.module.editorOnly &&
        (layout || kind == TokenKind::RightParen || kind == TokenKind::RightBracket ||
         kind == TokenKind::RightBrace || kind == TokenKind::Colon))
      return Token{kind, {current().span.begin, current().span.begin}};
    return {};
  }
  std::optional<TypeSyntax> parseType() {
    if (depth_ >= limits_.maxDepth) {
      error("P036", "type nesting limit exceeded", current().span);
      return {};
    }
    const Depth guard(depth_);
    if (typeNodes_ >= limits_.maxNodes) {
      error("P036", "type node limit exceeded", current().span);
      return {};
    }
    ++typeNodes_;
    if (at(TokenKind::LeftParen)) {
      const auto start = advance().span;
      auto type = parseType();
      if (!type)
        return {};
      auto end = expect(TokenKind::RightParen, "expected ')' after grouped type");
      if (!end)
        return {};
      type->span = {start.begin, end->span.end};
      return type;
    }
    if (word("fn")) {
      const auto start = advance().span;
      if (!expect(TokenKind::LeftParen, "expected '(' after function type 'fn'"))
        return {};
      auto signature = std::make_shared<FunctionTypeSyntax>();
      if (!at(TokenKind::RightParen)) {
        do {
          auto parameter = parseType();
          if (!parameter)
            return {};
          signature->parameters.push_back(std::move(*parameter));
        } while (consume(TokenKind::Comma) && !at(TokenKind::RightParen));
      }
      if (!expect(TokenKind::RightParen, "expected ')' after function type parameters") ||
          !expect(TokenKind::Arrow, "expected '->' and a result type after function parameters"))
        return {};
      auto result = parseType();
      if (!result)
        return {};
      signature->result = std::move(*result);
      auto end = signature->result.span.end;
      if (consume(TokenKind::Bang)) {
        if (signature->result.function &&
            signature->result.span.begin == signature->result.name.begin) {
          error("P006", "parenthesize a returned function type before its factory error channel",
                signature->result.span);
          return {};
        }
        auto channels = errorMembers(true);
        if (!channels)
          return {};
        signature->errorTypes = std::move(*channels);
        end = tokens_[position_ - 1].span.end;
      }
      TypeSyntax type;
      type.name = start;
      type.span = {start.begin, end};
      type.function = std::move(signature);
      return type;
    }
    if (at(TokenKind::LeftBracket)) {
      const auto start = advance().span;
      auto element = parseType();
      if (!element)
        return {};
      if (element->array || element->tensor) {
        error("P006", "array elements require a scalar, record or function type", element->span);
        return {};
      }
      auto end = expect(TokenKind::RightBracket, "expected ']' after array element type");
      if (!end)
        return {};
      element->span = {start.begin, end->span.end};
      element->array = true;
      return element;
    }
    auto name = expect(TokenKind::Identifier, "expected a type name");
    if (!name)
      return {};
    TypeSyntax type;
    type.name = name->span;
    type.span = name->span;
    if (source_->slice(name->span) != "Tensor")
      return type;
    type.tensor = true;
    if (!expect(TokenKind::LeftBracket, "expected '[' after Tensor"))
      return {};
    auto element = expect(TokenKind::Identifier, "expected a tensor element type");
    if (!element)
      return {};
    type.element = element->span;
    while (consume(TokenKind::Comma)) {
      if (type.dimensions.size() >= limits_.maxDepth) {
        error("P003", "tensor rank exceeds the configured syntax limit", current().span);
        return {};
      }
      if (at(TokenKind::Question)) {
        type.dimensions.push_back({{}, advance().span});
      } else {
        auto dimension = expect(TokenKind::Number, "expected a nonnegative integer extent or '?'");
        if (!dimension)
          return {};
        const auto text = source_->slice(dimension->span);
        std::uint64_t value = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
          error("P004", "tensor extent must be a representable nonnegative integer",
                dimension->span);
          return {};
        }
        type.dimensions.push_back({value, dimension->span});
      }
    }
    auto end = expect(TokenKind::RightBracket, "expected ']' after tensor type");
    if (!end)
      return {};
    type.span.end = end->span.end;
    return type;
  }
  std::optional<Import> parseImport() {
    const bool exported = word("export");
    if (exported)
      advance();
    advance();
    auto path = expect(TokenKind::String, "expected a quoted relative module path after 'from'");
    if (!path)
      return {};
    if (!word("import")) {
      error("P002", "expected 'import' after module path", current().span);
      return {};
    }
    advance();
    Import result{path->span, {}};
    result.exported = exported;
    while (true) {
      auto name = expect(TokenKind::Identifier, "expected an imported declaration name");
      if (!name)
        return {};
      ImportedName imported{name->span, {}};
      if (word("as")) {
        advance();
        auto alias = expect(TokenKind::Identifier, "expected an import alias");
        if (!alias)
          return {};
        imported.alias = alias->span;
      }
      result.names.push_back(imported);
      if (!consume(TokenKind::Comma))
        break;
    }
    if (!expect(TokenKind::Newline, "expected end of import declaration"))
      return {};
    return result;
  }
  std::optional<std::vector<SourceSpan>> errorMembers(bool channel) {
    const bool grouped = consume(TokenKind::LeftParen);
    std::vector<SourceSpan> members;
    do {
      auto member = expect(TokenKind::Identifier, "expected an error family");
      if (!member)
        return {};
      members.push_back(member->span);
      if (members.size() > limits_.maxTokens) {
        error("P006", "error union exceeds configured token budget", member->span);
        return {};
      }
    } while (consume(TokenKind::Pipe));
    if (grouped && !expect(TokenKind::RightParen, "expected ')' after error union"))
      return {};
    if (channel && members.size() > 1 && !grouped) {
      error("P006", "parenthesize an error union after '!'", members.front());
      return {};
    }
    return members;
  }
  std::optional<RecordDeclaration> parseRecord() {
    const bool exported = word("export");
    if (exported)
      advance();
    const bool applicationError = word("error");
    const bool enumeration = word("enum") || applicationError;
    advance();
    auto name = expect(TokenKind::Identifier, "expected a record or enum type name");
    if (!name)
      return {};
    if (!enumeration && consume(TokenKind::Equal)) {
      auto members = errorMembers(false);
      if (!members || !expect(TokenKind::Newline, "expected end of error alias"))
        return {};
      RecordDeclaration alias{name->span, {}, exported};
      alias.errorAlias = std::move(*members);
      return alias;
    }
    if (!expect(TokenKind::Colon, "expected ':' after record name") ||
        !expect(TokenKind::Newline, "record fields must start on the next line") ||
        !expect(TokenKind::Indent, "expected indented record fields"))
      return {};
    RecordDeclaration result{name->span, {}, exported};
    result.error = applicationError;
    if (enumeration) {
      while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
        auto variant = expect(TokenKind::Identifier, "expected an enum variant name");
        if (!variant)
          return {};
        VariantDeclaration declaration{variant->span, {}};
        if (consume(TokenKind::Colon)) {
          if (!expect(TokenKind::Newline, "variant fields must start on a new line") ||
              !expect(TokenKind::Indent, "expected indented variant fields"))
            return {};
          while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
            auto field = expect(TokenKind::Identifier, "expected a variant field name");
            if (!field || !expect(TokenKind::Colon, "variant fields require ': Type'"))
              return {};
            auto type = parseType();
            if (!type || !expect(TokenKind::Newline, "expected end of variant field"))
              return {};
            declaration.fields.push_back({field->span, std::move(*type)});
          }
          if (!expect(TokenKind::Dedent, "expected end of variant fields"))
            return {};
        } else if (!expect(TokenKind::Newline, "expected end of enum variant"))
          return {};
        result.variants.push_back(std::move(declaration));
      }
      if (!expect(TokenKind::Dedent, "expected end of enum declaration"))
        return {};
      return result;
    }
    while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
      auto field = expect(TokenKind::Identifier, "expected a field name");
      if (!field || !expect(TokenKind::Colon, "record fields require ': Type'"))
        return {};
      auto type = parseType();
      if (!type || !expect(TokenKind::Newline, "expected end of record field"))
        return {};
      result.fields.push_back({field->span, std::move(*type)});
    }
    if (!expect(TokenKind::Dedent, "expected end of record declaration"))
      return {};
    return result;
  }
  std::optional<std::vector<FieldInitializer>> recordFields() {
    std::vector<FieldInitializer> fields;
    if (!at(TokenKind::RightBrace)) {
      while (true) {
        auto name = expect(TokenKind::Identifier, "expected a named record field");
        if (!name || !expect(TokenKind::Colon, "record fields require 'name: value'"))
          return {};
        auto value = parseExpression();
        if (!value)
          return {};
        fields.push_back({name->span, *value});
        if (!consume(TokenKind::Comma) || at(TokenKind::RightBrace))
          break;
      }
    }
    if (!expect(TokenKind::RightBrace, "expected '}' after record fields"))
      return {};
    return fields;
  }
  std::optional<Function> parseFunction() {
    const bool exported = word("export");
    if (exported)
      advance();
    if (!word("fn")) {
      error(
          "P005",
          "this frontend expects a named 'fn' declaration; other declarations are not implemented",
          current().span);
      return {};
    }
    const auto start = advance().span.begin;
    auto name = expect(TokenKind::Identifier, "expected a function name after 'fn'");
    if (!name)
      return {};
    Function function;
    function.exported = exported;
    function.name = name->span;
    if (!expect(TokenKind::LeftParen, "expected '(' after function name"))
      return {};
    if (!at(TokenKind::RightParen)) {
      while (true) {
        auto parameter = expect(TokenKind::Identifier, "expected a parameter name");
        if (!parameter || !expect(TokenKind::Colon, "named parameters require ': Type'"))
          return {};
        auto type = parseType();
        if (!type)
          return {};
        function.parameters.push_back({parameter->span, std::move(*type)});
        if (!consume(TokenKind::Comma) || at(TokenKind::RightParen))
          break;
      }
    }
    if (!expect(TokenKind::RightParen, "expected ')' after parameters"))
      return {};
    if (consume(TokenKind::Arrow)) {
      auto result = parseType();
      if (!result)
        return {};
      function.result = std::move(*result);
    }
    if (at(TokenKind::Bang)) {
      advance();
      if (function.result && function.result->function &&
          function.result->span.begin == function.result->name.begin) {
        error("P006", "parenthesize a returned function type before its factory error channel",
              function.result->span);
        return {};
      }
      auto channels = errorMembers(true);
      if (!channels)
        return {};
      function.errorType = channels->front();
      function.errorTypes = std::move(*channels);
      function.taskError = true;
    }
    if (!expect(TokenKind::Colon, "expected ':' before function body") ||
        !expect(TokenKind::Newline, "function body must start on the next line") ||
        !expect(TokenKind::Indent, "expected an indented function body"))
      return {};
    while (!at(TokenKind::Dedent) && !at(TokenKind::End) &&
           result_.diagnostics.size() < limits_.maxDiagnostics) {
      const auto before = position_;
      auto statement = parseStatement();
      if (statement)
        function.body.push_back(std::move(*statement));
      else {
        if (result_.module.editorOnly) {
          const SourceSpan hole{current().span.begin, current().span.begin};
          if (auto id = add({hole, NameExpr{hole}}))
            function.body.push_back({hole, {}, {}, *id});
        }
        synchronizeStatement();
      }
      if (position_ == before)
        break;
    }
    auto end = expect(TokenKind::Dedent, "expected the end of the function body");
    if (!end)
      return {};
    function.span = {start,
                     function.body.empty() ? end->span.begin : function.body.back().span.end};
    if (function.body.empty() || function.body.back().binding) {
      error("P007", "function body must end with a result expression", end->span);
      if (result_.module.editorOnly) {
        const SourceSpan hole{end->span.begin, end->span.begin};
        if (auto id = add({hole, NameExpr{hole}}))
          function.body.push_back({hole, {}, {}, *id});
      }
    }
    return function;
  }
  std::optional<Statement> parseStatement() {
    Statement statement;
    statement.span.begin = current().span.begin;
    if (at(TokenKind::Identifier) && !word("parallel") &&
        (peek().kind == TokenKind::Equal || peek().kind == TokenKind::Colon)) {
      statement.binding = advance().span;
      if (consume(TokenKind::Colon)) {
        auto type = parseType();
        if (!type)
          return {};
        statement.annotation = std::move(*type);
      }
      if (!expect(TokenKind::Equal, "expected '=' before binding initializer"))
        return {};
    }
    auto expression = parseExpression();
    if (!expression)
      return {};
    statement.expression = *expression;
    statement.span.end = node(*expression).span.end;
    if (tokens_[position_ - 1].kind != TokenKind::Dedent &&
        !expect(TokenKind::Newline, "expected a newline after the expression"))
      return {};
    return statement;
  }
  struct Depth {
    std::size_t &value;
    explicit Depth(std::size_t &value) : value(value) { ++value; }
    ~Depth() { --value; }
  };
  Expr &node(ExprId id) { return result_.module.expressions[id]; }
  std::optional<ExprId> add(Expr expression) {
    if (result_.module.expressions.size() >= limits_.maxNodes) {
      error("P008", "expression node limit exceeded", expression.span);
      return {};
    }
    const auto id = result_.module.expressions.size();
    result_.module.expressions.push_back(std::move(expression));
    return id;
  }
  std::optional<ExprId> parseExpression(int minimum = 0) {
    if (depth_ >= limits_.maxDepth) {
      error("P009", "expression nesting limit exceeded", current().span);
      return {};
    }
    const Depth guard(depth_);
    auto left = prefix();
    if (!left)
      return {};
    bool compared = false;
    while (true) {
      // A block expression consumed its terminating newline/dedent. A call
      // beginning the next source line is not a postfix on that block.
      if (position_ && tokens_[position_ - 1].kind == TokenKind::Dedent)
        break;
      if (at(TokenKind::LeftParen)) {
        const auto begin = node(*left).span.begin;
        advance();
        auto arguments = sequence(TokenKind::RightParen);
        if (!arguments)
          return {};
        auto call =
            add({{begin, tokens_[position_ - 1].span.end}, CallExpr{*left, std::move(*arguments)}});
        if (!call)
          return {};
        left = call;
        continue;
      }
      if (consume(TokenKind::LeftBracket)) {
        const auto begin = node(*left).span.begin;
        auto indices = sequence(TokenKind::RightBracket);
        if (!indices)
          return {};
        left =
            add({{begin, tokens_[position_ - 1].span.end}, IndexExpr{*left, std::move(*indices)}});
        if (!left)
          return {};
        continue;
      }
      if (consume(TokenKind::Dot)) {
        const auto memberOffset = tokens_[position_ - 1].span.end;
        auto field = expect(TokenKind::Identifier, "expected a field name after '.'");
        if (!field && result_.module.editorOnly)
          field = Token{TokenKind::Identifier, {memberOffset, memberOffset}};
        if (!field)
          return {};
        left = add({{node(*left).span.begin, field->span.end}, FieldExpr{*left, field->span}});
        if (!left)
          return {};
        continue;
      }
      if (at(TokenKind::LeftBrace) || (word("with") && minimum <= 8)) {
        const auto begin = node(*left).span.begin;
        const bool update = word("with");
        SourceSpan name{};
        std::optional<SourceSpan> family;
        if (update) {
          advance();
          if (!expect(TokenKind::LeftBrace, "expected '{' after with"))
            return {};
        } else {
          const auto *identifier = std::get_if<NameExpr>(&node(*left).value);
          if (identifier)
            name = identifier->name;
          else if (const auto *qualified = std::get_if<FieldExpr>(&node(*left).value)) {
            const auto *owner = std::get_if<NameExpr>(&node(qualified->input).value);
            if (!owner) {
              error("P035", "variant construction requires Type.Variant", node(*left).span);
              return {};
            }
            family = owner->name;
            name = qualified->field;
          } else {
            error("P035", "record construction requires a type name", node(*left).span);
            return {};
          }
          advance();
        }
        auto fields = recordFields();
        if (!fields)
          return {};
        left = add({{begin, tokens_[position_ - 1].span.end},
                    RecordExpr{update ? left : std::nullopt, name, std::move(*fields), family}});
        if (!left)
          return {};
        continue;
      }
      if (word("catch") && minimum <= 4) {
        advance();
        if (!word("fn")) {
          error("P013", "catch requires an explicit fn(error) handler", current().span);
          return {};
        }
        advance();
        if (!expect(TokenKind::LeftParen, "expected '(' before handler parameter"))
          return {};
        auto parameter = expect(TokenKind::Identifier, "expected handler parameter name");
        if (!parameter || !expect(TokenKind::RightParen, "expected ')' after handler parameter") ||
            !expect(TokenKind::Colon, "expected ':' before handler body"))
          return {};
        auto body = suite();
        if (!body)
          return {};
        auto span = SourceSpan{node(*left).span.begin, body->back().span.end};
        left = add({span, CatchExpr{*left, parameter->span, std::move(*body)}});
        if (!left)
          return {};
        continue;
      }
      const bool conjunction = word("and"), disjunction = word("or");
      const auto power = conjunction ? 6 : disjunction ? 5 : precedence(current().kind);
      if (power < minimum)
        break;
      const auto operation = advance();
      if (operation.kind == TokenKind::Comparison && compared) {
        error("P034", "comparison chaining requires explicit grouping", operation.span);
        return {};
      }
      auto right = parseExpression(power + 1);
      if (!right)
        return {};
      const auto span = SourceSpan{node(*left).span.begin, node(*right).span.end};
      if (conjunction || disjunction) {
        left = add({span, LogicalExpr{conjunction, *left, *right}});
        compared = false;
      } else if (operation.kind == TokenKind::Comparison) {
        const auto spelling = source_->slice(operation.span);
        const auto kind = spelling == "=="   ? ComparisonKind::Equal
                          : spelling == "!=" ? ComparisonKind::NotEqual
                          : spelling == "<"  ? ComparisonKind::Less
                          : spelling == "<=" ? ComparisonKind::LessEqual
                          : spelling == ">"  ? ComparisonKind::Greater
                                             : ComparisonKind::GreaterEqual;
        left = add({span, ComparisonExpr{kind, *left, *right}});
        compared = true;
      } else
        left = add({span, BinaryExpr{operation.kind, *left, *right}});
      if (!left)
        return {};
    }
    if ((!position_ || tokens_[position_ - 1].kind != TokenKind::Dedent) &&
        (at(TokenKind::FatArrow) || at(TokenKind::Dot) || at(TokenKind::LeftBracket) ||
         at(TokenKind::LeftBrace))) {
      error("P010", "this expression form is not implemented in the first frontend",
            current().span);
      return {};
    }
    return left;
  }
  std::optional<std::vector<ExprId>> sequence(TokenKind closing) {
    std::vector<ExprId> values;
    if (!at(closing)) {
      while (true) {
        auto value = parseExpression();
        if (!value)
          return {};
        values.push_back(*value);
        if (!consume(TokenKind::Comma) || at(closing))
          break;
      }
    }
    if (!expect(closing, closing == TokenKind::RightParen ? "expected ')' after arguments"
                                                          : "expected ']' after array elements"))
      return {};
    return values;
  }
  std::optional<std::vector<Statement>> suite() {
    std::vector<Statement> body;
    if (!consume(TokenKind::Newline)) {
      auto value = parseExpression();
      if (!value)
        return {};
      body.push_back({node(*value).span, {}, {}, *value});
      return body;
    }
    if (result_.module.editorOnly &&
        (at(TokenKind::Dedent) || at(TokenKind::End) || at(TokenKind::RightParen) ||
         at(TokenKind::RightBracket) || at(TokenKind::RightBrace))) {
      error("P002", "expected an indented body", current().span);
      const SourceSpan hole{current().span.begin, current().span.begin};
      auto id = add({hole, NameExpr{hole}});
      if (!id)
        return {};
      body.push_back({hole, {}, {}, *id});
      return body; // Leave the enclosing suite's boundary untouched.
    }
    if (!expect(TokenKind::Indent, "expected an indented body"))
      return {};
    while (
        !at(TokenKind::Dedent) && !at(TokenKind::End) &&
        !(result_.module.editorOnly && (at(TokenKind::RightParen) || at(TokenKind::RightBracket) ||
                                        at(TokenKind::RightBrace)))) {
      const auto before = position_;
      auto statement = parseStatement();
      if (!statement)
        return {};
      body.push_back(std::move(*statement));
      if (position_ == before)
        break;
    }
    auto end = expect(TokenKind::Dedent, "expected end of body");
    if (!end)
      return {};
    if (body.empty() || body.back().binding) {
      error("P007", "body must end with a result expression", end->span);
      if (!result_.module.editorOnly)
        return {};
      const SourceSpan hole{end->span.begin, end->span.begin};
      auto id = add({hole, NameExpr{hole}});
      if (!id)
        return {};
      body.push_back({hole, {}, {}, *id});
    }
    return body;
  }
  std::optional<ExprId> prefix() {
    const auto token = current();
    if (consume(TokenKind::Number))
      return add({token.span, NumberExpr{token.span}});
    if (consume(TokenKind::String))
      return add({token.span, StringExpr{token.span}});
    if (at(TokenKind::Identifier)) {
      const auto text = source_->slice(token.span);
      if (text == "fn") {
        advance();
        if (!expect(TokenKind::LeftParen, "expected '(' after anonymous fn"))
          return {};
        LambdaExpr lambda;
        if (!at(TokenKind::RightParen)) {
          do {
            auto parameter = expect(TokenKind::Identifier, "expected an anonymous parameter name");
            if (!parameter)
              return {};
            std::optional<TypeSyntax> annotation;
            if (consume(TokenKind::Colon)) {
              annotation = parseType();
              if (!annotation)
                return {};
            }
            lambda.parameters.push_back({parameter->span, std::move(annotation)});
          } while (consume(TokenKind::Comma) && !at(TokenKind::RightParen));
        }
        if (!expect(TokenKind::RightParen, "expected ')' after anonymous parameters"))
          return {};
        if (consume(TokenKind::Arrow)) {
          lambda.result = parseType();
          if (!lambda.result)
            return {};
        }
        if (consume(TokenKind::Bang)) {
          if (lambda.result && lambda.result->function &&
              lambda.result->span.begin == lambda.result->name.begin) {
            error("P006", "parenthesize a returned function type before its factory error channel",
                  lambda.result->span);
            return {};
          }
          auto channels = errorMembers(true);
          if (!channels)
            return {};
          lambda.errorTypes = std::move(*channels);
        }
        if (!expect(TokenKind::Colon, "expected ':' before anonymous body"))
          return {};
        auto body = suite();
        if (!body)
          return {};
        const auto end = body->back().span.end;
        lambda.body = std::move(*body);
        return add({{token.span.begin, end}, std::move(lambda)});
      }
      if (text == "if") {
        advance();
        auto condition = parseExpression();
        if (!condition || !expect(TokenKind::Colon, "expected ':' after if condition"))
          return {};
        auto yes = suite();
        if (!yes)
          return {};
        const auto endOfTrue = position_;
        const bool inlineTrue = tokens_[position_ - 1].kind != TokenKind::Dedent;
        if (inlineTrue && !expect(TokenKind::Newline, "expected newline before else"))
          return {};
        if (!word("else")) {
          if (!std::holds_alternative<FailExpr>(node(yes->back().expression).value)) {
            error("P035", "if expressions require an else branch", current().span);
            if (result_.module.editorOnly) {
              const SourceSpan hole{current().span.begin, current().span.begin};
              auto id = add({hole, NameExpr{hole}});
              if (!id)
                return {};
              return add({{token.span.begin, hole.end},
                          ConditionalExpr{*condition, std::move(*yes), {{hole, {}, {}, *id}}}});
            }
            return {};
          }
          // An inline guard leaves its terminating newline to its enclosing statement.
          if (inlineTrue)
            position_ = endOfTrue;
          return add({{token.span.begin, yes->back().span.end},
                      ConditionalExpr{*condition, std::move(*yes), {}}});
        }
        advance();
        if (!expect(TokenKind::Colon, "expected ':' after else"))
          return {};
        auto no = suite();
        if (!no)
          return {};
        return add({{token.span.begin, no->back().span.end},
                    ConditionalExpr{*condition, std::move(*yes), std::move(*no)}});
      }
      if (text == "not") {
        advance();
        auto operand = parseExpression(7);
        if (!operand)
          return {};
        return add(
            {{token.span.begin, node(*operand).span.end}, UnaryExpr{TokenKind::Bang, *operand}});
      }
      if (text == "else" || text == "and" || text == "or") {
        error("P035", "unexpected control-flow keyword", token.span);
        return {};
      }
      if (text == "match") {
        advance();
        auto subject = parseExpression();
        if (!subject || !expect(TokenKind::Colon, "expected ':' after match subject") ||
            !expect(TokenKind::Newline, "match cases start on a new line") ||
            !expect(TokenKind::Indent, "expected indented match cases"))
          return {};
        std::vector<MatchArm> arms;
        while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
          MatchArm arm;
          arm.span.begin = current().span.begin;
          if (word("_"))
            advance();
          else {
            auto family = expect(TokenKind::Identifier, "expected variant family or '_'");
            if (!family || !expect(TokenKind::Dot, "expected qualified variant name"))
              return {};
            auto variant = expect(TokenKind::Identifier, "expected variant name");
            if (!variant)
              return {};
            arm.family = family->span;
            arm.variant = variant->span;
            if (consume(TokenKind::LeftBrace)) {
              if (!at(TokenKind::RightBrace)) {
                do {
                  auto field = expect(TokenKind::Identifier, "expected payload field name");
                  if (!field)
                    return {};
                  auto binding = field;
                  if (consume(TokenKind::Colon)) {
                    binding = expect(TokenKind::Identifier, "expected field binding name");
                    if (!binding)
                      return {};
                  }
                  arm.fields.push_back({field->span, binding->span});
                } while (consume(TokenKind::Comma) && !at(TokenKind::RightBrace));
              }
              if (!expect(TokenKind::RightBrace, "expected '}' after pattern fields"))
                return {};
            }
          }
          if (!expect(TokenKind::Colon, "expected ':' before match arm"))
            return {};
          auto body = suite();
          if (!body)
            return {};
          arm.span.end = body->back().span.end;
          arm.body = std::move(*body);
          arms.push_back(std::move(arm));
          if (tokens_[position_ - 1].kind != TokenKind::Dedent &&
              !expect(TokenKind::Newline, "expected newline after match arm"))
            return {};
        }
        if (!expect(TokenKind::Dedent, "expected end of match"))
          return {};
        if (arms.empty()) {
          error("P014", "match requires cases", token.span);
          return {};
        }
        auto span = SourceSpan{token.span.begin, arms.back().span.end};
        return add({span, MatchExpr{*subject, std::move(arms)}});
      }
      if (text == "fail") {
        advance();
        auto operand = parseExpression(40);
        if (!operand)
          return {};
        return add({{token.span.begin, node(*operand).span.end}, FailExpr{*operand}});
      }
      if (text == "parallel") {
        advance();
        if (!expect(TokenKind::Colon, "expected ':' after parallel") ||
            !expect(TokenKind::Newline, "parallel body starts on a new line") ||
            !expect(TokenKind::Indent, "expected an indented parallel body"))
          return {};
        std::vector<Statement> body;
        while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
          auto statement = parseStatement();
          if (!statement)
            return {};
          body.push_back(std::move(*statement));
        }
        auto end = expect(TokenKind::Dedent, "expected end of parallel body");
        if (!end)
          return {};
        if (body.empty() || body.back().binding) {
          error("P007", "parallel body must end with a result expression", end->span);
          return {};
        }
        auto span = SourceSpan{token.span.begin, body.back().span.end};
        return add({span, ParallelExpr{std::move(body)}});
      }
      if (text == "spawn" || text == "await" || text == "try") {
        advance();
        auto operand = parseExpression(40);
        if (!operand)
          return {};
        auto prefix = text == "spawn"   ? TaskPrefix::Spawn
                      : text == "await" ? TaskPrefix::Await
                                        : TaskPrefix::Try;
        std::optional<ExprId> placement;
        if (prefix == TaskPrefix::Spawn && word("on")) {
          advance();
          placement = parseExpression(40);
          if (!placement)
            return {};
        }
        return add({{token.span.begin, node(placement.value_or(*operand)).span.end},
                    TaskExpr{prefix, *operand, placement}});
      }
      if (text == "catch") {
        error("P011", "this construct is not implemented in the first frontend", token.span);
        return {};
      }
      advance();
      auto span = token.span;
      if (text == "devices" && consume(TokenKind::Dot)) {
        auto member = expect(TokenKind::Identifier, "expected device selector name");
        if (!member)
          return {};
        span.end = member->span.end;
      }
      return add({span, NameExpr{span}});
    }
    if (consume(TokenKind::Plus) || consume(TokenKind::Minus)) {
      auto value = parseExpression(40);
      if (!value)
        return {};
      return add({{token.span.begin, node(*value).span.end}, UnaryExpr{token.kind, *value}});
    }
    if (consume(TokenKind::LeftParen)) {
      auto expression = parseExpression();
      if (!expression)
        return {};
      auto end = expect(TokenKind::RightParen, "expected ')' after grouped expression");
      if (!end)
        return {};
      node(*expression).span = {token.span.begin, end->span.end};
      return expression;
    }
    if (consume(TokenKind::LeftBracket)) {
      auto elements = sequence(TokenKind::RightBracket);
      if (!elements)
        return {};
      return add(
          {{token.span.begin, tokens_[position_ - 1].span.end}, ArrayExpr{std::move(*elements)}});
    }
    error("P012", "expected an expression", token.span);
    if (result_.module.editorOnly &&
        (at(TokenKind::Comma) || at(TokenKind::RightParen) || at(TokenKind::RightBracket) ||
         at(TokenKind::RightBrace) || at(TokenKind::Newline) || at(TokenKind::Dedent) ||
         at(TokenKind::End) || at(TokenKind::Colon))) {
      const SourceSpan hole{token.span.begin, token.span.begin};
      return add({hole, NameExpr{hole}}); // Unknown name, never an invented typed value.
    }
    return {};
  }
  void synchronizeStatement() {
    // Consume a malformed nested suite together; don't mistake its dedent for
    // the enclosing function's end during recovery.
    std::size_t nested = 0;
    while (!at(TokenKind::End)) {
      if (at(TokenKind::Indent))
        ++nested;
      if (at(TokenKind::Dedent)) {
        if (!nested)
          return;
        --nested;
      }
      if (at(TokenKind::Newline) && !nested) {
        advance();
        return;
      }
      advance();
    }
  }
  void synchronizeFunction() {
    while (!at(TokenKind::End)) {
      if ((word("fn") || word("export") || word("from") || word("type") || word("enum") ||
           word("error")) &&
          source_->location(current().span.begin).second == 1)
        return;
      advance();
    }
  }
  ParseResult result_;
  std::shared_ptr<const SourceFile> source_;
  std::vector<Token> tokens_;
  SyntaxLimits limits_;
  std::size_t position_ = 0;
  std::size_t depth_ = 0;
  std::size_t typeNodes_ = 0;
};
} // namespace
ParseResult parse(std::shared_ptr<const SourceFile> source, SyntaxLimits limits) {
  limits.maxDiagnostics = std::max<std::size_t>(1, limits.maxDiagnostics);
  auto result = Parser(lex(std::move(source), limits), limits).run();
  if (result.diagnostics.size() >= limits.maxDiagnostics ||
      std::ranges::any_of(result.diagnostics, [](const auto &d) {
        return d.code == "P008" || d.code == "P009" || d.code == "P036";
      }))
    result.module.editorOnly = false;
  return result;
}
} // namespace kalandra
