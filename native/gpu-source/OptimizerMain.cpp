// Private command frontend for the pinned MLIR libraries. The Windows SDK
// supplies these libraries but does not install the upstream command binaries.
#include "mlir/Pass/Pass.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"
int main(int argc, char **argv) {
  mlir::registerAllPasses();
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);
  mlir::registerAllExtensions(registry);
  return mlir::asMainReturnCode(mlir::MlirOptMain(argc, argv, "Kalandra private MLIR optimizer", registry));
}
