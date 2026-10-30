#include "kalandra/Version.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

#include <expected>
#include <span>
#include <string>

namespace {
std::expected<mlir::OwningOpRef<mlir::ModuleOp>, std::string> readModule(mlir::MLIRContext &context,
                                                                         const char *path) {
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(path, &context);
  if (!module)
    return std::unexpected("cannot parse MLIR input");
  if (mlir::failed(mlir::verify(*module)))
    return std::unexpected("MLIR verification failed");
  return module;
}
} // namespace

int main(int argc, char **argv) {
  if (kalandra::printVersion(argc, argv, "kalandra-mlir-check"))
    return 0;
  const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
  if (arguments.size() != 2) {
    llvm::errs() << "usage: kalandra-mlir-check <file.mlir>\n";
    return 2;
  }
  mlir::DialectRegistry dialects;
  dialects.insert<mlir::arith::ArithDialect, mlir::bufferization::BufferizationDialect,
                  mlir::func::FuncDialect, mlir::tensor::TensorDialect>();
  mlir::MLIRContext context(dialects);
  auto module = readModule(context, arguments[1]);
  if (!module) {
    llvm::errs() << module.error() << '\n';
    return 1;
  }
  llvm::outs() << "MLIR module verified through native C++23 API\n";
  return 0;
}
