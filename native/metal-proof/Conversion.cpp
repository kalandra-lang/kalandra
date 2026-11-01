#include "kalandra/GpuConversion.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
using namespace mlir;
using namespace kalandra;
namespace {
bool save(ModuleOp module, const char *path) {
  if (failed(verify(module)))
    return false;
  std::error_code error;
  llvm::raw_fd_ostream out(path, error, llvm::sys::fs::OF_Text);
  if (error)
    return false;
  module.print(out);
  out << '\n';
  return true;
}
SmallVector<Value> convert(OpBuilder &b, Location loc, Value input, Value mode, NumericType source,
                           ArithmeticMode arithmetic = ArithmeticMode::Software,
                           bool nativeFloat64 = false, unsigned n = 0) {
  if (n + 1 == numericTypes.size()) {
    auto [bits, valid] = gpuNumericConversion(b, loc, input, source, numericTypes[n].type,
                                              arithmetic, nativeFloat64);
    return {bits, valid};
  }
  auto which = arith::ConstantIntOp::create(b, loc, n, 32);
  auto matches = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, mode, which);
  auto branch = scf::IfOp::create(b, loc, TypeRange{b.getI64Type(), b.getI1Type()}, matches, true);
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    auto [bits, valid] = gpuNumericConversion(b, loc, input, source, numericTypes[n].type,
                                              arithmetic, nativeFloat64);
    scf::YieldOp::create(b, loc, ValueRange{bits, valid});
  }
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getElseRegion().front());
    scf::YieldOp::create(b, loc,
                         convert(b, loc, input, mode, source, arithmetic, nativeFloat64, n + 1));
  }
  return {branch.getResult(0), branch.getResult(1)};
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 4 && argc != 6)
    return 2;
  const bool vulkan = argc == 6;
  const auto arithmetic =
      vulkan ? parseArithmeticMode(argv[4]) : std::optional{ArithmeticMode::Software};
  if (!arithmetic || (vulkan && std::string_view(argv[5]) != "--vulkan"))
    return 2;
  auto source = resolveNumeric(argv[1]);
  if (!source)
    return 2;
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  context.loadDialect<arith::ArithDialect, func::FuncDialect, scf::SCFDialect>();
  OpBuilder b(&context);
  auto loc = b.getUnknownLoc();
  auto cpu = ModuleOp::create(loc);
  for (bool validity : {false, true}) {
    b.setInsertionPointToEnd(cpu.getBody());
    auto fn = func::FuncOp::create(
        b, loc, std::string(validity ? "conversion_valid_" : "converted_") + argv[1],
        b.getFunctionType({b.getI64Type(), b.getI32Type()},
                          {validity ? Type(b.getI32Type()) : Type(b.getI64Type())}));
    auto block = fn.addEntryBlock();
    b.setInsertionPointToStart(block);
    auto result = convert(b, loc, block->getArgument(0), block->getArgument(1), *source);
    Value value =
        validity ? Value(arith::ExtUIOp::create(b, loc, b.getI32Type(), result[1])) : result[0];
    func::ReturnOp::create(b, loc, value);
  }
  const std::string gpuTemplate = R"(module attributes {gpu.container_module,
    spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
    gpu.module @kernels {
      gpu.func @checked_conversion(%in:memref<4096xi64>,%out:memref<4096xi64>,%validity:memref<4096xi32>,%params:memref<2xi32>)
        kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64,1,1]>} {
        %block=gpu.block_id x
        %thread=gpu.thread_id x
        %width=gpu.block_dim x
        %base=arith.muli %block,%width:index
        %i=arith.addi %base,%thread:index
        %zero=arith.constant 0:index
        %one=arith.constant 1:index
        %count=memref.load %params[%zero]:memref<2xi32>
        %mode=memref.load %params[%one]:memref<2xi32>
        %n=arith.index_cast %count:i32 to index
        %active=arith.cmpi ult,%i,%n:index
        scf.if %active {
          %input=memref.load %in[%i]:memref<4096xi64>
          %bits=arith.constant 0:i64 loc("conversion.bits")
          %valid=arith.constant false loc("conversion.valid")
          %word=arith.extui %valid:i1 to i32
          memref.store %bits,%out[%i]:memref<4096xi64>
          memref.store %word,%validity[%i]:memref<4096xi32>
        }
        gpu.return
      }
    }
  })";
  auto gpuText = gpuTemplate;
  if (vulkan) {
    const std::string original = "%out:memref<4096xi64>,%validity:memref<4096xi32>";
    const auto at = gpuText.find(original);
    if (at == std::string::npos)
      return 2;
    gpuText.replace(at, original.size(), "%validity:memref<4096xi32>,%out:memref<4096xi64>");
  }
  auto gpu = parseSourceString<ModuleOp>(gpuText, &context);
  if (!gpu)
    return 1;
  arith::ConstantOp bits, valid;
  Value input, mode;
  gpu->walk([&](Operation *op) {
    if (auto name = dyn_cast<NameLoc>(op->getLoc())) {
      if (name.getName() == "conversion.bits")
        bits = cast<arith::ConstantOp>(op);
      if (name.getName() == "conversion.valid")
        valid = cast<arith::ConstantOp>(op);
    }
    if (auto load = dyn_cast<memref::LoadOp>(op)) {
      if (load.getMemRefType().getElementType().isInteger(64))
        input = load.getResult();
      if (load.getMemRefType().getShape()[0] == 2)
        mode = load.getResult();
    }
  });
  b.setInsertionPoint(bits);
  auto result = convert(b, loc, input, mode, *source, *arithmetic, vulkan);
  bits.getResult().replaceAllUsesWith(result[0]);
  valid.getResult().replaceAllUsesWith(result[1]);
  bits.erase();
  valid.erase();
  return save(cpu, argv[3]) && save(*gpu, argv[2]) ? 0 : 1;
}
