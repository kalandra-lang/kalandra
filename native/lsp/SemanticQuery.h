#pragma once
#include "kalandra/Semantics.h"
namespace kalandra::lsp {
struct SourceLocation {
  std::shared_ptr<const SourceFile> source;
  SourceSpan span;
};
struct SemanticToken {
  TypedModule module;
  SourceLocation token;
  std::optional<SourceSpan> declaration;
  std::optional<ValueType> type;
  std::optional<FunctionId> function;
  std::optional<std::size_t> record;
};
[[nodiscard]] std::optional<SourceLocation> sourceLocation(std::shared_ptr<const SourceFile> source,
                                                           SourceSpan span);
// Resolve once from checked identities for both hover and definition. No name guessing.
[[nodiscard]] std::optional<SemanticToken> semanticToken(ParseResult syntax, std::string_view path,
                                                         std::size_t offset,
                                                         CheckOptions options = {});
[[nodiscard]] std::optional<SourceLocation> definition(ParseResult syntax, std::string_view path,
                                                       std::size_t offset,
                                                       CheckOptions options = {});
struct SourceOccurrence {
  SourceSpan span, declaration;
  bool secondary = false;
};
struct SourceIndex {
  TypedModule module;
  std::vector<SourceOccurrence> occurrences;
};
// All compiler-resolved identifier relationships, including secondary pattern roles.
[[nodiscard]] std::optional<SourceIndex> sourceIndex(ParseResult syntax, CheckOptions options = {});
// One check and one indexed identifier pass per graph. A failed check returns nullopt.
[[nodiscard]] std::optional<std::vector<SourceLocation>>
references(ParseResult syntax, const SourceLocation &declaration, bool includeDeclaration,
           CheckOptions options = {});
} // namespace kalandra::lsp
