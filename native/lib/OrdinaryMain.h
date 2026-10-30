#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace kalandra {
// The CLI uses the ordinary companion only for a parameterless scalar main.
// This is applied to its linker copy, not the emitted checked-library IR.
inline llvm::Function *addOrdinaryMain(llvm::Module &module) {
  using namespace llvm;
  if (module.getFunction("main"))
    return nullptr;
  const char *names[] = {"entry",        "float64_entry", "bool_entry",  "int32_entry",
                         "uint32_entry", "int64_entry",   "uint64_entry"};
  llvm::Function *entry = nullptr;
  unsigned kind = 0;
  bool direct = false;
  for (; kind < 7; ++kind) {
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
  auto *main = llvm::Function::Create(FunctionType::get(Type::getInt32Ty(ctx), false),
                                      GlobalValue::ExternalLinkage, "main", module);
  IRBuilder<> b(BasicBlock::Create(ctx, "entry", main));
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
      "kalandra_ordinary_finish_v1",
      FunctionType::get(b.getInt32Ty(), {b.getInt64Ty(), b.getInt32Ty(), b.getPtrTy()}, false));
  b.CreateRet(b.CreateCall(finish, {bits, b.getInt32(kind), fault}));
  return main;
}
} // namespace kalandra
