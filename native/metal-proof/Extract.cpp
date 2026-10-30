#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVDialect.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <string_view>

// GPUToSPIRV retains the original gpu.module alongside its converted clone.
// Extract via the IR API, and make separate rounding explicit before serialization.
int main(int argc, char **argv) {
  if (argc != 3 && !(argc == 4 && std::string_view(argv[3]) == "--metal-compiler-rounding")) {
    llvm::errs() << "usage: kalandra-metal-extract <converted.mlir> <kernel.mlir> "
                    "[--metal-compiler-rounding]\n";
    return 2;
  }
  mlir::DialectRegistry registry;
  registry.insert<mlir::arith::ArithDialect, mlir::gpu::GPUDialect, mlir::memref::MemRefDialect,
                  mlir::scf::SCFDialect, mlir::spirv::SPIRVDialect>();
  mlir::MLIRContext context(registry);
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(argv[1], &context);
  if (!module || mlir::failed(mlir::verify(*module)))
    return 1;
  mlir::spirv::ModuleOp selected;
  unsigned count = 0;
  module->walk([&](mlir::spirv::ModuleOp op) {
    selected = op;
    ++count;
  });
  if (count != 1) {
    llvm::errs() << "expected exactly one SPIR-V module\n";
    return 1;
  }
  selected.walk<mlir::WalkOrder::PostOrder>([&](mlir::Operation *op) {
    if (argc == 3 && mlir::isa<mlir::spirv::FMulOp, mlir::spirv::FAddOp>(op))
      op->setAttr("no_contraction", mlir::UnitAttr::get(&context));
    // LLVM 23 canonicalizes integer compare/select into min/max. MSL 2.2
    // has no signed/unsigned 64-bit min/max overloads; preserve exact integer
    // semantics using comparisons and selects after the last canonicalizer.
    if (argc != 4 ||
        !mlir::isa<mlir::spirv::GLSMaxOp, mlir::spirv::GLSMinOp, mlir::spirv::GLUMaxOp,
                   mlir::spirv::GLUMinOp>(op) ||
        !op->getResult(0).getType().isInteger(64))
      return;
    mlir::OpBuilder builder(op);
    auto lhs = op->getOperand(0), rhs = op->getOperand(1);
    mlir::Value condition;
    if (mlir::isa<mlir::spirv::GLSMaxOp>(op))
      condition =
          mlir::spirv::SGreaterThanOp::create(builder, op->getLoc(), builder.getI1Type(), lhs, rhs);
    else if (mlir::isa<mlir::spirv::GLSMinOp>(op))
      condition =
          mlir::spirv::SLessThanOp::create(builder, op->getLoc(), builder.getI1Type(), lhs, rhs);
    else if (mlir::isa<mlir::spirv::GLUMaxOp>(op))
      condition =
          mlir::spirv::UGreaterThanOp::create(builder, op->getLoc(), builder.getI1Type(), lhs, rhs);
    else
      condition =
          mlir::spirv::ULessThanOp::create(builder, op->getLoc(), builder.getI1Type(), lhs, rhs);
    auto selectedValue = mlir::spirv::SelectOp::create(
        builder, op->getLoc(), op->getResult(0).getType(), condition, lhs, rhs);
    op->getResult(0).replaceAllUsesWith(selectedValue.getResult());
    op->erase();
  });
  if (mlir::failed(mlir::verify(selected)))
    return 1;
  std::error_code error;
  llvm::raw_fd_ostream output(argv[2], error, llvm::sys::fs::OF_Text);
  if (error) {
    llvm::errs() << error.message() << '\n';
    return 1;
  }
  selected.print(output);
  output << '\n';
  return 0;
}
