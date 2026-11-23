#pragma once
#include "kalandra/GitSnapshot.h"
#include <functional>
namespace kalandra {
struct PackageInstallLimits {
  std::size_t maxPackages = 64;
  std::size_t maxDepth = 32;
  std::size_t maxSourceEntries = 32768;
  std::size_t maxSourceBytes = 128 * 1024 * 1024;
  std::size_t maxLockBytes = 1024 * 1024;
  GitSnapshotLimits snapshot = {};
};
struct InstalledNativeProject {
  std::vector<PackageNode> nodes;
  std::vector<HeaderImport> headers = {};
  std::vector<std::string> libraries = {};
};
using PackageTrace = std::function<void(std::string_view, std::string_view)>;
struct PackageInstallOptions {
  bool frozen = false;
  bool offline = false;
  // Explicit root aliases to re-resolve. Unchanged package identities keep private pins.
  std::vector<std::string> updateRoots = {};
  PackageTrace trace = {};
};
// Explicit installation with owner-local revision pins. Publishes a lock only
// after the entire declared graph and reachable source imports validate.
[[nodiscard]] std::expected<void, std::string>
installNativeProject(std::string manifest, PackageInstallLimits limits = {},
                     PackageInstallOptions options = {});
// Remove one direct Git dependency and prune its unreachable pins without fetching.
[[nodiscard]] std::expected<void, std::string>
removeNativeDependency(std::string manifest, std::string_view alias,
                       PackageInstallLimits limits = {});
// Read one public lock generation and verify all selected snapshots. Never runs Git.
[[nodiscard]] std::expected<InstalledNativeProject, std::string>
loadInstalledNativeProject(const NativeProject &, PackageInstallLimits limits = {});
// Protect the reserved lock/cache namespaces and published cache file aliases, including
// old generations and unused snapshots, without modifying anything.
[[nodiscard]] std::expected<void, std::string>
protectNativePackageOutput(const NativeProject &, std::string output,
                           PackageInstallLimits limits = {});
} // namespace kalandra
