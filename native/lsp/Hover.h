#pragma once
#include "kalandra/Semantics.h"
namespace kalandra::lsp {
struct HoverInfo {
  std::string text;
  std::shared_ptr<const SourceFile> source;
  SourceSpan span;
};
struct SignatureInfo {
  std::string label;
  std::vector<std::string> parameters;
  std::optional<std::size_t> activeParameter;
};
// Checked calls or bounded validated-callee snapshots for a missing argument.
[[nodiscard]] std::optional<SignatureInfo> signatureInfo(ParseResult syntax, std::string_view path,
                                                         std::size_t offset,
                                                         CheckOptions options = {},
                                                         bool requireFieldProjection = false,
                                                         bool requireNameProjection = false);
// Checked semantic information only. Failed graphs never supply partial/stale types.
[[nodiscard]] std::optional<HoverInfo> hoverInfo(ParseResult syntax, std::string_view path,
                                                 std::size_t offset, CheckOptions options = {});
} // namespace kalandra::lsp
