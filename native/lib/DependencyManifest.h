#pragma once
#include "kalandra/Project.h"
#include <string>
#include <string_view>

namespace kalandra::detail {
struct DependencyManifest {
  std::string path;
  std::string contents;
  std::vector<NativeDependency> dependencies;
};
// Read dependency declarations from either an application or a source library.
std::expected<DependencyManifest, std::string> loadDependencyManifest(std::string path);
// Edit an already validated manifest using the TOML parser's source locations.
std::string withoutDependency(const std::string &source, std::string_view alias);
} // namespace kalandra::detail
