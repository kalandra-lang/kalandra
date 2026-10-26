#pragma once
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include <functional>

namespace kalandra {
// Recover immutable expression bounds at their use, including facts established
// before a loop. LVI can lose those facts across a backedge, or retain only the
// range at an expression's original definition. No input/range is assumed.
inline bool foldProvenIntegerComparisons(llvm::Function &fn, llvm::LazyValueInfo &lvi,
                                         llvm::DominatorTree &dom) {
  using namespace llvm;
  SmallVector<std::pair<ICmpInst *, bool>, 16> folds;
  unsigned queries = 32768, comparisons = 512;
  for (auto &block : fn) {
    auto *node = dom.getNode(&block);
    if (!node) continue;
    for (auto &instruction : block) {
      auto *cmp = dyn_cast<ICmpInst>(&instruction);
      if (!cmp || !cmp->getOperand(0)->getType()->isIntegerTy(128)) continue;
      if (!comparisons) break;
      --comparisons;
      DenseMap<Value *, ConstantRange> cache;
      std::function<ConstantRange(Value *, unsigned)> range =
          [&](Value *value, unsigned depth) -> ConstantRange {
        const auto width = value->getType()->getIntegerBitWidth();
        if (depth > 24 || !queries) return ConstantRange::getFull(width);
        if (auto found = cache.find(value); found != cache.end()) return found->second;
        --queries;
        auto result = lvi.getConstantRange(value, cmp, false);
        // Only strict dominators: facts at a later instruction in this block
        // need not hold if an intervening call throws or does not return.
        auto *parent = node->getIDom();
        for (unsigned levels = 0; parent && levels < 64 && queries;
             parent = parent->getIDom(), ++levels) {
          auto *at = parent->getBlock()->getTerminator();
          if (auto *definition = dyn_cast<Instruction>(value);
              definition && !dom.dominates(definition, at)) continue;
          --queries;
          result = result.intersectWith(lvi.getConstantRange(value, at, false));
        }
        if (auto *cast = dyn_cast<CastInst>(value)) {
          const auto op = cast->getOpcode();
          if (op == Instruction::SExt || op == Instruction::ZExt || op == Instruction::Trunc) {
            auto input = range(cast->getOperand(0), depth + 1);
            auto converted = op == Instruction::SExt ? input.signExtend(width)
                : op == Instruction::ZExt ? input.zeroExtend(width) : input.truncate(width);
            result = result.intersectWith(converted);
          }
        } else if (auto *binary = dyn_cast<BinaryOperator>(value)) {
          const auto op = binary->getOpcode();
          if (op == Instruction::Add || op == Instruction::Sub || op == Instruction::Mul) {
            auto left = range(binary->getOperand(0), depth + 1);
            auto right = range(binary->getOperand(1), depth + 1);
            // Modular ranges overapproximate even unflagged wrapping arithmetic.
            auto calculated = op == Instruction::Add ? left.add(right)
                : op == Instruction::Sub ? left.sub(right) : left.multiply(right);
            result = result.intersectWith(calculated);
          }
        }
        cache.try_emplace(value, result);
        return result;
      };
      auto left = range(cmp->getOperand(0), 0), right = range(cmp->getOperand(1), 0);
      if (left.isEmptySet() || right.isEmptySet()) continue;
      if (left.icmp(cmp->getPredicate(), right)) folds.emplace_back(cmp, true);
      else if (left.icmp(cmp->getInversePredicate(), right)) folds.emplace_back(cmp, false);
    }
    if (!comparisons || !queries) break;
  }
  // Keep every query on the original IR; mutation invalidates LVI's caches.
  for (auto [cmp, value] : folds) {
    cmp->replaceAllUsesWith(ConstantInt::get(cmp->getType(), value));
    cmp->eraseFromParent();
  }
  return !folds.empty();
}
} // namespace kalandra
