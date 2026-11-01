#include "SpirvInterface.h"
#include <fstream>
#include <iostream>
#include <string_view>
int main(int argc, char **argv) {
  if (argc < 3 || argc > 4 || (argc == 4 && std::string_view(argv[3]) != "--denorm"))
    return 2;
  std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
  const std::streamoff length = input.tellg();
  if (length < 20 || length > 1024 * 1024 || length % 4)
    return 2;
  std::vector<std::uint32_t> words(static_cast<std::size_t>(length) / 4);
  input.seekg(0);
  input.read(reinterpret_cast<char *>(words.data()), length);
  if (!input)
    return 2;
  auto controlled = kalandra::runtime::vulkan::preserveNativeFloat64(words, argc == 4);
  if (!controlled) {
    std::cerr << controlled.error() << '\n';
    return 1;
  }
  std::ofstream output(argv[2], std::ios::binary);
  output.write(reinterpret_cast<const char *>(controlled->data()), controlled->size() * 4);
  return output ? 0 : 1;
}
