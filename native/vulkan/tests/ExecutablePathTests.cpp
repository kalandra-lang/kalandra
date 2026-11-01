#include "ExecutablePath.h"
#include <cstdio>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto directory = kalandra::runtime::vulkan::executableDirectoryUtf8();
  if (!directory || *directory != argv[1]) {
    std::fprintf(stderr, "wrong executable directory: %s\n", directory ? directory->c_str() : "unavailable");
    return 1;
  }
}
