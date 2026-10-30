#pragma once
#include "kalandra/Syntax.h"
namespace kalandra {
struct FormatResult {
  std::optional<std::string> text;
  std::vector<Diagnostic> diagnostics;
};
// Syntax-only, single-buffer formatting. No imports, type checking or writes.
// Preserve token spellings/layout and comments; reject unverified output.
[[nodiscard]] FormatResult formatSource(std::shared_ptr<const SourceFile> source,
                                        SyntaxLimits limits = {});
} // namespace kalandra
