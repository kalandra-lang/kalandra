#pragma once
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace kalandra::lsp {
struct DiscoveryLimits {
  std::size_t maxFiles = 512;
  std::size_t maxEntries = 20000;
  std::size_t maxDepth = 32;
};
[[nodiscard]] bool containsPath(std::string_view root, std::string_view path);
[[nodiscard]] std::optional<std::string> containingWorkspace(const std::vector<std::string> &roots,
                                                             std::string_view path);
// Fresh, bounded traversal. No source reads, symlink following, or silent truncation.
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
discoverSources(const std::string &root, DiscoveryLimits limits = {});
} // namespace kalandra::lsp
