#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InlineAsm.h"

namespace kalandra {
// TSan restarts a trace part by recording the active call stack. Very deep,
// tiny native frames can exhaust that trace part before the physical stack
// guard runs. Charge a fixed diagnostic frame area to functions with growing
// native calls, so ordinary stack admission runs first on qualified stacks.
// Tail-only workers retain their constant-space path. Memory/race checking and
// function tracing remain enabled; this adds no logical recursion counter.
inline void padThreadSanitizerFrames(llvm::Module &module) {
  using namespace llvm;
  for (auto &fn : module) {
    if (fn.isDeclaration()) continue;
    bool growing = false;
    for (auto &block : fn)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction); call && !call->isMustTailCall()) {
          auto *target = call->getCalledFunction();
          growing |= !target || !target->isDeclaration();
        }
    if (!growing) continue;
    IRBuilder<> builder(&*fn.getEntryBlock().getFirstInsertionPt());
    auto *areaType = ArrayType::get(builder.getInt8Ty(), 1024);
    auto *area = builder.CreateAlloca(areaType, nullptr, "tsan.frame.area");
    auto *type = FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
    auto *barrier = InlineAsm::get(type, "", "*m,~{memory}", true);
    auto *use = builder.CreateCall(type, barrier, {area});
    use->addParamAttr(0, Attribute::get(module.getContext(), Attribute::ElementType, areaType));
  }
}
} // namespace kalandra
