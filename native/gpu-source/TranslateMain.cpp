// Private command frontend using the pinned MLIR translation registrations.
#include "mlir/InitAllTranslations.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Tools/mlir-translate/MlirTranslateMain.h"
int main(int argc, char **argv) {
  mlir::registerAllTranslations();
  return mlir::failed(mlir::mlirTranslateMain(argc, argv, "Kalandra private MLIR translator"));
}
