#pragma once
#include "kalandra/Syntax.h"
#include <algorithm>
namespace kalandra::lsp {
// Recover only unclosed call/group parentheses at the original end-of-buffer
// cursor. No arrays, records, other lexical errors or later source are hidden.
inline std::optional<std::size_t> trailingCallClosures(const LexResult &tokens,
                                                       std::string_view text, std::size_t offset) {
  if (offset > text.size() || tokens.diagnostics.empty() ||
      !std::ranges::all_of(tokens.diagnostics, [](const auto &d) { return d.code == "L005"; }) ||
      !std::ranges::all_of(text.substr(offset),
                           [](char c) { return c == ' ' || c == '\n' || c == '\r'; }))
    return {};
  std::vector<TokenKind> delimiters;
  for (const auto &token : tokens.tokens) {
    if (token.kind == TokenKind::LeftParen || token.kind == TokenKind::LeftBracket ||
        token.kind == TokenKind::LeftBrace)
      delimiters.push_back(token.kind);
    else if (token.kind == TokenKind::RightParen || token.kind == TokenKind::RightBracket ||
             token.kind == TokenKind::RightBrace) {
      if (delimiters.empty())
        return {};
      delimiters.pop_back(); // Mismatches already fail the lexer diagnostic gate.
    }
  }
  if (delimiters.empty() || delimiters.size() > 8 ||
      !std::ranges::all_of(delimiters, [](auto kind) { return kind == TokenKind::LeftParen; }))
    return {};
  return delimiters.size();
}
} // namespace kalandra::lsp
