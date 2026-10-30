#include "kalandra/StrictFloat.h"
#include "mlir/AsmParser/AsmParser.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <string_view>
int main(int argc, char **argv) {
  if (argc != 3 && !(argc == 4 && (std::string_view(argv[3]) == "--hardware" ||
                                   std::string_view(argv[3]) == "--allow-software-fallback"))) {
    llvm::errs() << "usage: " << argv[0] << " <input.mlir> <output.mlir>\n";
    return 2;
  }
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(argv[1], &context);
  if (!module || mlir::failed(mlir::verify(*module)))
    return 1;
  if (argc == 4) {
    if (mlir::failed(kalandra::lowerMetalHardwareFloat(*module, std::string_view(argv[3]) ==
                                                                    "--allow-software-fallback")))
      return 1;
  } else
#if defined(KALANDRA_STRICT_GPU64)
      if (mlir::failed(kalandra::lowerStrictGpuFloat64(*module)))
#elif defined(KALANDRA_STRICT_CPU)
      if (mlir::failed(kalandra::lowerStrictCpuFloat(*module)))
#else
      if (mlir::failed(kalandra::lowerStrictFloat32(*module)))
#endif
    return 1;
  bool hasGpu = false;
  module->walk([&](mlir::gpu::GPUModuleOp) { hasGpu = true; });
  if (hasGpu)
    (*module)->setAttr(
        "spirv.target_env",
        mlir::parseAttribute("#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], "
                             "[SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>",
                             &context));
  if (mlir::failed(mlir::verify(*module)))
    return 1;
  std::error_code error;
  llvm::raw_fd_ostream output(argv[2], error, llvm::sys::fs::OF_Text);
  if (error) {
    llvm::errs() << error.message() << '\n';
    return 1;
  }
  module->print(output);
  output << '\n';
  return 0;
}
