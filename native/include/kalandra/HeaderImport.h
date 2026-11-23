#pragma once
#include "kalandra/Project.h"
#include <filesystem>

namespace kalandra {
[[nodiscard]] std::expected<ParseResult, std::string>
loadApplicationModules(const std::string &entry, std::span<const ModulePackage> packages,
                       std::span<const PackageNode> graph, std::vector<SourceOverlay> overlays,
                       std::span<const HeaderImport> headers, std::string clang = {},
                       std::optional<std::filesystem::path> resources = {},
                       std::vector<std::string> *systemLinkArgs = nullptr);
}
