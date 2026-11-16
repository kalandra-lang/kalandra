#include "SpirvInterface.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime::vulkan;
static void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void checkSpirvFiles() {
  try {
    const auto root = std::filesystem::temp_directory_path() /
                      ("kalandra-spirv-files-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(root), "cannot create private fixture directory");
    struct RootCleanup {
      std::filesystem::path root;
      ~RootCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
      }
    } rootCleanup{root};
    const auto path = root / std::filesystem::path(u8"module λ.spv");
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    } cleanup{path};
    std::filesystem::remove(path);
    auto missing = readSpirvFile(path);
    check(!missing && missing.error() == SpirvFileError::Unavailable, "missing file error changed");
    const auto write = [&](const std::vector<std::uint32_t> &words) {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char *>(words.data()), words.size() * 4);
      output.close();
      check(bool(output), "fixture write failed");
    };
    for (auto bytes : {std::uint64_t(20), std::uint64_t(1024 * 1024 + 512), MaxSpirvModuleBytes}) {
      std::vector<std::uint32_t> words(bytes / 4, 0x00010000); // OpNop padding.
      words[0] = 0x07230203;
      words[1] = 0x00010000;
      words[2] = 0;
      words[3] = 100;
      words[4] = 0;
      write(words);
      auto loaded = readSpirvFile(path);
      check(loaded && *loaded == words, "bounded module did not round-trip exactly");
    }
    // Sparse oversized files are rejected before allocating their contents.
    std::filesystem::resize_file(path, MaxSpirvModuleBytes + 4);
    auto tooLarge = readSpirvFile(path);
    check(!tooLarge && tooLarge.error() == SpirvFileError::Invalid, "oversized file accepted");
    for (auto bytes : {0u, 4u, 19u, 21u}) {
      write({0x07230203, 0x00010000, 0, 100, 0});
      std::filesystem::resize_file(path, bytes);
      auto malformed = readSpirvFile(path);
      check(!malformed && malformed.error() == SpirvFileError::Invalid,
            "truncated/unaligned file accepted");
    }
    write({0, 0x00010000, 0, 100, 0});
    auto magic = readSpirvFile(path);
    check(!magic && magic.error() == SpirvFileError::Invalid, "bad magic accepted");
    std::cout << "PASS: bounded SPIR-V file loading, large control-flow modules, Unicode paths and "
                 "malformed files\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    std::abort();
  }
}
