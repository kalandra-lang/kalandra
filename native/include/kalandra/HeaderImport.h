#pragma once
#include "kalandra/Project.h"
#include <filesystem>

namespace kalandra {
struct NativeHeaderShim {
  std::string header, body;
};
struct HeaderTarget {
  std::string triple;
  std::filesystem::path sysroot;
  std::vector<std::filesystem::path> includes;
};
[[nodiscard]] std::expected<ParseResult, std::string>
loadApplicationModules(const std::string &entry, std::span<const ModulePackage> packages,
                       std::span<const PackageNode> graph, std::vector<SourceOverlay> overlays,
                       std::span<const HeaderImport> headers, std::string clang = {},
                       std::optional<std::filesystem::path> resources = {},
                       std::vector<std::string> *systemLinkArgs = nullptr,
                       const HeaderTarget *target = nullptr,
                       std::vector<NativeHeaderShim> *shims = nullptr);
}
