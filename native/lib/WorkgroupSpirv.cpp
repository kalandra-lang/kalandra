#include "kalandra/WorkgroupSpirv.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SPIRV/IR/SPIRVOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/SmallVector.h"

namespace kalandra {
mlir::LogicalResult lowerWorkgroupSpirv(mlir::ModuleOp module) {
  using namespace mlir;
  bool valid = true;
  module.walk([&](gpu::GPUFuncOp function) {
    const auto count = function.getNumWorkgroupAttributions();
    if (!count || !valid)
      return;
    auto parent = function->getParentOfType<gpu::GPUModuleOp>();
    if (!parent) {
      function.emitError("cooperative storage requires a GPU module");
      valid = false;
      return;
    }
    // Validate all uses before mutation. In particular, a cast or an escaping
    // memref must never be silently reinterpreted as thread-private storage.
    for (auto argument : function.getWorkgroupAttributionBBArgs()) {
      const auto type = dyn_cast<MemRefType>(argument.getType());
      if (!type || type.getRank() != 1 || !type.hasStaticShape() ||
          !type.getLayout().isIdentity() || type.getDimSize(0) <= 0 ||
          type.getDimSize(0) > UINT32_MAX ||
          (!type.getElementType().isInteger(32) && !type.getElementType().isInteger(64))) {
        function.emitError("unsupported cooperative scratch carrier layout");
        valid = false;
        return;
      }
      for (auto &use : argument.getUses()) {
        const auto *owner = use.getOwner();
        if ((!isa<memref::LoadOp>(owner) || use.getOperandNumber() != 0) &&
            (!isa<memref::StoreOp>(owner) || use.getOperandNumber() != 1)) {
          function.emitError("cooperative storage may only be loaded and stored");
          valid = false;
          return;
        }
      }
    }
    OpBuilder builder(function.getContext());
    for (unsigned slot = 0; slot < count; ++slot) {
      const auto argument = function.getWorkgroupAttributionBBArgs()[slot];
      const auto type = cast<MemRefType>(argument.getType());
      const auto array = spirv::ArrayType::get(type.getElementType(), type.getDimSize(0));
      const auto pointer = spirv::PointerType::get(array, spirv::StorageClass::Workgroup);
      const auto cell =
          spirv::PointerType::get(type.getElementType(), spirv::StorageClass::Workgroup);
      const auto name =
          "__kalandra_workgroup_" + function.getName().str() + "_" + std::to_string(slot);
      if (SymbolTable::lookupSymbolIn(parent, name)) {
        function.emitError("duplicate cooperative storage identity");
        valid = false;
        return;
      }
      builder.setInsertionPoint(function);
      spirv::GlobalVariableOp::create(builder, function.getLoc(), pointer, name);
      builder.setInsertionPointToStart(&function.getBody().front());
      auto base = spirv::AddressOfOp::create(builder, function.getLoc(), pointer, name);
      llvm::SmallVector<Operation *> users(argument.getUsers());
      for (auto *user : users) {
        builder.setInsertionPoint(user);
        auto load = dyn_cast<memref::LoadOp>(user);
        auto store = dyn_cast<memref::StoreOp>(user);
        const auto indices = load ? load.getIndices() : store.getIndices();
        auto index =
            arith::IndexCastUIOp::create(builder, user->getLoc(), builder.getI32Type(), indices[0]);
        auto address =
            spirv::AccessChainOp::create(builder, user->getLoc(), cell, base, ValueRange{index});
        if (load) {
          auto replacement = spirv::LoadOp::create(builder, user->getLoc(), address);
          load.getResult().replaceAllUsesWith(replacement.getValue());
        } else
          spirv::StoreOp::create(builder, user->getLoc(), address, store.getValue());
        user->erase();
      }
    }
    // Workgroup attributions follow the ordinary descriptor arguments.
    function.getBody().front().eraseArguments(function.getFirstWorkgroupAttributionIndex(), count);
    function.setWorkgroupAttributionsAttr(builder.getI64IntegerAttr(0));
  });
  return success(valid);
}
} // namespace kalandra
