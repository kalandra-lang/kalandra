#pragma once
#include "../runtime/Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <algorithm>

namespace kalandra {
// Speculative private buffers. This is used only in a scalar ordinary entry:
// no task/foreign effects or caller-owned pointers survive the final proof.
// Replay starts with original scalar arguments, before any external output.
struct OrdinaryPool {
  static constexpr unsigned stackBudget = 8192;
  unsigned slots = 4, bytes = 128;
  // Size every slot for this entry's largest constant allocation, preserving
  // cache-line alignment and the existing total static-stack budget. Keep four
  // slots: nested immutable borrows can retain more than input and output.
  bool configure(std::uint64_t largest) {
    if (largest > stackBudget / 4)
      return false;
    bytes = unsigned((std::max<std::uint64_t>(128, largest) + 63) & ~std::uint64_t(63));
    slots = 4;
    return slots * bytes <= stackBudget;
  }
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
