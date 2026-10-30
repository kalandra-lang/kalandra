#include "../metal-source/NumericShader.h"
#include "kalandra/GpuInteger.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <sstream>
using namespace mlir;
using namespace kalandra;
int main(int argc, char **argv) {
  if (argc != 3 && argc != 4)
    return 2;
  if (argc == 4 && std::string_view(argv[3]) == "reduce") {
    auto type = resolveNumeric(argv[1]);
    if (!type)
      return 2;
    const auto ir = numericReductionShader(*type);
    if (ir.empty())
      return 1;
    std::error_code error;
    llvm::raw_fd_ostream out(argv[2], error, llvm::sys::fs::OF_Text);
    if (error)
      return 1;
    out << ir << '\n';
    return 0;
  }
  const bool scalarOnly = argc == 4;
  if (scalarOnly && std::string_view(argv[3]) != "scalar")
    return 2;
  const auto type = resolveNumeric(argv[1]);
  if (!type)
    return 2;
  const auto bits = describeNumeric(*type)->bits;
  const auto words = bits == 64 ? 4 : 3;
  const auto element = "i" + std::to_string(bits);
  const auto buffer = "memref<?x" + element + ">";
  const auto params = "memref<" + std::to_string(words) + "xi32>";
  std::ostringstream source;
  source << "module attributes {gpu.container_module, spirv.target_env = "
            "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64";
  if (bits == 8)
    source << ", Int8, StorageBuffer8BitAccess";
  if (bits == 16)
    source << ", Int16, StorageBuffer16BitAccess";
  source << "], [SPV_KHR_storage_buffer_storage_class";
  if (bits == 8)
    source << ", SPV_KHR_8bit_storage";
  if (bits == 16)
    source << ", SPV_KHR_16bit_storage";
  source
      << "]>, #spirv.resource_limits<>>} { gpu.module @kernels { gpu.func @kalandra_numeric_v1_"
      << describeNumeric(*type)->name << "(%a:" << buffer << ",%b:" << buffer << ",%out:" << buffer
      << ",%params:" << params
      << ")"
         " kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, "
         "1, 1]>} {\n"
         "%block=gpu.block_id x\n%thread=gpu.thread_id x\n%width=gpu.block_dim x\n"
         "%base=arith.muli %block,%width:index\n%i=arith.addi %base,%thread:index\n"
         "%zero=arith.constant 0:index\n%one=arith.constant 1:index\n"
         "%count=memref.load %params[%zero]:"
      << params
      << "\n%n=arith.index_cast %count:i32 to index\n"
         "%active=arith.cmpi ult,%i,%n:index\nscf.if %active {\n"
         "%x=memref.load %a[%i]:"
      << buffer << "\n%y=memref.load %b[%i]:" << buffer
      << "\n"
         "%scalarLo=memref.load %params[%one]:"
      << params << "\n%lo=arith.extui %scalarLo:i32 to i64 loc(\"numeric.scalar\")\n";
  if (bits == 64)
    source << "%two=arith.constant 2:index\n%scalarHi=memref.load %params[%two]:" << params
           << "\n"
              "%hi=arith.extui %scalarHi:i32 to i64\n%shift=arith.constant 32:i64\n"
              "%upper=arith.shli %hi,%shift:i64\n%scalar=arith.ori %lo,%upper:i64 "
              "loc(\"numeric.scalar\")\n";
  source << "%placeholder=arith.constant 0:i64 loc(\"numeric.bits\")\n"
            "%ok=arith.constant true loc(\"numeric.valid\")\n";
  if (bits == 64)
    source << "memref.store %placeholder,%out[%i]:" << buffer << "\n";
  else
    source << "%narrow=arith.trunci %placeholder:i64 to " << element
           << "\nmemref.store %narrow,%out[%i]:" << buffer << "\n";
  source << "%yes=arith.constant true\n%bad=arith.xori %ok,%yes:i1\nscf.if %bad {\n"
            "%fault=arith.constant "
         << words - 1
         << ":index\n%lane=arith.index_cast %i:index to i32\n"
            "%previous=memref.atomic_rmw minu %lane,%params[%fault]:(i32,"
         << params << ")->i32\n}\n}\ngpu.return\n}}}\n";
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  auto text = source.str();
  if (scalarOnly) {
    const auto from = "%a:" + buffer + ",%b:" + buffer + ",";
    auto pos = text.find(from);
    if (pos == std::string::npos)
      return 1;
    text.erase(pos, from.size());
    const auto loads =
        "%x=memref.load %a[%i]:" + buffer + "\n%y=memref.load %b[%i]:" + buffer + "\n";
    pos = text.find(loads);
    if (pos == std::string::npos)
      return 1;
    text.erase(pos, loads.size());
  }
  auto module = parseSourceString<ModuleOp>(text, &context);
  if (!module)
    return 1;
  arith::ConstantOp placeholder, validity;
  Value scalar;
  SmallVector<Value> operands;
  module->walk([&](Operation *op) {
    if (auto name = dyn_cast<NameLoc>(op->getLoc())) {
      if (name.getName() == "numeric.bits")
        placeholder = cast<arith::ConstantOp>(op);
      if (name.getName() == "numeric.valid")
        validity = cast<arith::ConstantOp>(op);
      if (name.getName() == "numeric.scalar")
        scalar = op->getResult(0);
    }
    if (auto load = dyn_cast<memref::LoadOp>(op); load && !load.getMemRefType().hasStaticShape())
      operands.push_back(load.getResult());
  });
  if (!placeholder || !validity || !scalar || operands.size() != (scalarOnly ? 0 : 2))
    return 1;
  OpBuilder b(placeholder);
  auto loc = b.getUnknownLoc();
  auto widen = [&](Value input) -> Value {
    return bits == 64 ? input : Value(arith::ExtUIOp::create(b, loc, b.getI64Type(), input));
  };
  const auto one = isInteger(*type) ? UINT64_C(1)
                   : bits == 32     ? UINT64_C(0x3f800000)
                                    : UINT64_C(0x3ff0000000000000);
  Value a = scalarOnly ? Value(arith::ConstantIntOp::create(b, loc, one, 64)) : widen(operands[0]);
  Value d = scalarOnly ? Value(arith::ConstantIntOp::create(b, loc, 0, 64)) : widen(operands[1]);
  Value result, valid;
  if (isInteger(*type)) {
    auto first = *emitCheckedGpuInteger(b, loc, *type, GpuIntegerOperation::Add, a, d);
    auto second =
        *emitCheckedGpuInteger(b, loc, *type, GpuIntegerOperation::Add, first.bits, scalar);
    result = second.bits;
    valid = arith::AndIOp::create(b, loc, first.valid, second.valid);
  } else {
    Type floating = bits == 64 ? Type(b.getF64Type()) : Type(b.getF32Type());
    auto decode = [&](Value value) -> Value {
      if (bits == 32)
        value = arith::TruncIOp::create(b, loc, b.getI32Type(), value);
      return arith::BitcastOp::create(b, loc, floating, value);
    };
    auto first = arith::AddFOp::create(b, loc, decode(a), decode(d));
    auto second = arith::AddFOp::create(b, loc, first, decode(scalar));
    result = arith::BitcastOp::create(b, loc, b.getIntegerType(bits), second);
    if (bits == 32)
      result = arith::ExtUIOp::create(b, loc, b.getI64Type(), result);
    valid = arith::ConstantIntOp::create(b, loc, 1, 1);
  }
  placeholder.getResult().replaceAllUsesWith(result);
  validity.getResult().replaceAllUsesWith(valid);
  placeholder.erase();
  validity.erase();
  if (failed(verify(*module)))
    return 1;
  std::error_code error;
  llvm::raw_fd_ostream out(argv[2], error, llvm::sys::fs::OF_Text);
  if (error)
    return 1;
  module->print(out);
  out << '\n';
}
