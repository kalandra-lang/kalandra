#pragma once
#include "Hover.h"
#include "SourceGraph.h"
namespace kalandra::lsp {
// Current graph only. May close at most eight trailing parentheses in a temporary
// overlay; never invents arguments, changes buffers, or exposes partial typed IR.
[[nodiscard]] std::optional<SignatureInfo>
signatureAt(const std::string &path, const std::string &text, std::size_t offset,
            const std::vector<ModulePackage> &packages = {},
            std::vector<SourceOverlay> overlays = {}, CheckOptions options = {},
            const SourceGraphLoader &loader = {});
} // namespace kalandra::lsp
