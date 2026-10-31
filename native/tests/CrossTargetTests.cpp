#include "kalandra/Backend.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 5) return 2;
  auto source = llvm::MemoryBuffer::getFile(argv[1]);
  auto layout = llvm::MemoryBuffer::getFile(argv[3]);
  if (!source || !layout) return 2;
  auto typed = kalandra::check(kalandra::parse(std::make_shared<kalandra::SourceFile>(
      argv[1], (*source)->getBuffer().str())));
  if (!typed.ok()) {
    for (const auto &d : typed.diagnostics)
      std::cerr << kalandra::renderDiagnostic(*typed.source, d);
    return 1;
  }
  auto lowered = kalandra::lowerNative(*typed.module, {}, {}, kalandra::ArithmeticMode::Hardware,
      false, {argv[2], (*layout)->getBuffer().trim().str()});
  if (!lowered) { std::cerr << lowered.error() << '\n'; return 1; }
  std::error_code error;
  llvm::raw_fd_ostream output(argv[4], error);
  if (error) return 1;
  output << lowered->llvm;
  return 0;
}
