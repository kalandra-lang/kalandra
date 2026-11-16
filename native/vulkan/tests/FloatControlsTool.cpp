#include "SpirvInterface.h"
#include <fstream>
#include <iostream>
#include <string_view>
int main(int argc, char **argv) {
  if (argc < 3 || argc > 4 || (argc == 4 && std::string_view(argv[3]) != "--denorm"))
    return 2;
  auto loaded = kalandra::runtime::vulkan::readSpirvFile(argv[1]);
  if (!loaded)
    return 2;
  const auto &words = *loaded;
  auto controlled = kalandra::runtime::vulkan::preserveNativeFloat64(words, argc == 4);
  if (!controlled) {
    std::cerr << controlled.error() << '\n';
    return 1;
  }
  std::ofstream output(argv[2], std::ios::binary);
  output.write(reinterpret_cast<const char *>(controlled->data()), controlled->size() * 4);
  return output ? 0 : 1;
}
