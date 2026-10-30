#include "kalandra/GitRepository.h"
#include "kalandra/PackageInstall.h"
#include <filesystem>
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 7)
    return 2;
  const std::string mode = argv[1], budget = argv[6];
  kalandra::GitSnapshotLimits limits;
  if (std::string_view(argv[5]) != "-")
    limits.certificateAuthorityFile = argv[5];
  if (budget == "timeout")
    limits.processSeconds = 1;
  else if (budget == "bytes")
    limits.maxObjectBytes = 65536;
  else if (budget == "entries")
    limits.maxObjectFiles = 1;
  else if (budget == "source")
    limits.maxSourceBytes = 64;
  else if (budget == "listing")
    limits.maxListingBytes = 32;
  else if (budget != "normal")
    return 2;
  if (mode == "project" || mode == "update") {
    kalandra::PackageInstallLimits projectLimits;
    projectLimits.snapshot = limits;
    kalandra::PackageInstallOptions options;
    if (mode == "update")
      options.updateRoots.emplace_back(argv[4]);
    auto result = kalandra::installNativeProject(argv[2], projectLimits, options);
    if (!result) {
      std::cerr << result.error() << '\n';
      return 1;
    }
    return 0;
  }
  if (mode == "validate") {
    auto result = kalandra::gitTransport(argv[3]);
    if (!result) {
      std::cerr << result.error() << '\n';
      return 1;
    }
    return 0;
  }
  std::filesystem::create_directories(argv[2]);
  if (mode == "revision") {
    auto result = kalandra::installGitRevision(argv[2], argv[3], argv[4], limits);
    if (!result) {
      std::cerr << result.error() << '\n';
      return 1;
    }
    std::cout << result->commit << '\n'
              << result->snapshot.identity << '\n'
              << result->snapshot.digest << '\n';
    return 0;
  }
  const kalandra::GitSnapshotRequest request{argv[3], argv[4]};
  auto result = mode == "load" ? kalandra::loadGitSnapshot(argv[2], request, limits)
                               : kalandra::installGitSnapshot(argv[2], request, limits);
  if (!result) {
    std::cerr << result.error() << '\n';
    return 1;
  }
  std::cout << result->identity << '\n' << result->digest << '\n';
}
