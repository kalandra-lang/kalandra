#pragma once
#include "NativeStack.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <bit>
#include <functional>

namespace kalandra {
struct OrdinaryReduction {
  llvm::BasicBlock *header = nullptr, *latch = nullptr, *exit = nullptr, *initial = nullptr;
  llvm::PHINode *accumulator = nullptr;
  llvm::LoadInst *load = nullptr;
  llvm::SExtInst *extend = nullptr;
  llvm::CondBrInst *overflow = nullptr;
  llvm::Value *base = nullptr;
  llvm::AllocaInst *allocation = nullptr;
  std::uint64_t bytes = 0, allocationBytes = 0;
};
inline std::optional<OrdinaryReduction> findOrdinaryReduction(llvm::Function &fn) {
  using namespace llvm;
  DominatorTree dom(fn);
  LoopInfo loops(dom);
  for (auto &block : fn) {
    auto *loop = loops.getLoopFor(&block);
    if (!loop || loop->getHeader() != &block || loop->getNumBlocks() != 2)
      continue;
    auto *latch = loop->getLoopLatch(), *initial = loop->getLoopPredecessor();
    auto *overflow = dyn_cast<CondBrInst>(block.getTerminator());
    auto *end = latch ? dyn_cast<CondBrInst>(latch->getTerminator()) : nullptr;
    if (!initial || !overflow || !end)
      continue;
    auto *done = dyn_cast<ICmpInst>(end->getCondition());
    if (!done ||
        (done->getPredicate() != ICmpInst::ICMP_EQ && done->getPredicate() != ICmpInst::ICMP_NE))
      continue;
    auto *next = dyn_cast<BinaryOperator>(done->getOperand(0));
    auto *limit = dyn_cast<ConstantInt>(done->getOperand(1));
    if (!next || next->getParent() != latch || next->getOpcode() != Instruction::Add || !limit ||
        !limit->getType()->isIntegerTy(64) || limit->isZero() || limit->getValue().ugt(65536))
      continue;
    auto *index = dyn_cast<PHINode>(next->getOperand(0));
    auto *one = dyn_cast<ConstantInt>(next->getOperand(1));
    if (!index || index->getParent() != &block || index->getNumIncomingValues() != 2 || !one ||
        !one->isOne() || index->getIncomingValueForBlock(latch) != next)
      continue;
    auto *start = dyn_cast_or_null<ConstantInt>(index->getIncomingValueForBlock(initial));
    if (!start || !start->isZero())
      continue;
    const unsigned exitIndex = done->getPredicate() == ICmpInst::ICMP_EQ ? 0 : 1;
    auto *exit = end->getSuccessor(exitIndex);
    if (loop->contains(exit) || end->getSuccessor(1 - exitIndex) != &block)
      continue;
    for (auto &inst : block.phis()) {
      auto *acc = &inst;
      if (acc == index || !acc->getType()->isIntegerTy(64) || acc->getNumIncomingValues() != 2)
        continue;
      auto *zero = dyn_cast_or_null<ConstantInt>(acc->getIncomingValueForBlock(initial));
      auto *narrow = dyn_cast_or_null<TruncInst>(acc->getIncomingValueForBlock(latch));
      if (!zero || !zero->isZero() || !narrow || narrow->getParent() != latch ||
          !narrow->getSrcTy()->isIntegerTy(128))
        continue;
      auto *sum = dyn_cast<BinaryOperator>(narrow->getOperand(0));
      if (!sum || sum->getParent() != &block || sum->getOpcode() != Instruction::Add)
        continue;
      auto *a = dyn_cast<SExtInst>(sum->getOperand(0)), *b = dyn_cast<SExtInst>(sum->getOperand(1));
      if (!a || !b)
        continue;
      if (a->getOperand(0) != acc)
        std::swap(a, b);
      auto *load = dyn_cast<LoadInst>(b->getOperand(0));
      if (a->getOperand(0) != acc || !load || !load->getType()->isIntegerTy(64) ||
          load->getParent() != &block || !load->isSimple() || !load->hasOneUse() ||
          !isa<AllocaInst>(getUnderlyingObject(load->getPointerOperand())))
        continue;
      // Speculation must not turn an early arithmetic fault into an invalid
      // later read. The address advances exactly eight bytes from an invariant
      // private base; an entry guard checks the complete region's extent.
      auto *address = dyn_cast<GetElementPtrInst>(load->getPointerOperand());
      if (!address || address->getParent() != &block || address->getNumIndices() == 0 ||
          !loop->isLoopInvariant(address->getPointerOperand()))
        continue;
      bool indices = true;
      unsigned position = 0;
      for (auto &operand : address->indices()) {
        if (++position == address->getNumIndices())
          indices &= operand.get() == index;
        else {
          auto *zero = dyn_cast<ConstantInt>(operand);
          indices &= zero && zero->isZero();
        }
      }
      auto &layout = fn.getParent()->getDataLayout();
      auto elementBytes = layout.getTypeAllocSize(address->getResultElementType());
      auto *allocation = cast<AllocaInst>(getUnderlyingObject(address));
      auto *allocationCount = dyn_cast<ConstantInt>(allocation->getArraySize());
      auto objectBytes = layout.getTypeAllocSize(allocation->getAllocatedType());
      if (!indices || elementBytes.isScalable() || elementBytes.getFixedValue() != 8 ||
          objectBytes.isScalable() || !allocationCount ||
          allocationCount->getValue().getActiveBits() > 14 || !allocationCount->getZExtValue() ||
          objectBytes.getFixedValue() > 8192 / allocationCount->getZExtValue())
        continue;
      const auto allocated = objectBytes.getFixedValue() * allocationCount->getZExtValue();
      const auto bytes = limit->getZExtValue() * 8;
      if (bytes > allocated ||
          layout.isNonIntegralPointerType(address->getPointerOperand()->getType()) ||
          layout.getPointerSizeInBits(
              address->getPointerOperand()->getType()->getPointerAddressSpace()) != 64)
        continue;
      auto *cmp = dyn_cast<ICmpInst>(overflow->getCondition());
      if (!cmp || !cmp->getOperand(0)->getType()->isIntegerTy(128))
        continue;
      auto *rhs = dyn_cast<ConstantInt>(cmp->getOperand(1));
      if (!rhs)
        continue;
      APInt bias(128, 0);
      auto *tested = cmp->getOperand(0);
      if (auto *add = dyn_cast<BinaryOperator>(tested); add && add->getOpcode() == Instruction::Add)
        if (auto *c = dyn_cast<ConstantInt>(add->getOperand(1))) {
          bias = c->getValue();
          tested = add->getOperand(0);
        }
      if (tested != sum)
        continue;
      ConstantRange valid(APInt::getSignedMinValue(64).sext(128), APInt(128, 1).shl(63));
      auto truth =
          ConstantRange::makeExactICmpRegion(cmp->getPredicate(), rhs->getValue()).subtract(bias);
      unsigned safe;
      if (truth == valid)
        safe = 0;
      else if (truth == valid.inverse())
        safe = 1;
      else
        continue;
      if (overflow->getSuccessor(safe) != latch || loop->contains(overflow->getSuccessor(1 - safe)))
        continue;
      SmallPtrSet<Instruction *, 32> allowed{acc, index,    load,   address, a,    b,  sum,
                                             cmp, overflow, narrow, next,    done, end};
      if (auto *biasInstruction = dyn_cast<Instruction>(cmp->getOperand(0)))
        allowed.insert(biasInstruction);
      bool pure = true;
      for (auto *bb : loop->blocks())
        for (auto &i : *bb)
          if (!allowed.contains(&i) || i.mayHaveSideEffects() ||
              (i.mayReadFromMemory() && &i != load))
            pure = false;
      if (!pure)
        continue;
      // Replay is legal only before any caller-visible fault output. All other
      // memory/effects were already proven private by the ordinary-entry proof.
      SmallPtrSet<BasicBlock *, 32> ancestors;
      SmallVector<BasicBlock *, 32> pending{initial};
      while (!pending.empty()) {
        auto *bb = pending.pop_back_val();
        if (!ancestors.insert(bb).second)
          continue;
        for (auto *pred : predecessors(bb))
          pending.push_back(pred);
      }
      for (auto *bb : ancestors)
        for (auto &i : *bb) {
          Value *destination = nullptr;
          if (auto *store = dyn_cast<StoreInst>(&i))
            destination = store->getPointerOperand();
          if (auto *set = dyn_cast<MemSetInst>(&i))
            destination = set->getDest();
          if (destination && !isa<AllocaInst>(getUnderlyingObject(destination)))
            pure = false;
        }
      if (pure)
        return OrdinaryReduction{&block,     latch,    exit,
                                 initial,    acc,      load,
                                 b,          overflow, address->getPointerOperand(),
                                 allocation, bytes,    allocated};
    }
  }
  return std::nullopt;
}
// Deferred range validation is limited to a read-only, constant-trip reduction.
// For N terms and k=ceil(log2(N)), terms in [-2^(63-k),2^(63-k)) have every
// partial sum in int64. Accumulation is modular until the exit validates a witness
// for every term; rejected values tail-replay the original checked graph.
// This includes the previous unsigned-32 domain and wide signed terms without
// replaying an otherwise valid signed reduction. No range promises are attached
// to untrusted loads, and no overflow flags are added to speculative arithmetic.
inline bool
specializeOrdinaryReductions(llvm::Module &module,
                             const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        fn.hasFnAttribute("kalandra.ordinary.local-buffers") &&
        !fn.hasFnAttribute("kalandra.ordinary.reduction") && fn.use_empty())
      roots.push_back(&fn);
  bool changed = false;
  for (auto *original : roots) {
    ValueToValueMapTy map;
    auto *fast = CloneFunction(original, map);
    fast->setName(original->getName() + ".reduction-candidate");
    fast->setLinkage(GlobalValue::InternalLinkage);
    auto &ctx = module.getContext();
    unsigned reductions = 0;
    while (reductions < 8) {
      auto found = findOrdinaryReduction(*fast);
      if (!found)
        break;
      auto q = *found;
      // Each guarded exit needs fresh replay IR. Simplifying an earlier exit
      // may specialize its arguments using that predecessor's path facts.
      auto *fallback = BasicBlock::Create(ctx, "range.replay", fast);
      IRBuilder<> again(fallback);
      SmallVector<Value *, 8> currentArgs;
      for (auto &arg : fast->args())
        currentArgs.push_back(&arg);
      auto *retry = again.CreateCall(original, currentArgs);
      retry->setCallingConv(original->getCallingConv());
      retry->setAttributes(original->getAttributes().removeFnAttributes(ctx));
      retry->setMetadata("kalandra.ordinary.range-replay", MDNode::get(ctx, {}));
      retry->setTailCallKind(CallInst::TCK_MustTail);
      again.CreateRet(retry);

      auto *memory = SplitEdge(q.initial, q.header);
      auto *memoryTerminator = memory->getTerminator();
      IRBuilder<> extent(memoryTerminator);
      auto *offset = extent.CreatePtrDiff(extent.getInt8Ty(), q.base, q.allocation);
      extent.CreateCondBr(
          extent.CreateICmpULE(offset, extent.getInt64(q.allocationBytes - q.bytes)), q.header,
          fallback);
      memoryTerminator->eraseFromParent();
      q.initial = memory;

      // New phi is a second associative reduction. It cannot influence the
      // fixed trip count or cause speculative execution to fail to terminate.
      auto *witness = PHINode::Create(Type::getInt64Ty(ctx), 2, "range.witness", q.header->begin());
      witness->addIncoming(ConstantInt::get(Type::getInt64Ty(ctx), 0), q.initial);
      IRBuilder<> b(q.extend);
      // Later iterations may be reached only by this speculative copy. Do not
      // inherit UB-implying load promises from an original early-fault path.
      q.load->dropUBImplyingAttrsAndMetadata();
      q.load->setAlignment(Align(1));
      auto *raw = b.CreateFreeze(q.load, "range.raw");
      // N<=2^k, so N times either endpoint fits int64 (the upper is exclusive).
      // Unsigned modular bias validates the whole signed window without UB,
      // including for rejected INT64_MIN/MAX terms. A single term always fits.
      const unsigned magnitude = 63 - std::bit_width(q.bytes / 8 - 1);
      Value *high = b.getInt64(0);
      if (magnitude < 63) {
        auto *biased = b.CreateAdd(raw, b.getInt64(1ULL << magnitude), "range.biased");
        high = b.CreateLShr(biased, b.getInt64(magnitude + 1));
      }
      auto *nextWitness = b.CreateOr(witness, high);
      witness->addIncoming(nextWitness, q.latch);
      q.extend->setOperand(0, raw);
      auto *old = q.overflow;
      auto *bad = old->getSuccessor(0) == q.latch ? old->getSuccessor(1) : old->getSuccessor(0);
      bad->removePredecessor(q.header);
      UncondBrInst::Create(q.latch, old->getIterator());
      old->eraseFromParent();
      auto *check = SplitEdge(q.latch, q.exit);
      auto *terminator = check->getTerminator();
      IRBuilder<> exit(terminator);
      exit.CreateCondBr(exit.CreateICmpEQ(nextWitness, exit.getInt64(0)), q.exit, fallback);
      terminator->eraseFromParent();
      ++reductions;
      simplify(*fast, true);
    }
    if (!reductions || verifyFunction(*fast) ||
        nativeStackBudgetError(*fast, module.getDataLayout())) {
      fast->eraseFromParent();
      continue;
    }
    auto name = original->getName().str();
    auto linkage = original->getLinkage();
    original->setName(name + ".wide");
    original->setLinkage(GlobalValue::InternalLinkage);
    original->removeFnAttr("kalandra.ordinary.entry");
    original->addFnAttr(Attribute::NoInline);
    fast->setName(name);
    fast->setLinkage(linkage);
    fast->addFnAttr("kalandra.ordinary.reduction");
    changed = true;
  }
  return changed;
}
} // namespace kalandra
