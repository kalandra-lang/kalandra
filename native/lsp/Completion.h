#pragma once
#include "SourceGraph.h"
#include "kalandra/Semantics.h"
namespace kalandra::lsp {
struct CompletionInfo {
  SourceSpan replacement; // Original UTF-8 range: whole identifier or import-string contents.
  std::vector<ScopeName> names;
  bool incomplete = false;
  bool appendFieldColon = false; // Insert a separator only for a bare construction label.
};
// Temporary identifier/field probes query checked scopes and nominal members; parsed relative
// import strings also offer bounded filesystem/open-buffer paths. No source writes,
// stale modules, auto-imports, or executable partial type information. Structural editor
// recovery retains real diagnostics and exposes only a checked query snapshot.
[[nodiscard]] CompletionInfo completeNames(const std::string &path, const std::string &text,
                                           std::size_t offset,
                                           const std::vector<ModulePackage> &packages = {},
                                           std::vector<SourceOverlay> overlays = {},
                                           CheckOptions options = {},
                                           const SourceGraphLoader &loader = {});
} // namespace kalandra::lsp
