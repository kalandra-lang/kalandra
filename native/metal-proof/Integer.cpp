#include "kalandra/GpuInteger.h"
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
#include <system_error>
using namespace mlir;
using namespace kalandra;
namespace {
constexpr const char *names[] = {"add", "sub", "mul", "div", "rem", "neg"};
constexpr GpuIntegerOperation operations[] = {
    GpuIntegerOperation::Add,    GpuIntegerOperation::Subtract,  GpuIntegerOperation::Multiply,
    GpuIntegerOperation::Divide, GpuIntegerOperation::Remainder, GpuIntegerOperation::Negate};
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
} // namespace
int main(int argc, char **argv) {
  if (argc != 4) {
    llvm::errs() << "usage: kalandra-metal-integer <numeric type> <gpu.mlir> <cpu.mlir>\n";
    return 2;
  }
  auto type = resolveNumeric(argv[1]);
  if (!type || !isInteger(*type))
    return 2;
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  context.loadDialect<arith::ArithDialect, func::FuncDialect, scf::SCFDialect>();
  OpBuilder b(&context);
  auto loc = b.getUnknownLoc();
  auto cpu = ModuleOp::create(loc);
  for (unsigned i = 0; i < 6; ++i)
    for (bool validity : {false, true}) {
      b.setInsertionPointToEnd(cpu.getBody());
      auto name = std::string(validity ? "valid_" : "checked_") + argv[1] + "_" + names[i];
      auto fn = func::FuncOp::create(
          b, loc, name,
          b.getFunctionType({b.getI64Type(), b.getI64Type()},
                            {validity ? Type(b.getI32Type()) : Type(b.getI64Type())}));
      auto *entry = fn.addEntryBlock();
      b.setInsertionPointToStart(entry);
      auto result = emitCheckedGpuInteger(b, loc, *type, operations[i], entry->getArgument(0),
                                          entry->getArgument(1));
      if (!result)
        return 1;
      Value value = validity ? Value(arith::ExtUIOp::create(b, loc, b.getI32Type(), result->valid))
                             : result->bits;
      func::ReturnOp::create(b, loc, value);
    }
  auto gpu = parseSourceString<ModuleOp>(R"(module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @checked_integer(%a:memref<4096xi64>,%b:memref<4096xi64>,
                             %out:memref<4096xi64>,%validity:memref<4096xi32>,%params:memref<2xi32>)
      kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %i = arith.addi %base, %thread : index
      %zero = arith.constant 0 : index
      %one = arith.constant 1 : index
      %count = memref.load %params[%zero] : memref<2xi32>
      %mode = memref.load %params[%one] : memref<2xi32>
      %n = arith.index_cast %count : i32 to index
      %active = arith.cmpi ult, %i, %n : index
      scf.if %active {
        %x = memref.load %a[%i] : memref<4096xi64>
        %y = memref.load %b[%i] : memref<4096xi64>
        %placeholder = arith.constant 0 : i64
        %ok = arith.constant true
        %flag = arith.extui %ok : i1 to i32
        memref.store %placeholder, %out[%i] : memref<4096xi64>
        memref.store %flag, %validity[%i] : memref<4096xi32>
      }
      gpu.return
    }
  }
})",
                                         &context);
  if (!gpu)
    return 1;
  SmallVector<Value> operands;
  Value mode;
  gpu->walk([&](memref::LoadOp op) {
    if (op.getResult().getType().isInteger(64))
      operands.push_back(op.getResult());
    else
      mode = op.getResult();
  });
  arith::ConstantOp bits, valid;
  gpu->walk([&](arith::ConstantOp op) {
    if (op.getType().isInteger(64))
      bits = op;
    if (op.getType().isInteger(1))
      valid = op;
  });
  if (operands.size() != 2 || !mode || !bits || !valid)
    return 1;
  b.setInsertionPoint(bits);
  std::function<GpuIntegerResult(OpBuilder &, unsigned)> choose;
  choose = [&](OpBuilder &builder, unsigned i) -> GpuIntegerResult {
    if (i == 5)
      return *emitCheckedGpuInteger(builder, loc, *type, operations[i], operands[0], operands[1]);
    auto index = arith::ConstantIntOp::create(builder, loc, i, 32);
    auto condition = arith::CmpIOp::create(builder, loc, arith::CmpIPredicate::eq, mode, index);
    auto branch = scf::IfOp::create(
        builder, loc, TypeRange{builder.getI64Type(), builder.getI1Type()}, condition, true);
    {
      auto arm = OpBuilder::atBlockBegin(&branch.getThenRegion().front());
      auto result =
          *emitCheckedGpuInteger(arm, loc, *type, operations[i], operands[0], operands[1]);
      scf::YieldOp::create(arm, loc, ValueRange{result.bits, result.valid});
    }
    {
      auto arm = OpBuilder::atBlockBegin(&branch.getElseRegion().front());
      auto result = choose(arm, i + 1);
      scf::YieldOp::create(arm, loc, ValueRange{result.bits, result.valid});
    }
    return {branch.getResult(0), branch.getResult(1)};
  };
  auto result = choose(b, 0);
  bits.getResult().replaceAllUsesWith(result.bits);
  valid.getResult().replaceAllUsesWith(result.valid);
  bits.erase();
  valid.erase();
  return save(*gpu, argv[2]) && save(cpu, argv[3]) ? 0 : 1;
}
