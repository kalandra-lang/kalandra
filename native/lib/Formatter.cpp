#include "kalandra/Formatter.h"
#include <algorithm>
#include <unordered_set>
namespace kalandra {
namespace {
bool layout(TokenKind kind) {
  return kind == TokenKind::Indent || kind == TokenKind::Dedent || kind == TokenKind::Newline ||
         kind == TokenKind::End;
}
bool closing(TokenKind kind) {
  return kind == TokenKind::RightParen || kind == TokenKind::RightBracket ||
         kind == TokenKind::RightBrace;
}
bool spaced(const SourceFile &source, const Token &left, const Token &right,
            const std::unordered_set<std::size_t> &unary) {
  const auto a = left.kind, b = right.kind;
  if (a == TokenKind::Number && b == TokenKind::Dot)
    return true; // Joining an integer and dot would start a malformed decimal literal.
  if (b == TokenKind::Comma || b == TokenKind::Colon || b == TokenKind::Dot || a == TokenKind::Dot)
    return false;
  if (b == TokenKind::RightParen || b == TokenKind::RightBracket)
    return false;
  if (a == TokenKind::LeftParen || a == TokenKind::LeftBracket)
    return false;
  if (a == TokenKind::LeftBrace && b == TokenKind::RightBrace)
    return false;
  if (unary.contains(left.span.begin) && source.slice(left.span) != "not")
    return false;
  if (b == TokenKind::LeftParen || b == TokenKind::LeftBracket) {
    if (a == TokenKind::Identifier) {
      const auto word = source.slice(left.span);
      return word == "if" || word == "match" || word == "not" || word == "fail" || word == "try" ||
             word == "await" || word == "spawn" || word == "and" || word == "or" || word == "on";
    }
    if (a == TokenKind::Number || a == TokenKind::String || closing(a))
      return false;
  }
  return true;
}
bool equivalent(const LexResult &before, const LexResult &after) {
  if (!after.diagnostics.empty() || before.tokens.size() != after.tokens.size())
    return false;
  for (std::size_t i = 0; i < before.tokens.size(); ++i) {
    const auto &a = before.tokens[i], &b = after.tokens[i];
    if (a.kind != b.kind ||
        (!layout(a.kind) && before.source->slice(a.span) != after.source->slice(b.span)))
      return false;
  }
  return true;
}
std::optional<std::string> candidate(const LexResult &tokens,
                                     const std::unordered_set<std::size_t> &unary,
                                     bool canonicalIndent, std::size_t maximum) {
  const auto &source = *tokens.source;
  const auto &text = source.text();
  std::string output;
  output.reserve(text.size());
  const auto append = [&](std::string_view part) {
    if (part.size() > maximum - output.size())
      return false;
    output += part;
    return true;
  };
  std::vector<std::size_t> widths{0};
  std::size_t index = 0, lastContent = 0;
  for (std::size_t start = 0; start < text.size();) {
    auto end = text.find_first_of("\r\n", start);
    if (end == text.npos)
      end = text.size();
    auto content = start;
    while (content < end && text[content] == ' ')
      ++content;
    std::vector<const Token *> line;
    while (index < tokens.tokens.size() && tokens.tokens[index].span.begin < end) {
      const auto &token = tokens.tokens[index++];
      if (token.kind == TokenKind::Indent)
        widths.push_back(token.span.begin - start);
      else if (token.kind == TokenKind::Dedent) {
        if (widths.size() > 1)
          widths.pop_back();
      } else if (!layout(token.kind))
        line.push_back(&token);
    }
    // Comments are excluded by the lexer. Retain their complete bytes, including
    // trailing spaces, and their original physical line relative to code.
    const auto tail = line.empty() ? content : line.back()->span.end;
    auto comment = tail;
    while (comment < end && text[comment] == ' ')
      ++comment;
    const bool hasComment = comment < end && text[comment] == '#';
    if (!line.empty() || hasComment) {
      auto indent = content - start;
      if (canonicalIndent) {
        std::size_t level = 0;
        while (level + 1 < widths.size() && widths[level + 1] <= indent)
          ++level;
        indent = level * 2 + (indent > widths[level] ? 2 : 0);
      }
      if (indent > maximum - output.size() || !append(std::string(indent, ' ')))
        return {};
      const Token *previous = nullptr;
      for (const auto *token : line) {
        if (previous && spaced(source, *previous, *token, unary) && !append(" "))
          return {};
        if (!append(source.slice(token->span)))
          return {};
        previous = token;
      }
      if (hasComment && ((!line.empty() && !append("  ")) ||
                         !append(std::string_view(text).substr(comment, end - comment))))
        return {};
      lastContent = output.size();
    }
    if (!append("\n"))
      return {};
    start = end;
    if (start < text.size() && text[start++] == '\r' && start < text.size() && text[start] == '\n')
      ++start;
  }
  output.resize(lastContent);
  if (!output.empty() && !append("\n"))
    return {};
  return output;
}
} // namespace
FormatResult formatSource(std::shared_ptr<const SourceFile> source, SyntaxLimits limits) {
  auto tokens = lex(source, limits);
  if (!tokens.diagnostics.empty())
    return {{}, std::move(tokens.diagnostics)};
  const bool triviaOnly =
      std::ranges::all_of(tokens.tokens, [](const auto &token) { return layout(token.kind); });
  auto syntax = parse(source, limits);
  // Editors also format blank/comment-only buffers, although the compiler
  // requires a declaration when treating one as a complete compilation unit.
  if (!syntax.ok() && !triviaOnly)
    return {{}, std::move(syntax.diagnostics)};
  std::unordered_set<std::size_t> unary;
  for (const auto &expression : syntax.module.expressions)
    if (const auto *operation = std::get_if<UnaryExpr>(&expression.value)) {
      auto token =
          std::lower_bound(tokens.tokens.begin(), tokens.tokens.end(), expression.span.begin,
                           [](const Token &t, std::size_t at) { return t.span.begin < at; });
      while (token != tokens.tokens.end() &&
             (token->kind == TokenKind::LeftParen || layout(token->kind)))
        ++token;
      if (token != tokens.tokens.end() && token->kind == operation->operation)
        unary.insert(token->span.begin);
    }
  for (const bool canonicalIndent : {true, false}) {
    auto text = candidate(tokens, unary, canonicalIndent, limits.maxSourceBytes);
    if (!text)
      continue;
    auto formatted = std::make_shared<const SourceFile>(source->path(), *text);
    if (!equivalent(tokens, lex(formatted, limits)))
      continue;
    auto reparsed = parse(formatted, limits);
    if ((!reparsed.ok() && !triviaOnly) ||
        reparsed.module.expressions.size() != syntax.module.expressions.size())
      continue;
    bool namesEqual = true;
    // Device selectors currently use a multi-token NameExpr source payload.
    // Token equivalence alone must not silently repair its significant spelling.
    for (std::size_t i = 0; i < syntax.module.expressions.size(); ++i)
      if (const auto *name = std::get_if<NameExpr>(&syntax.module.expressions[i].value)) {
        const auto *other = std::get_if<NameExpr>(&reparsed.module.expressions[i].value);
        if (!other || source->slice(name->name) != formatted->slice(other->name)) {
          namesEqual = false;
          break;
        }
      }
    if (namesEqual)
      return {std::move(text), {}};
  }
  return {{},
          {{"F001",
            "cannot format source within limits while preserving tokens, names and block structure",
            {0, 0}}}};
}
} // namespace kalandra
