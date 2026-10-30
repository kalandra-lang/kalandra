#pragma once
#include "kalandra/Syntax.h"
#include <functional>
namespace kalandra::lsp {
// Request-local graph loading also applies to completion probes and proposed edits.
using SourceGraphLoader = std::function<std::expected<ParseResult, std::string>(
    const std::string &, std::span<const SourceOverlay>, bool editorRecovery)>;
using WritableSource = std::function<bool(std::string_view)>;
} // namespace kalandra::lsp
