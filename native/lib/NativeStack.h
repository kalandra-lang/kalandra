#pragma once
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include <optional>
#include <string>
namespace kalandra {
// Read SP without llvm.stacksave's dynamic-stack semantics. The read remains
// at the guard. Call-free leaf paths can be outlined before this physical
// check. Targets are the native architectures qualified by CMake.
inline llvm::Value *nativeStackPointer(llvm::IRBuilder<> &builder) {
  auto target = builder.GetInsertBlock()->getModule()->getTargetTriple();
  if (target.str().empty()) target = llvm::Triple(KALANDRA_NATIVE_CPU_TARGET_TRIPLE);
  const char *name = target.isAArch64() ? "sp" : "rsp";
  auto &context = builder.getContext();
  auto *read = llvm::Intrinsic::getOrInsertDeclaration(builder.GetInsertBlock()->getModule(),
      llvm::Intrinsic::read_volatile_register, {builder.getInt64Ty()});
  auto *reg = llvm::MDNode::get(context, llvm::MDString::get(context, name));
  return builder.CreateCall(read, {llvm::MetadataAsValue::get(context, reg)});
}
// Conservative admission for the pinned CPU backend: count all SSA values,
// arguments, outgoing argument copies and fixed allocas, rather than assuming
// register allocation or stack-slot reuse. Keep margin below runtime headroom.
inline std::optional<std::string> nativeStackBudgetError(const llvm::Function &function,
                                                         const llvm::DataLayout &layout,
                                                         bool numericLeaf = false,
                                                         std::uint64_t reservedBytes = 0) {
  const std::uint64_t budget = 16 * 1024 - reservedBytes;
  std::uint64_t total = 0;
  const auto account = [&](llvm::Type *type, std::uint64_t count = 1) {
    if (type->isVoidTy())
      return true;
    if (!type->isSized())
      return false;
    const auto size = layout.getTypeAllocSize(type);
    if (size.isScalable() ||
        (size.getFixedValue() && count > (budget - total) / size.getFixedValue()))
      return false;
    total += size.getFixedValue() * count;
    return true;
  };
  bool valid = account(function.getReturnType());
  for (const auto &argument : function.args())
    valid = valid && account(argument.getType());
  for (const auto &block : function)
    for (const auto &instruction : block) {
      if (!numericLeaf)
        valid = valid && account(instruction.getType());
      if (const auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
        const auto *count = llvm::dyn_cast<llvm::ConstantInt>(allocation->getArraySize());
        valid = valid && count && count->getValue().getActiveBits() <= 64 &&
                account(allocation->getAllocatedType(), count->getZExtValue());
      }
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        if (numericLeaf) {
          const auto *target = call->getCalledFunction();
          valid =
              valid && target &&
              (target->isIntrinsic() || target->getName() == "kalandra_alloc_v1" ||
               target->getName() == "kalandra_free_v1" ||
               target->getName() == "kalandra_source_abort_v2" ||
               target->getName() == "kalandra_fault_v1" ||
               target->getName() == "kalandra_poll_slow_v1" ||
               target->getName() == "kalandra_poll_v1");
        }
        for (const auto &argument : call->args())
          // Intrinsic metadata describes a register or alias scope; it has no
          // representation in the physical outgoing call frame.
          if (!argument->getType()->isMetadataTy())
            valid = valid && account(argument->getType());
      }
    }
  if (!valid)
    return "function exceeds the conservative native stack budget: " + function.getName().str();
  return {};
}
} // namespace kalandra
