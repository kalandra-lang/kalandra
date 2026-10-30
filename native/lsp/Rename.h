#pragma once
#include "SemanticQuery.h"
#include "SourceGraph.h"
#include <expected>
namespace kalandra::lsp {
struct SourceEdit {
  SourceLocation location;
  std::string text;
};
struct RenameResult {
  std::optional<SourceLocation> selection;
  std::string placeholder;
  std::vector<SourceEdit> edits;
};
[[nodiscard]] bool validRenameName(std::string_view name);
// No filesystem writes. A missing newName prepares the rename. The workspace and
// roots are explicit; resulting edits must remain inside that workspace.
[[nodiscard]] std::expected<RenameResult, std::string>
planRename(const std::vector<std::string> &roots, const std::string &workspace,
           std::span<const ModulePackage> packages, std::span<const SourceOverlay> overlays,
           const std::string &path, std::size_t offset, std::optional<std::string> newName,
           CheckOptions options = {}, const SourceGraphLoader &loader = {},
           const WritableSource &writable = {});
} // namespace kalandra::lsp
