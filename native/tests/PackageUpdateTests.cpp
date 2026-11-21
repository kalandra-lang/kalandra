#include "kalandra/PackageInstall.h"
#include <iostream>
int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  kalandra::PackageInstallOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string name = argv[i];
    if (name == "--frozen")
      options.frozen = true;
    else if (name == "--offline")
      options.offline = true;
    else
      options.updateRoots.push_back(name);
  }
  auto result = argc == 4 && std::string_view(argv[2]) == "--remove"
                    ? kalandra::removeNativeDependency(argv[1], argv[3])
                    : kalandra::installNativeProject(argv[1], {}, options);
  if (!result) {
    std::cerr << result.error() << '\n';
    return 1;
  }
}
