#pragma once
#include "kalandra/Syntax.h"
namespace kalandra {
struct NativeDependency {
  std::string name;
  std::string repository;
  std::string revision; // Requested commit or ref; source identity uses the locked commit.
};
struct NativeProject {
  std::string manifest;
  std::string entry;
  std::vector<ModulePackage> packages;
  // Also protect a declared public entry when building the example application.
  std::optional<std::string> libraryEntry = {};
  std::vector<NativeDependency> dependencies = {};
  std::string manifestContents = {};
};
struct NativePackage {
  std::string manifest;
  std::string name;
  std::string version;
  std::string entry;
  std::optional<std::string> projectEntry;
  std::vector<ModulePackage> packages;
  std::vector<NativeDependency> dependencies = {};
  // Metadata does not select a resolved identity. The offline resolver supplies
  // that identity and dependency edges; graph validation remains in the loader.
  [[nodiscard]] PackageNode node(std::string identity,
                                 std::vector<PackageDependency> dependencies = {}) const;
};
struct ProjectLimits {
  std::size_t maxBytes = 256 * 1024;
  std::size_t maxPackages = 128;
};
// Explicit native manifests only; no ancestor discovery, installation or network work.
[[nodiscard]] std::expected<NativeProject, std::string>
loadNativeProject(std::string path, ProjectLimits limits = {});
// Select the library entry from a [package] manifest (optionally also [project]).
// Validates metadata and paths without loading .k sources or fetching dependencies.
[[nodiscard]] std::expected<NativePackage, std::string>
loadNativePackage(std::string path, ProjectLimits limits = {});
// Create an offline starter in a new directory. Never merges into or replaces
// an existing file/directory/symlink; the parent directory must already exist.
[[nodiscard]] std::expected<void, std::string> createNativeProject(std::string path);
} // namespace kalandra
