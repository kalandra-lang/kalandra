#include "Supervisor.h"
#include "kalandra/Diagnostics.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace kalandra;
// Private source-supervisor qualification tool, not an installed CLI target.
int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: kalandra-vulkan-supervisor source.k directory arithmetic-mode\n";
    return 2;
  }
  auto arithmetic = parseArithmeticMode(argv[3]);
  if (!arithmetic) {
    std::cerr << "unknown arithmetic policy\n";
    return 2;
  }
  auto loaded = loadModules(argv[1]);
  if (!loaded) {
    std::cerr << loaded.error() << '\n';
    return 1;
  }
  auto source = loaded->module.source;
  if (!loaded->ok()) {
    for (auto &diagnostic : loaded->diagnostics)
      std::cerr << renderDiagnostic(*source, diagnostic);
    return 1;
  }
  try {
    auto output =
        emitGpuSupervisor(std::move(loaded->module), *arithmetic, false, GpuBackend::Vulkan);
    if (!output) {
      std::cerr << renderDiagnostic(*source, output.error());
      return 1;
    }
    const std::filesystem::path directory(argv[2]);
    std::string artifacts;
    for (const auto &[name, text] : *output) {
      (void)text;
      artifacts += name + "\n";
    }
    (*output)["artifacts.txt"] = std::move(artifacts);
    std::filesystem::create_directories(directory);
    for (auto &[name, text] : *output) {
      auto target = directory / name;
      if (std::filesystem::is_symlink(target) ||
          std::filesystem::weakly_canonical(target) == std::filesystem::canonical(argv[1]))
        throw std::runtime_error("refusing source/symlink output");
      std::ofstream stream(target, std::ios::binary);
      stream << text;
      stream.close();
      if (!stream)
        throw std::runtime_error("cannot write supervisor artifact");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
