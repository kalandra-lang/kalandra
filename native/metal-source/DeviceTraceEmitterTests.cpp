#include "DeviceHeap.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
int main(int argc, char **) {
  static_assert(DeviceHeapEmitter::Cells == 24, "update isolated trace execution ABI");
  try {
    mlir::DialectRegistry registry;
    mlir::registerAllDialects(registry);
    mlir::MLIRContext context(registry);
    context.loadAllAvailableDialects();
    const bool gpu = argc == 2;
    const std::string source =
        gpu ? "module attributes "
              "{gpu.container_module,spirv.target_env=#spirv.target_env<#spirv.vce<v1.0,[Shader,"
              "Int64],[SPV_KHR_storage_buffer_storage_class]>,#spirv.resource_limits<>>} { "
              "gpu.module @kernels { gpu.func "
              "@kalandra_numeric_v1_uint64_d_t(%heap:memref<?xi64>,%out:memref<?xi64>,%frame:"
              "memref<52xi32>) kernel attributes "
              "{spirv.entry_point_abi=#spirv.entry_point_abi<workgroup_size=[64,1,1]>} { "
              "%thread=gpu.thread_id x %zero=arith.constant 0:index %active=arith.cmpi "
              "eq,%thread,%zero:index scf.if %active {} gpu.return } } }"
            : "module { func.func @step(%heap:memref<?xi64>,%frame:memref<52xi32>) attributes "
              "{llvm.emit_c_interface} { return } }";
    auto module = mlir::parseSourceString<mlir::ModuleOp>(source, &context);
    if (!module)
      throw std::runtime_error("trace test skeleton failed");
    mlir::Block *body = nullptr;
    mlir::Value arena, frame, output;
    if (gpu) {
      module->walk([&](mlir::gpu::GPUFuncOp function) {
        arena = function.getArgument(0);
        output = function.getArgument(1);
        frame = function.getArgument(2);
      });
      module->walk([&](mlir::scf::IfOp condition) { body = &condition.getThenRegion().front(); });
    } else {
      auto function = *module->getOps<mlir::func::FuncOp>().begin();
      arena = function.getArgument(0);
      frame = function.getArgument(1);
      body = &function.front();
    }
    auto builder = mlir::OpBuilder::atBlockTerminator(body);
    std::vector<DeviceRootMap> roots{{1, {DeviceHeapEmitter::Cells}}};
    // Repeated records: tag, first child, second child. Variant 1 owns the
    // second field; variant 2 owns both. Inactive bits must not be traversed.
    std::vector<DeviceTraceLayout> layouts{{12, 3, {{1, 0, 1}, {1, 0, 2}, {2, 0, 2}}},
                                           {13, 1, {{0}}}};
    for (auto invalid : std::vector<DeviceTraceLayout>{
             {11, 1, {{0}}}, {12, 0, {{0}}}, {12, 1, {{1}}}, {12, 1, {{0, 1, 1}}}}) {
      bool rejected = false;
      try {
        DeviceHeapEmitter heap(builder, arena, frame, 52, roots, std::span(&invalid, 1));
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      if (!rejected)
        throw std::runtime_error("malformed traversal descriptor accepted");
    }
    DeviceHeapEmitter heap(builder, arena, frame, 52, roots, layouts);
    heap.service();
    if (gpu) {
      auto zero = mlir::arith::ConstantIntOp::create(builder, builder.getUnknownLoc(), 0, 64);
      auto index = mlir::arith::ConstantIndexOp::create(builder, builder.getUnknownLoc(), 0);
      mlir::memref::StoreOp::create(builder, builder.getUnknownLoc(), zero, output,
                                    mlir::ValueRange{index});
    }
    if (mlir::failed(mlir::verify(*module)))
      throw std::runtime_error("trace emitter verification failed");
    module->print(llvm::outs());
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
