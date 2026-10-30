#include "kalandra/PackageInstall.h"
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  const std::string mode = argv[1], limit = argv[3];
  kalandra::PackageInstallLimits limits;
  if (limit == "packages")
    limits.maxPackages = 0;
  else if (limit == "depth")
    limits.maxDepth = 1;
  else if (limit == "shared-depth")
    limits.maxDepth = 2;
  else if (limit == "entries")
    limits.maxSourceEntries = 0;
  else if (limit == "bytes")
    limits.maxSourceBytes = 1;
  else if (limit == "lock")
    limits.maxLockBytes = 1;
  else if (limit != "normal")
    return 2;
  if (mode == "install") {
    auto installed = kalandra::installNativeProject(argv[2], limits);
    if (!installed) {
      std::cerr << installed.error() << '\n';
      return 1;
    }
    return 0;
  }
  if (mode == "load") {
    auto project = kalandra::loadNativeProject(argv[2]);
    if (!project) {
      std::cerr << project.error() << '\n';
      return 1;
    }
    auto graph = kalandra::loadInstalledNativeProject(*project, limits);
    if (!graph) {
      std::cerr << graph.error() << '\n';
      return 1;
    }
    return 0;
  }
  return 2;
}
