#include "BuiltinShaders.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

using namespace kalandra::runtime::vulkan;
int main(int argc, char **argv) {
  if (argc != 2 || builtinShaderNames().size() != 14 ||
      !builtinShader("../add.spv").empty() || !builtinShader("").empty())
    return 1;
  for (auto name : builtinShaderNames()) {
    auto words = builtinShader(name);
    if (words.size() < 5 || words[0] != 0x07230203 || !words[3] || words[4])
      return 1;
    auto output = std::filesystem::path(argv[1]) / name;
    std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output, std::ios::binary);
    for (auto word : words)
      for (unsigned byte = 0; byte < 4; ++byte)
        stream.put(static_cast<char>((word >> (8 * byte)) & 255));
    stream.close();
    if (!stream)
      return 1;
  }
  std::puts("PASS: embedded resource lookup and little-endian extraction");
}
