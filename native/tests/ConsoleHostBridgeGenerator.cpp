#include "../gpu-source/ConsoleHostBridge.h"
#include "../lib/NativeLayout.h"
#include "kalandra/Semantics.h"
#include "llvm/Support/raw_ostream.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("expected output directory and variant");
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto source = root / "main.k";
    {
      std::ofstream file(source);
      file << "from \"std/io\" import println, readLine\nfn main()->unit:\n  return ()\n";
    }
    auto syntax = loadModules(source.string());
    if (!syntax || !syntax->ok())
      throw std::runtime_error("console bridge source did not parse");
    auto checked = check(std::move(*syntax));
    if (!checked.ok())
      throw std::runtime_error("console bridge source did not check");
    llvm::LLVMContext context;
    llvm::Module module("console-host", context);
    module.setDataLayout(nativeCpuDataLayout);
    module.setTargetTriple(llvm::Triple(KALANDRA_NATIVE_CPU_TARGET_TRIPLE));
    unsigned count = 0;
    for (const auto &expression : checked.module->expressions)
      if (const auto *op = std::get_if<ConsoleOperation>(&expression.operation)) {
        if (op->action != ConsoleAction::Println && op->action != ConsoleAction::ReadLine)
          continue;
        auto *fn = emitConsoleHostBridge(*checked.module, module, *op, expression.type,
                                         op->action == ConsoleAction::Println ? "test_write"
                                                                              : "test_read");
        const std::string variant = argv[2];
        if (variant == "sanitized")
          fn->addFnAttr(llvm::Attribute::SanitizeAddress);
        if (variant == "thread-sanitized")
          fn->addFnAttr(llvm::Attribute::SanitizeThread);
        ++count;
      }
    if (count != 2 || llvm::verifyModule(module, &llvm::errs()))
      throw std::runtime_error("console bridge module invalid");
    std::error_code error;
    llvm::raw_fd_ostream file((root / "bridge.ll").string(), error);
    if (error)
      throw std::runtime_error(error.message());
    module.print(file, nullptr);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
