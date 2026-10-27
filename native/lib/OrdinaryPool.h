#pragma once
#include "../runtime/Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

namespace kalandra {
// Speculative private buffers. This is used only in a scalar ordinary entry:
// no task/foreign effects or caller-owned pointers survive the final proof.
// Replay starts with original scalar arguments, before any external output.
struct OrdinaryPool {
  static constexpr unsigned slots = 4, bytes = 128;
  bool active = false;
  llvm::Function &entry;
  llvm::Function &original;
  llvm::AllocaInst *storage = nullptr, *mask = nullptr, *enabled = nullptr;
  llvm::Function *heap = nullptr;
  llvm::BasicBlock *replay = nullptr;
  llvm::Value *fault;

  OrdinaryPool(llvm::Function &entry, llvm::Function &original, llvm::Value *fault)
      : entry(entry), original(original), fault(fault) {}
  void initialize();
  void allocate(llvm::CallInst *call);
  void release(llvm::CallInst *call);
};
} // namespace kalandra
