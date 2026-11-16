#pragma once
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
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
  using Query = std::pair<Value *, Instruction *>;
  DenseSet<Query> provenNonnegative;
  unsigned queries = 32768, comparisons = 512;
  for (auto &block : fn) {
    auto *node = dom.getNode(&block);
    if (!node) continue;
    for (auto &instruction : block) {
      auto *cmp = dyn_cast<ICmpInst>(&instruction);
      if (!cmp || !cmp->getOperand(0)->getType()->isIntegerTy(128)) continue;
      if (!comparisons) break;
      --comparisons;
      DenseMap<Query, ConstantRange> cache;
      DenseSet<Query> active;
      auto observed = [&](Value *value, Instruction *at) {
        const auto width = value->getType()->getIntegerBitWidth();
        if (!queries) return ConstantRange::getFull(width);
        --queries;
        auto result = lvi.getConstantRange(value, at, false);
        // Only strict dominators: facts at a later instruction in this block
        // need not hold if an intervening call throws or does not return.
        auto *context = dom.getNode(at->getParent());
        auto *parent = context ? context->getIDom() : nullptr;
        for (unsigned levels = 0; parent && levels < 64 && queries;
             parent = parent->getIDom(), ++levels) {
          auto *before = parent->getBlock()->getTerminator();
          if (auto *definition = dyn_cast<Instruction>(value);
              definition && !dom.dominates(definition, before)) continue;
          --queries;
          result = result.intersectWith(lvi.getConstantRange(value, before, false));
        }
        return result;
      };
      // Inductively retain a nonnegative loop invariant only when every
      // entering value and every defined backedge preserve it. Wrapping adds,
      // unknown loads, negative seeds and freeze are deliberately unproved.
      SmallPtrSet<PHINode *, 16> induction;
      SmallVector<Query, 32> provisional;
      std::function<bool(Value *, Instruction *, unsigned)> nonnegative =
          [&](Value *value, Instruction *at, unsigned depth) {
        const Query query{value, at};
        if (provenNonnegative.contains(query)) return true;
        if (depth > 24 || !queries) return false;
        auto accept = [&] { provisional.push_back(query); return true; };
        const auto known = observed(value, at);
        if (!known.isEmptySet() && known.isAllNonNegative()) return accept();
        if (auto *phi = dyn_cast<PHINode>(value)) {
          if (!induction.insert(phi).second) return accept();
          bool proven = phi->getNumIncomingValues() != 0;
          for (unsigned n = 0; proven && n < phi->getNumIncomingValues(); ++n)
            proven = nonnegative(phi->getIncomingValue(n),
                                  phi->getIncomingBlock(n)->getTerminator(), depth + 1);
          induction.erase(phi);
          return proven && accept();
        }
        if (auto *cast = dyn_cast<CastInst>(value)) {
          if (isa<ZExtInst>(cast)) return accept();
          if (isa<SExtInst>(cast) ||
              (isa<TruncInst>(cast) && llvm::cast<TruncInst>(cast)->hasNoSignedWrap()))
            return nonnegative(cast->getOperand(0), at, depth + 1) && accept();
        } else if (auto *binary = dyn_cast<BinaryOperator>(value)) {
          const auto op = binary->getOpcode();
          if ((op == Instruction::Shl || op == Instruction::Add || op == Instruction::Mul) &&
              binary->hasNoSignedWrap()) {
            if (op == Instruction::Shl)
              return nonnegative(binary->getOperand(0), at, depth + 1) && accept();
            if (op == Instruction::Add || op == Instruction::Mul)
              return nonnegative(binary->getOperand(0), at, depth + 1) &&
                     nonnegative(binary->getOperand(1), at, depth + 1) && accept();
          }
        } else if (auto *choice = dyn_cast<SelectInst>(value)) {
          return nonnegative(choice->getTrueValue(), at, depth + 1) &&
                 nonnegative(choice->getFalseValue(), at, depth + 1) && accept();
        }
        return false;
      };
      std::function<ConstantRange(Value *, Instruction *, unsigned)> range =
          [&](Value *value, Instruction *at, unsigned depth) -> ConstantRange {
        if (auto *constant = dyn_cast<ConstantInt>(value))
          return ConstantRange(constant->getValue());
        const auto width = value->getType()->getIntegerBitWidth();
        if (depth > 24 || !queries) return ConstantRange::getFull(width);
        const Query query{value, at};
        if (auto found = cache.find(query); found != cache.end()) return found->second;
        auto result = observed(value, at);
        if (!active.insert(query).second) return result;
        provisional.clear();
        const bool positive = nonnegative(value, at, 0);
        // A nested result may depend on an enclosing PHI hypothesis. Publish
        // none of those provisional facts until the entire induction closes.
        if (positive)
          for (const auto &fact : provisional) provenNonnegative.insert(fact);
        if (positive)
          result = result.intersectWith(ConstantRange(APInt::getZero(width),
                                                       APInt::getSignedMinValue(width)));
        // Recover relational bounds at a dominating *edge*, not merely its
        // destination block (which the other branch could also reach).
        auto *context = dom.getNode(at->getParent());
        for (auto *parent = context ? context->getIDom() : nullptr;
             parent && queries; parent = parent->getIDom()) {
          auto *branch = dyn_cast<CondBrInst>(parent->getBlock()->getTerminator());
          auto *condition = branch ? dyn_cast<ICmpInst>(branch->getCondition()) : nullptr;
          if (!condition || branch->getSuccessor(0) == branch->getSuccessor(1)) continue;
          const bool left = condition->getOperand(0) == value;
          if (!left && condition->getOperand(1) != value) continue;
          for (unsigned edge = 0; edge < 2; ++edge) {
            if (!dom.dominates(BasicBlockEdge(branch->getParent(), branch->getSuccessor(edge)),
                               at->getParent())) continue;
            auto predicate = edge == 0 ? condition->getPredicate()
                                       : condition->getInversePredicate();
            if (!left) predicate = ICmpInst::getSwappedPredicate(predicate);
            result = result.intersectWith(ConstantRange::makeAllowedICmpRegion(
                predicate, range(condition->getOperand(left ? 1 : 0), at, depth + 1)));
          }
        }
        if (auto *cast = dyn_cast<CastInst>(value)) {
          const auto op = cast->getOpcode();
          if (op == Instruction::SExt || op == Instruction::ZExt || op == Instruction::Trunc) {
            auto input = range(cast->getOperand(0), at, depth + 1);
            auto converted = op == Instruction::SExt ? input.signExtend(width)
                : op == Instruction::ZExt ? input.zeroExtend(width) : input.truncate(width);
            result = result.intersectWith(converted);
          }
        } else if (auto *binary = dyn_cast<BinaryOperator>(value)) {
          const auto op = binary->getOpcode();
          if (op == Instruction::Add || op == Instruction::Sub || op == Instruction::Mul) {
            auto left = range(binary->getOperand(0), at, depth + 1);
            auto right = range(binary->getOperand(1), at, depth + 1);
            // Modular ranges overapproximate even unflagged wrapping arithmetic.
            auto calculated = op == Instruction::Add ? left.add(right)
                : op == Instruction::Sub ? left.sub(right) : left.multiply(right);
            result = result.intersectWith(calculated);
          }
        } else if (auto *phi = dyn_cast<PHINode>(value)) {
          auto incoming = ConstantRange::getEmpty(width);
          for (unsigned n = 0; n < phi->getNumIncomingValues(); ++n) {
            auto *input = phi->getIncomingValue(n);
            auto *before = phi->getIncomingBlock(n)->getTerminator();
            auto edgeRange = ConstantRange::getFull(width);
            // A rotated loop's condition constrains the value *entering* its
            // PHI. That edge need not dominate the loop because of its initial
            // entry, but the fact is valid for this one incoming value.
            auto *branch = dyn_cast<CondBrInst>(before);
            auto *condition = branch ? dyn_cast<ICmpInst>(branch->getCondition()) : nullptr;
            if (condition && branch->getSuccessor(0) != branch->getSuccessor(1)) {
              const bool left = condition->getOperand(0) == input;
              if (left || condition->getOperand(1) == input) {
                auto predicate = branch->getSuccessor(0) == phi->getParent()
                                     ? condition->getPredicate() : condition->getInversePredicate();
                if (!left) predicate = ICmpInst::getSwappedPredicate(predicate);
                edgeRange = ConstantRange::makeAllowedICmpRegion(
                    predicate, range(condition->getOperand(left ? 1 : 0), before, depth + 1));
              }
            }
            incoming = incoming.unionWith(edgeRange.intersectWith(range(input, before, depth + 1)));
          }
          result = result.intersectWith(incoming);
        }
        active.erase(query);
        cache.try_emplace(query, result);
        return result;
      };
      auto left = range(cmp->getOperand(0), cmp, 0), right = range(cmp->getOperand(1), cmp, 0);
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
