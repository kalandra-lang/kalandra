#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace kalandra {
// The CLI uses the ordinary companion only for a parameterless scalar main.
// This is applied to its linker copy, not the emitted checked-library IR.
inline llvm::Function *addOrdinaryMain(llvm::Module &module, bool application = false) {
  using namespace llvm;
  if (module.getFunction("main") || module.getFunction("wmain"))
    return nullptr;
  const char *names[] = {"entry",        "float64_entry", "bool_entry",   "int32_entry",
                         "uint32_entry", "int64_entry",   "uint64_entry", "unit_entry"};
  llvm::Function *entry = nullptr;
  unsigned kind = 0;
  bool direct = false;
  for (; kind < std::size(names); ++kind) {
    entry = module.getFunction(std::string("_mlir_ciface_kalandra_") + names[kind] +
                               "_ordinary_direct_v1");
    if (entry && entry->hasFnAttribute("kalandra.ordinary.direct") && entry->arg_empty()) {
      direct = true;
      break;
    }

    entry =
        module.getFunction(std::string("_mlir_ciface_kalandra_") + names[kind] + "_ordinary_v1");
    if (entry && entry->hasFnAttribute("kalandra.ordinary.entry") && entry->arg_size() == 1)
      break;
    entry = nullptr;
  }
  if (!entry)
    return nullptr;
  auto &ctx = module.getContext();
  if (application && kind != 3)
    return nullptr;
  auto *mainType =
      application ? FunctionType::get(Type::getInt32Ty(ctx),
                                      {Type::getInt32Ty(ctx), PointerType::getUnqual(ctx)}, false)
                  : FunctionType::get(Type::getInt32Ty(ctx), false);
  auto *main = llvm::Function::Create(
      mainType, GlobalValue::ExternalLinkage,
      application && module.getTargetTriple().isOSWindows() ? "wmain" : "main", module);
  IRBuilder<> b(BasicBlock::Create(ctx, "entry", main));
  if (application) {
    auto initialize = module.getOrInsertFunction(
        "kalandra_application_initialize_v1",
        FunctionType::get(b.getInt1Ty(), {b.getInt32Ty(), b.getPtrTy()}, false));
    auto *ready = BasicBlock::Create(ctx, "arguments.valid", main);
    auto *invalid = BasicBlock::Create(ctx, "arguments.invalid", main);
    b.CreateCondBr(b.CreateCall(initialize, {main->getArg(0), main->getArg(1)}), ready, invalid);
    b.SetInsertPoint(invalid);
    b.CreateRet(b.getInt32(1));
    b.SetInsertPoint(ready);
    // This is only the executable link copy. Checked library/test entries keep
    // their original diagnostics and ordinary companion ABI.
    if (auto *failure = module.getFunction("kalandra_ordinary_fail_v1"))
      failure->setName("kalandra_application_fail_v1");
  }
  auto *fault = b.CreateAlloca(ArrayType::get(b.getInt64Ty(), 3));
  llvm::SmallVector<Value *, 1> args;
  if (!direct)
    args.push_back(fault);
  auto *call = b.CreateCall(entry, args);
  call->setCallingConv(entry->getCallingConv());
  call->setAttributes(entry->getAttributes().removeFnAttributes(ctx));
  // A direct entry can return only on success, including legitimate NaNs and
  // minimum integers. The existing display adapter therefore sees code zero.
  if (direct)
    b.CreateStore(b.getInt64(0), fault);
  Value *bits = call;
  if (bits->getType()->isFloatingPointTy())
    bits = b.CreateBitCast(bits, b.getIntNTy(bits->getType()->getPrimitiveSizeInBits()));
  bits = b.CreateZExtOrTrunc(bits, b.getInt64Ty());
  auto finish = module.getOrInsertFunction(
      application ? "kalandra_application_finish_v1" : "kalandra_ordinary_finish_v1",
      FunctionType::get(b.getInt32Ty(), {b.getInt64Ty(), b.getInt32Ty(), b.getPtrTy()}, false));
  b.CreateRet(b.CreateCall(finish, {bits, b.getInt32(kind), fault}));
  return main;
}
} // namespace kalandra
