#include "kalandra/Syntax.h"
#include <algorithm>

namespace kalandra {
namespace {
bool digit(char value) { return value >= '0' && value <= '9'; }
bool identifierStart(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}
bool identifierPart(char value) { return identifierStart(value) || digit(value); }
// Reject truncated, overlong, surrogate, and out-of-range UTF-8 sequences.
std::optional<std::size_t> invalidUtf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80) {
      ++i;
      continue;
    }
    const std::size_t length = first >= 0xc2 && first <= 0xdf   ? 2
                               : first >= 0xe0 && first <= 0xef ? 3
                               : first >= 0xf0 && first <= 0xf4 ? 4
                                                                : 0;
    if (!length || length > text.size() - i)
      return i;
    std::uint32_t point = first & (0x7fu >> length);
    for (std::size_t j = 1; j < length; ++j) {
      auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0) != 0x80)
        return i + j;
      point = (point << 6) | (next & 0x3f);
    }
    if ((length == 2 && point < 0x80) || (length == 3 && point < 0x800) ||
        (length == 4 && point < 0x10000) || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff))
      return i;
    i += length;
  }
  return {};
}
class Lexer {
public:
  Lexer(std::shared_ptr<const SourceFile> source, SyntaxLimits limits)
      : result_{std::move(source), {}, {}}, limits_(limits), text_(result_.source->text()) {}
  LexResult run() {
    if (text_.size() > limits_.maxSourceBytes) {
      error("L001", "source exceeds the configured size limit", {0, 0});
      emit(TokenKind::End, text_.size(), text_.size());
      return std::move(result_);
    }
    if (auto invalid = invalidUtf8(text_)) {
      error("L002", "source is not valid UTF-8", {*invalid, *invalid + 1});
      emit(TokenKind::End, text_.size(), text_.size());
      return std::move(result_);
    }
    while (position_ < text_.size() && !stopped_) {
      if (lineStart_)
        indentation();
      if (position_ >= text_.size() || stopped_)
        break;
      const auto begin = position_;
      const char value = text_[position_];
      if (value == ' ') {
        ++position_;
        continue;
      }
      if (value == '\t') {
        error("L003", "tabs are not supported; use spaces", {position_, position_ + 1});
        ++position_;
        continue;
      }
      if (value == '#') {
        while (position_ < text_.size() && text_[position_] != '\n' && text_[position_] != '\r')
          ++position_;
        continue;
      }
      if (value == '\n' || value == '\r') {
        newline();
        continue;
      }
      if (identifierStart(value)) {
        while (position_ < text_.size() && identifierPart(text_[position_]))
          ++position_;
        emit(TokenKind::Identifier, begin, position_);
        continue;
      }
      if (digit(value)) {
        number();
        continue;
      }
      if (value == '"') {
        string();
        continue;
      }
      ++position_;
      switch (value) {
      case '(':
        open(TokenKind::LeftParen, begin);
        break;
      case '[':
        open(TokenKind::LeftBracket, begin);
        break;
      case '{':
        open(TokenKind::LeftBrace, begin);
        break;
      case ')':
        close(TokenKind::RightParen, TokenKind::LeftParen, begin);
        break;
      case ']':
        close(TokenKind::RightBracket, TokenKind::LeftBracket, begin);
        break;
      case '}':
        close(TokenKind::RightBrace, TokenKind::LeftBrace, begin);
        break;
      case ':':
        emit(TokenKind::Colon, begin, position_);
        break;
      case ',':
        emit(TokenKind::Comma, begin, position_);
        break;
      case '?':
        emit(TokenKind::Question, begin, position_);
        break;
      case '+':
        emit(TokenKind::Plus, begin, position_);
        break;
      case '-': {
        const auto kind = take('>') ? TokenKind::Arrow : TokenKind::Minus;
        emit(kind, begin, position_);
        break;
      }
      case '*':
        emit(TokenKind::Star, begin, position_);
        break;
      case '/':
        emit(TokenKind::Slash, begin, position_);
        break;
      case '%':
        emit(TokenKind::Percent, begin, position_);
        break;
      case '.':
        emit(TokenKind::Dot, begin, position_);
        break;
      case '=': {
        // Consume first: function argument evaluation order differs between
        // native ABIs, so emit must read the updated end after take().
        const auto kind = take('>')   ? TokenKind::FatArrow
                          : take('=') ? TokenKind::Comparison
                                      : TokenKind::Equal;
        emit(kind, begin, position_);
        break;
      }
      case '|':
        emit(TokenKind::Pipe, begin, position_);
        break;
      case '!': {
        const auto kind = take('=') ? TokenKind::Comparison : TokenKind::Bang;
        emit(kind, begin, position_);
        break;
      }
      case '<':
      case '>':
        take('=');
        emit(TokenKind::Comparison, begin, position_);
        break;
      default:
        error("L004",
              static_cast<unsigned char>(value) >= 0x80
                  ? "non-ASCII identifiers are not supported in this first frontend"
                  : "unexpected source character",
              {begin, position_});
      }
    }
    for (const auto &token : delimiters_)
      error("L005", "unclosed delimiter", token.span);
    if (limits_.editorRecovery && delimiters_.size() <= 8)
      while (!delimiters_.empty())
        recoverClose(position_);
    if (lineHasTokens_)
      emit(TokenKind::Newline, position_, position_);
    while (indents_.size() > 1) {
      indents_.pop_back();
      emit(TokenKind::Dedent, position_, position_);
    }
    // Always terminate even after a token limit is reached.
    result_.tokens.push_back({TokenKind::End, {position_, position_}});
    return std::move(result_);
  }

private:
  void error(std::string code, std::string message, SourceSpan span) {
    if (result_.diagnostics.size() < limits_.maxDiagnostics)
      result_.diagnostics.push_back({std::move(code), std::move(message), span});
  }
  void emit(TokenKind kind, std::size_t begin, std::size_t end) {
    if (result_.tokens.size() >= limits_.maxTokens) {
      error("L006", "token limit exceeded", {begin, end});
      stopped_ = true;
      return;
    }
    result_.tokens.push_back({kind, {begin, end}});
    if (kind != TokenKind::Indent && kind != TokenKind::Dedent && kind != TokenKind::Newline &&
        kind != TokenKind::End) {
      lambdaColon_ = false;
      if (kind == TokenKind::LeftParen && functionStart_ &&
          *functionStart_ + 1 == delimiters_.size())
        functionHeaders_.push_back(*functionStart_);
      functionStart_.reset();
      if (kind == TokenKind::Identifier && text_.substr(begin, end - begin) == "fn" &&
          !delimiters_.empty() &&
          (result_.tokens.size() < 2 ||
           result_.tokens[result_.tokens.size() - 2].kind != TokenKind::Dot))
        functionStart_ = delimiters_.size();
      if (kind == TokenKind::Comma)
        while (!functionHeaders_.empty() && functionHeaders_.back() == delimiters_.size())
          functionHeaders_.pop_back();
      if (kind == TokenKind::Colon && !functionHeaders_.empty() &&
          functionHeaders_.back() == delimiters_.size()) {
        lambdaColon_ = true;
        while (!functionHeaders_.empty() && functionHeaders_.back() == delimiters_.size())
          functionHeaders_.pop_back();
      }
    }
    if (kind != TokenKind::Indent && kind != TokenKind::Dedent && kind != TokenKind::Newline &&
        kind != TokenKind::End)
      lineHasTokens_ = true;
  }
  bool take(char value) {
    if (position_ < text_.size() && text_[position_] == value) {
      ++position_;
      return true;
    }
    return false;
  }
  void indentation() {
    lineStart_ = false;
    const auto begin = position_;
    while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t')) {
      if (text_[position_] == '\t')
        error("L003", "indent with spaces, not tabs", {position_, position_ + 1});
      ++position_;
    }
    if (position_ == text_.size() || text_[position_] == '#' || text_[position_] == '\n' ||
        text_[position_] == '\r')
      return;
    const auto width = position_ - begin;
    lineIndent_ = width;
    if (limits_.editorRecovery && width == 0 && !delimiters_.empty()) {
      const auto rest = text_.substr(position_);
      const auto namedFunction = [](std::string_view text) {
        if (!text.starts_with("fn "))
          return false;
        const auto name = text.find_first_not_of(' ', 3);
        return name != text.npos && text[name] != '(';
      };
      const bool declaration =
          namedFunction(rest) || (rest.starts_with("export ") && namedFunction(rest.substr(7))) ||
          rest.starts_with("type ") || rest.starts_with("enum ") || rest.starts_with("error ") ||
          rest.starts_with("from ") || rest.starts_with("export from ") ||
          rest.starts_with("export type ") || rest.starts_with("export enum ") ||
          rest.starts_with("export error ");
      if (declaration && delimiters_.size() <= 8) {
        const auto boundary = position_ ? position_ - 1 : 0;
        while (!delimiters_.empty()) {
          error("L005", "unclosed delimiter before declaration", delimiters_.back().span);
          recoverClose(boundary);
        }
        emit(TokenKind::Newline, boundary, boundary);
        lineHasTokens_ = false;
      }
    }
    while (!layouts_.empty() && delimiters_.size() == layouts_.back().depth) {
      auto &layout = layouts_.back();
      if (width <= layout.base) {
        while (indents_.size() > layout.floor) {
          indents_.pop_back();
          emit(TokenKind::Dedent, position_, position_);
        }
        layouts_.pop_back();
        continue;
      }
      if (layout.pending) {
        layout.pending = false;
        indent(width, begin);
        return;
      }
      break;
    }
    if (!delimiters_.empty() && (layouts_.empty() || delimiters_.size() != layouts_.back().depth))
      return;
    if (width > indents_.back()) {
      indent(width, begin);
    } else {
      while (width < indents_.back()) {
        indents_.pop_back();
        emit(TokenKind::Dedent, position_, position_);
      }
      if (width != indents_.back())
        error("L008", "dedent does not match an enclosing indentation level", {begin, position_});
    }
  }
  void indent(std::size_t width, std::size_t begin) {
    if (indents_.size() > limits_.maxDepth) {
      error("L007", "indentation nesting limit exceeded", {begin, position_});
      stopped_ = true;
      return;
    }
    indents_.push_back(width);
    emit(TokenKind::Indent, position_, position_);
  }
  void newline() {
    const auto begin = position_++;
    if (text_[begin] == '\r' && !take('\n'))
      error("L009", "use LF or CRLF line endings", {begin, position_});
    if (lambdaColon_ && !delimiters_.empty() &&
        (layouts_.empty() || layouts_.back().depth != delimiters_.size()))
      layouts_.push_back(
          {delimiters_.size(), std::max(lineIndent_, indents_.back()), indents_.size(), true});
    if ((delimiters_.empty() ||
         (!layouts_.empty() && layouts_.back().depth == delimiters_.size())) &&
        lineHasTokens_) {
      emit(TokenKind::Newline, begin, position_);
      lineHasTokens_ = false;
    }
    lambdaColon_ = false;
    lineStart_ = true;
  }
  void recoverClose(std::size_t at) {
    while (!layouts_.empty() && layouts_.back().depth >= delimiters_.size()) {
      if (lineHasTokens_) {
        emit(TokenKind::Newline, at, at);
        lineHasTokens_ = false;
      }
      while (indents_.size() > layouts_.back().floor) {
        indents_.pop_back();
        emit(TokenKind::Dedent, at, at);
      }
      layouts_.pop_back();
    }
    const auto kind = delimiters_.back().kind;
    delimiters_.pop_back();
    emit(kind == TokenKind::LeftParen     ? TokenKind::RightParen
         : kind == TokenKind::LeftBracket ? TokenKind::RightBracket
                                          : TokenKind::RightBrace,
         at, at);
    while (!functionHeaders_.empty() && functionHeaders_.back() >= delimiters_.size())
      functionHeaders_.pop_back();
  }
  void open(TokenKind kind, std::size_t begin) {
    if (delimiters_.size() >= limits_.maxDepth) {
      error("L010", "delimiter nesting limit exceeded", {begin, position_});
      stopped_ = true;
      return;
    }
    delimiters_.push_back({kind, {begin, position_}});
    emit(kind, begin, position_);
  }
  void close(TokenKind kind, TokenKind expected, std::size_t begin) {
    if (!layouts_.empty() && layouts_.back().depth == delimiters_.size()) {
      error("L016", "close the delimiter on a dedented line after the anonymous body",
            {begin, position_});
      while (indents_.size() > layouts_.back().floor) {
        indents_.pop_back();
        emit(TokenKind::Dedent, begin, begin);
      }
      layouts_.pop_back();
    }
    while (!functionHeaders_.empty() && functionHeaders_.back() >= delimiters_.size())
      functionHeaders_.pop_back();
    if (delimiters_.empty() || delimiters_.back().kind != expected) {
      error("L011", "mismatched closing delimiter", {begin, position_});
      if (limits_.editorRecovery && delimiters_.size() <= 8) {
        while (!delimiters_.empty() && delimiters_.back().kind != expected)
          recoverClose(begin);
        if (delimiters_.empty())
          return;
        delimiters_.pop_back();
      }
    } else
      delimiters_.pop_back();
    emit(kind, begin, position_);
  }
  void number() {
    const auto begin = position_;
    while (position_ < text_.size() && digit(text_[position_]))
      ++position_;
    if (take('.')) {
      const auto digits = position_;
      while (position_ < text_.size() && digit(text_[position_]))
        ++position_;
      if (digits == position_)
        error("L012", "expected digits after decimal point", {begin, position_});
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (!take('+'))
        take('-');
      const auto digits = position_;
      while (position_ < text_.size() && digit(text_[position_]))
        ++position_;
      if (digits == position_)
        error("L013", "expected digits in exponent", {begin, position_});
    }
    if (position_ < text_.size() && identifierStart(text_[position_])) {
      while (position_ < text_.size() && identifierPart(text_[position_]))
        ++position_;
      error("L014", "numeric suffixes and non-decimal literals are not supported",
            {begin, position_});
    }
    emit(TokenKind::Number, begin, position_);
  }
  void string() {
    const auto begin = position_++;
    bool closed = false;
    while (position_ < text_.size() && text_[position_] != '\n' && text_[position_] != '\r') {
      const auto current = text_[position_++];
      if (current == '"') {
        closed = true;
        break;
      }
      if (current == '\\' && position_ < text_.size() && text_[position_] != '\n' &&
          text_[position_] != '\r')
        ++position_;
    }
    if (!closed)
      error("L015", "unterminated string literal", {begin, position_});
    emit(TokenKind::String, begin, position_);
  }
  LexResult result_;
  SyntaxLimits limits_;
  std::string_view text_;
  std::size_t position_ = 0;
  std::vector<std::size_t> indents_{0};
  std::vector<Token> delimiters_;
  struct Layout {
    std::size_t depth, base, floor;
    bool pending;
  };
  // Delimited anonymous suites temporarily restore layout at their delimiter
  // depth. Deeper calls and collections still use ordinary line continuation.
  std::vector<Layout> layouts_;
  std::vector<std::size_t> functionHeaders_;
  std::optional<std::size_t> functionStart_;
  std::size_t lineIndent_ = 0;
  bool lambdaColon_ = false;
  bool lineStart_ = true;
  bool lineHasTokens_ = false;
  bool stopped_ = false;
};
} // namespace
LexResult lex(std::shared_ptr<const SourceFile> source, SyntaxLimits limits) {
  // A zero diagnostic budget must not silently turn invalid input into success.
  limits.maxDiagnostics = std::max<std::size_t>(1, limits.maxDiagnostics);
  return Lexer(std::move(source), limits).run();
}
} // namespace kalandra
