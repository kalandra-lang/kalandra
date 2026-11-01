#include "LowerConversions.h"
#include "kalandra/StrictFloat.h"
#include "mlir/AsmParser/AsmParser.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <string_view>
// Vulkan keeps supported float32/float64 arithmetic native. Unsupported physical
// features are rejected by artifact admission; software mode is always explicit.
int main(int argc, char **argv) {
  if (argc != 4) {
    llvm::errs() << "usage: kalandra-vulkan-lower input.mlir output.mlir "
                    "hardware|software|hardware-with-software-fallback\n";
    return 2;
  }
  auto policy = kalandra::parseArithmeticMode(argv[3]);
  if (!policy)
    return 2;
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(argv[1], &context);
  if (!module || mlir::failed(mlir::verify(*module)))
    return 1;
  if (*policy != kalandra::ArithmeticMode::Hardware &&
      mlir::failed(lowerVulkanFloat64Conversions(*module)))
    return 1;
  if (*policy != kalandra::ArithmeticMode::Hardware &&
      mlir::failed(kalandra::lowerStrictGpuFloat64(
          *module, *policy == kalandra::ArithmeticMode::HardwareWithSoftwareFallback)))
    return 1;
  // The shared emitter has a linear work domain. Fold a physical 2D dispatch
  // into that domain rather than imposing Vulkan's per-axis group limit on
  // tensor sizes. The entry suffix versions this private compiler/runtime ABI.
  module->walk([&](mlir::gpu::GPUFuncOp function) {
    if (function.getName().ends_with("_vk_flat_v1"))
      return;
    llvm::SmallVector<mlir::gpu::BlockIdOp> indices;
    function.walk([&](mlir::gpu::BlockIdOp index) {
      if (index.getDimension() == mlir::gpu::Dimension::x)
        indices.push_back(index);
    });
    for (auto index : indices) {
      mlir::OpBuilder builder(index);
      auto loc = index.getLoc();
      auto x = mlir::gpu::BlockIdOp::create(builder, loc, mlir::gpu::Dimension::x);
      auto y = mlir::gpu::BlockIdOp::create(builder, loc, mlir::gpu::Dimension::y);
      auto width = mlir::gpu::GridDimOp::create(builder, loc, mlir::gpu::Dimension::x);
      auto row = mlir::arith::MulIOp::create(builder, loc, y, width);
      auto flat = mlir::arith::AddIOp::create(builder, loc, x, row);
      index.replaceAllUsesWith(flat.getResult());
      index.erase();
    }
    function.setName((function.getName() + "_vk_flat_v1").str());
  });
  bool gpu = false;
  module->walk([&](mlir::gpu::GPUModuleOp) { gpu = true; });
  if (gpu) {
    auto environment = *policy != kalandra::ArithmeticMode::Hardware
                           ? "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], "
                             "[SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>"
                           : "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64, Float64], "
                             "[SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>";
    (*module)->setAttr("spirv.target_env", mlir::parseAttribute(environment, &context));
  }
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
