#include "kalandra/PackageLock.h"
#include <fstream>
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  kalandra::PackageLockLimits limits{std::stoull(argv[4]), std::stoull(argv[2]),
                                     std::stoull(argv[3])};
  std::ifstream input(argv[1], std::ios::binary);
  if (!input)
    return 2;
  const std::string contents{std::istreambuf_iterator<char>(input), {}};
  auto result = kalandra::parseNativePackageLock(contents, limits);
  if (!result) {
    std::cerr << result.error() << '\n';
    return 1;
  }
  std::cout << kalandra::serializeNativePackageLock(*result);
}
