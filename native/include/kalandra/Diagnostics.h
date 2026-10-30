#pragma once
#include "kalandra/Syntax.h"
namespace kalandra {
struct CheckSummary {
  std::size_t functions = 0, values = 0;
};
// Versioned checker response. Source spans map to original files before UTF-16
// positions are computed. Without source storage, ranges are explicitly null.
[[nodiscard]] std::string renderDiagnosticsJson(const SourceFile *source,
                                                std::span<const Diagnostic> diagnostics,
                                                std::optional<CheckSummary> summary = {},
                                                std::string_view file = {});
} // namespace kalandra
