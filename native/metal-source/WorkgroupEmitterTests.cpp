#include "NumericShader.h"
#include "kalandra/Syntax.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char **argv) {
  if (argc != 4 && argc != 5)
    return 2;
  const bool metal = argc == 4 || std::string_view(argv[4]) == "metal";
  auto syntax = kalandra::loadModules(argv[1]);
  if (!syntax) {
    std::cerr << syntax.error();
    return 1;
  }
  auto checked = kalandra::check(std::move(*syntax));
  if (!checked.ok()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  for (std::size_t id = 0; id < checked.module->functions.size(); ++id)
    if ((checked.module->functions[id].name == argv[2] ||
         checked.module->functions[id].name.ends_with("::" + std::string(argv[2])))) {
      auto shader = kalandra::emitWorkgroupShader(
          *checked.module, id,
          {.arithmetic = metal ? kalandra::ArithmeticMode::HardwareWithSoftwareFallback
                               : kalandra::ArithmeticMode::Hardware,
           .nativeFloat64Conversions = !metal});
      if (!shader) {
        std::cerr << shader.error().message << '\n';
        return 1;
      }
      std::ofstream(argv[3]) << shader->mlir;
      std::cout << "Emitted cooperative kernel: " << shader->workgroup->groupShape.size()
                << " dimensions, " << shader->workgroup->scratchBytes << " shared bytes\n";
      return 0;
    }
  for (const auto &function : checked.module->functions)
    std::cerr << function.name << "\n";
  return 3;
}
