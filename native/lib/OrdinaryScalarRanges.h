#pragma once
#include "NativeStack.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopPeel.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include <cmath>
#include <functional>

namespace kalandra {
struct OrdinaryExactScale {
  llvm::Argument *input;
  llvm::APFloat coefficient;
  unsigned operations;
};
// Materialize a proven-exact dyadic using bounded integer fixed point. Its
// product fits the destination precision (and therefore signed i64). The
// AArch64 scaled conversion avoids a second rounding and keeps unequal scales
// distinct through CFG tail merging. All other targets retain strict FP IR.
inline llvm::Value *ordinaryExactScaleValue(llvm::IRBuilder<> &b, const OrdinaryExactScale &proof,
                                            llvm::Type *type, llvm::Module &module) {
  using namespace llvm;
  const unsigned fraction = type->isFloatTy() ? 23 : 52;
  auto raw = proof.coefficient.bitcastToAPInt();
  auto mantissa = raw & APInt::getLowBitsSet(raw.getBitWidth(), fraction);
  mantissa.setBit(fraction);
  const unsigned trailing = mantissa.countr_zero();
  auto multiplier = mantissa.lshr(trailing).zextOrTrunc(64);
  const unsigned exponentBits = type->isFloatTy() ? 8 : 11;
  const int exponent =
      int((raw.lshr(fraction) & APInt::getLowBitsSet(raw.getBitWidth(), exponentBits))
              .getZExtValue()) -
      (type->isFloatTy() ? 127 : 1023) - int(fraction - trailing);
  auto *product = b.CreateMul(proof.input, ConstantInt::get(b.getInt64Ty(), multiplier));
  if (module.getTargetTriple().isAArch64() && exponent < 0 && exponent >= -64) {
    auto *signature = llvm::FunctionType::get(type, {b.getInt64Ty()}, false);
    const std::string instruction = std::string("scvtf ${0:") + (type->isFloatTy() ? "s" : "d") +
                                    "}, $1, #" + std::to_string(-exponent);
    auto *call =
        b.CreateCall(signature, InlineAsm::get(signature, instruction, "=w,r", false), {product});
    call->setDoesNotThrow();
    call->setMemoryEffects(MemoryEffects::none());
    call->addFnAttr(Attribute::WillReturn);
    call->setMetadata("kalandra.ordinary.fixed-convert", MDNode::get(module.getContext(), {}));
    return call;
  }
  return b.CreateFMul(b.CreateSIToFP(product, type),
                      ConstantFP::get(type, std::ldexp(1.0, exponent)));
}
// A guarded integer has at most 16 significant bits. A positive normal
// dyadic coefficient with s significant bits yields an exact product when
// 16+s fits the destination precision; the largest input additionally checks
// overflow, and positivity/normality exclude signed-zero and underflow changes.
// Recursive proofs require every original operation and coefficient combination
// to be exact. Folding therefore preserves rounding, without fast-math flags.
inline bool ordinaryExactCoefficient(const llvm::APFloat &c) {
  using namespace llvm;
  if (!c.isNormal() || c.isNegative())
    return false;
  const unsigned fraction = &c.getSemantics() == &APFloat::IEEEsingle() ? 23 : 52;
  APInt significand =
      c.bitcastToAPInt() & APInt::getLowBitsSet(c.bitcastToAPInt().getBitWidth(), fraction);
  significand.setBit(fraction);
  if (16 + significand.getActiveBits() - significand.countr_zero() > fraction + 1)
    return false;
  APFloat max(c.getSemantics());
  max.convertFromAPInt(APInt(64, 65535), false, APFloat::rmNearestTiesToEven);
  return max.multiply(c, APFloat::rmNearestTiesToEven) == APFloat::opOK && max.isNormal();
}
inline std::optional<OrdinaryExactScale> ordinaryExactScale(llvm::Value *v, unsigned depth = 0) {
  using namespace llvm;
  if (depth > 16 || !(v->getType()->isFloatTy() || v->getType()->isDoubleTy()))
    return std::nullopt;
  if (auto *convert = dyn_cast<CastInst>(v);
      convert && (isa<SIToFPInst>(convert) || isa<UIToFPInst>(convert))) {
    Value *integer = convert->getOperand(0);
    if (auto *truncate = dyn_cast<TruncInst>(integer)) {
      const unsigned width = truncate->getDestTy()->getIntegerBitWidth();
      if (width < (isa<UIToFPInst>(convert) ? 16 : 17))
        return std::nullopt;
      integer = truncate->getOperand(0);
    }
    auto *arg = dyn_cast<Argument>(integer);
    if (!arg || !arg->getType()->isIntegerTy(64) || !arg->hasAttribute(Attribute::Range))
      return std::nullopt;
    auto range = arg->getAttribute(Attribute::Range).getRange();
    if (!ConstantRange(APInt(64, 0), APInt(64, 65536)).contains(range))
      return std::nullopt;
    return OrdinaryExactScale{arg, APFloat::getOne(convert->getType()->getFltSemantics()), 0};
  }
  auto *op = dyn_cast<BinaryOperator>(v);
  if (!op)
    return std::nullopt;
  if (op->getOpcode() == Instruction::FMul)
    for (unsigned k = 0; k < 2; ++k)
      if (auto *constant = dyn_cast<ConstantFP>(op->getOperand(k))) {
        auto child = ordinaryExactScale(op->getOperand(1 - k), depth + 1);
        if (!child)
          return std::nullopt;
        if (child->coefficient.multiply(constant->getValueAPF(), APFloat::rmNearestTiesToEven) !=
                APFloat::opOK ||
            !ordinaryExactCoefficient(child->coefficient))
          return std::nullopt;
        ++child->operations;
        return child;
      }
  if (op->getOpcode() == Instruction::FAdd) {
    auto a = ordinaryExactScale(op->getOperand(0), depth + 1),
         b = ordinaryExactScale(op->getOperand(1), depth + 1);
    if (!a || !b || a->input != b->input ||
        a->coefficient.add(b->coefficient, APFloat::rmNearestTiesToEven) != APFloat::opOK ||
        !ordinaryExactCoefficient(a->coefficient))
      return std::nullopt;
    a->operations += b->operations + 1;
    return a;
  }
  return std::nullopt;
}
// Count-only versioning can unroll strict floating chains for arbitrary data.
// It does not use the small-domain exact-scaling proof or alter FP flags/order.
inline llvm::Function *
ordinaryWideCountedBody(llvm::Function &original, unsigned count,
                        const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  ValueToValueMapTy map;
  auto *fn = CloneFunction(&original, map);
  fn->setLinkage(GlobalValue::InternalLinkage);
  fn->setName(original.getName() + ".wide-counted");
  fn->removeFnAttr("kalandra.ordinary.entry");
  fn->addParamAttr(count, Attribute::get(fn->getContext(), Attribute::Range,
                                         ConstantRange(APInt(64, 2), APInt(64, 65536))));
  simplify(*fn, true);
  bool changed = false;
  if (fn->getInstructionCount() <= 128) {
    DominatorTree dom(*fn);
    LoopInfo loops(dom);
    AssumptionCache ac(*fn);
    TargetLibraryInfoImpl impl(fn->getParent()->getTargetTriple());
    TargetLibraryInfo tli(impl);
    ScalarEvolution se(*fn, tli, ac, dom, loops);
    TargetTransformInfo tti(fn->getParent()->getDataLayout());
    for (auto *loop : loops.getLoopsInPreorder()) {
      unsigned size = 0, fp = 0;
      bool pure = loop->getSubLoops().empty();
      for (auto *block : loop->blocks())
        for (auto &i : *block) {
          ++size;
          fp += i.getOpcode() == Instruction::FAdd || i.getOpcode() == Instruction::FMul;
          if (!i.isTerminator() &&
              (i.mayHaveSideEffects() || i.mayReadFromMemory() || isa<CallBase>(i)))
            pure = false;
        }
      if (pure && size <= 12 && fp > 0 && fp <= 2) {
        formLCSSARecursively(*loop, dom, &loops, &se);
        UnrollLoopOptions options{4, true, true, false, true, false, nullptr, 128};
        changed = UnrollLoop(loop, options, &loops, &se, &dom, &ac, &tti, nullptr, true) !=
                  LoopUnrollResult::Unmodified;
        break;
      }
    }
  }
  if (changed)
    simplify(*fn, true);
  if (!changed || verifyFunction(*fn) ||
      nativeStackBudgetError(*fn, fn->getParent()->getDataLayout())) {
    fn->eraseFromParent();
    return nullptr;
  }
  fn->addFnAttr(Attribute::NoInline);
  return fn;
}
inline bool
specializeOrdinaryScalarRanges(llvm::Module &module,
                               const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        !fn.hasFnAttribute("kalandra.ordinary.local-buffers") &&
        !fn.hasFnAttribute("kalandra.ordinary.scalar-ranges") &&
        (fn.getReturnType()->isFloatTy() || fn.getReturnType()->isDoubleTy()))
      roots.push_back(&fn);
  bool changed = false;
  for (auto *original : roots) {
    DominatorTree dt(*original);
    LoopInfo li(dt);
    Argument *counter = nullptr;
    for (auto *loop : li.getLoopsInPreorder())
      for (auto &phi : loop->getHeader()->phis()) {
        if (!phi.getType()->isIntegerTy(64) || phi.getNumIncomingValues() != 2)
          continue;
        for (unsigned k = 0; k < 2; ++k) {
          auto *arg = dyn_cast<Argument>(phi.getIncomingValue(k));
          auto *step = dyn_cast<BinaryOperator>(phi.getIncomingValue(1 - k));
          if (!arg || !step || step->getOpcode() != Instruction::Add || step->getOperand(0) != &phi)
            continue;
          auto *amount = dyn_cast<ConstantInt>(step->getOperand(1));
          if (amount && amount->isMinusOne())
            counter = arg;
        }
      }
    if (!counter)
      continue;
    bool eligible = true;
    for (auto &arg : original->args())
      if (&arg != original->getArg(original->arg_size() - 1) && !arg.getType()->isIntegerTy(64))
        eligible = false;
    if (!eligible)
      continue;
    unsigned count = counter->getArgNo();
    ValueToValueMapTy zeroMap;
    auto *zero = CloneFunction(original, zeroMap);
    zero->setLinkage(GlobalValue::InternalLinkage);
    zero->setName(original->getName() + ".zero-range");
    zero->getArg(count)->replaceAllUsesWith(ConstantInt::get(counter->getType(), 0));
    simplify(*zero, true);
    DominatorTree zd(*zero);
    LoopInfo zl(zd);
    unsigned zeroSize = 0;
    bool safe = zl.empty();
    for (auto &block : *zero)
      for (auto &i : block) {
        ++zeroSize;
        if (isa<AllocaInst>(i) || isa<InvokeInst>(i) || i.isAtomic())
          safe = false;
        if (auto *call = dyn_cast<CallBase>(&i);
            call && (!call->getCalledFunction() || !call->getCalledFunction()->isIntrinsic()))
          safe = false;
      }
    if (!safe || zeroSize > 32) {
      zero->eraseFromParent();
      continue;
    }
    ValueToValueMapTy sm;
    auto *small = CloneFunction(original, sm);
    small->setLinkage(GlobalValue::InternalLinkage);
    small->setName(original->getName() + ".scalar-small");
    auto &ctx = module.getContext();
    for (auto &arg : small->args())
      if (arg.getType()->isIntegerTy(64))
        small->addParamAttr(arg.getArgNo(),
                            Attribute::get(ctx, Attribute::Range,
                                           ConstantRange(APInt(64, arg.getArgNo() == count ? 1 : 0),
                                                         APInt(64, 65536))));
    simplify(*small, true);
    {
      DominatorTree dom(*small);
      LoopInfo loops(dom);
      AssumptionCache ac(*small);
      TargetLibraryInfoImpl impl(module.getTargetTriple());
      TargetLibraryInfo tli(impl);
      ScalarEvolution se(*small, tli, ac, dom, loops);
      for (auto *loop : loops.getLoopsInPreorder()) {
        unsigned size = 0;
        bool fp = false;
        for (auto *block : loop->blocks())
          for (auto &i : *block) {
            ++size;
            fp |= i.getOpcode() == Instruction::FAdd || i.getOpcode() == Instruction::FMul;
          }
        if (fp && size <= 32 && canPeel(loop)) {
          // Peeling requires explicit loop-exit phis. Verify its result before
          // invoking optimizers that assume valid SSA dominance.
          formLCSSARecursively(*loop, dom, &loops, &se);
          ValueToValueMapTy peeled;
          peelLoop(loop, 1, false, &loops, &se, dom, &ac, true, peeled);
          break;
        }
      }
    }
    if (verifyFunction(*small, &errs())) {
      small->eraseFromParent();
      zero->eraseFromParent();
      continue;
    }
    simplify(*small, true);
    DominatorTree sd(*small);
    LoopInfo sl(sd);
    SmallVector<Instruction *, 8> folds;
    for (auto &block : *small)
      if (!sl.getLoopFor(&block))
        for (auto &i : block)
          if (auto proof = ordinaryExactScale(&i); proof && proof->operations > 1)
            folds.push_back(&i);
    unsigned folded = 0;
    for (auto *i : llvm::reverse(folds))
      if (auto proof = ordinaryExactScale(i); !i->use_empty() && proof && proof->operations > 1) {
        IRBuilder<> b(i);
        auto *value = ordinaryExactScaleValue(b, *proof, i->getType(), module);
        i->replaceAllUsesWith(value);
        i->eraseFromParent();
        ++folded;
      }
    if (!folded) {
      small->eraseFromParent();
      zero->eraseFromParent();
      continue;
    }
    if (verifyFunction(*small, &errs())) {
      small->eraseFromParent();
      zero->eraseFromParent();
      continue;
    }
    simplify(*small, true);
    // Keep the one-iteration return separate from the loop's exit phi.
    // The general worker has at least two iterations and needs no extra
    // first-iteration exit dispatch after its exact peeled iteration.
    ValueToValueMapTy oneMap;
    // This variant is unrestricted: preserve the original operations for
    // every input, with only the countdown fixed to one. Its entry therefore
    // needs no input-range guard at all.
    auto *one = CloneFunction(original, oneMap);
    one->setLinkage(GlobalValue::InternalLinkage);
    one->setName(original->getName() + ".range-one");
    one->getArg(count)->replaceAllUsesWith(ConstantInt::get(counter->getType(), 1));
    simplify(*one, true);
    {
      DominatorTree dom(*one);
      LoopInfo loops(dom);
      AssumptionCache ac(*one);
      TargetLibraryInfoImpl impl(module.getTargetTriple());
      TargetLibraryInfo tli(impl);
      ScalarEvolution se(*one, tli, ac, dom, loops);
      for (auto *loop : loops.getLoopsInPreorder())
        if (canPeel(loop)) {
          formLCSSARecursively(*loop, dom, &loops, &se);
          ValueToValueMapTy peeled;
          peelLoop(loop, 1, false, &loops, &se, dom, &ac, true, peeled);
          break;
        }
    }
    bool validOne = !verifyFunction(*one);
    if (validOne)
      simplify(*one, true);
    bool compact = validOne && one->size() == 1 && one->getInstructionCount() <= 32;
    for (auto &block : *one)
      for (auto &i : block) {
        if (isa<AllocaInst>(i) || isa<InvokeInst>(i) || i.isAtomic())
          compact = false;
        if (auto *call = dyn_cast<CallBase>(&i);
            call && !(call->getCalledFunction() && call->getCalledFunction()->isIntrinsic()) &&
            !(call->isInlineAsm() && call->getMetadata("kalandra.ordinary.fixed-convert")))
          compact = false;
      }
    if (!compact) {
      one->eraseFromParent();
      one = nullptr;
    } else {
      small->addParamAttr(count, Attribute::get(ctx, Attribute::Range,
                                                ConstantRange(APInt(64, 2), APInt(64, 65536))));
      simplify(*small, true);
    }
    // Reduce loop-control traffic for tiny scalar recurrences. Duplicate the
    // original operation chain in order; never introduce extra accumulators
    // or floating reassociation. Run before verification and frame budgeting.
    if (small->getInstructionCount() <= 128) {
      DominatorTree dom(*small);
      LoopInfo loops(dom);
      AssumptionCache ac(*small);
      TargetLibraryInfoImpl impl(module.getTargetTriple());
      TargetLibraryInfo tli(impl);
      ScalarEvolution se(*small, tli, ac, dom, loops);
      TargetTransformInfo tti(module.getDataLayout());
      for (auto *loop : loops.getLoopsInPreorder()) {
        unsigned size = 0, fp = 0;
        bool pure = loop->getSubLoops().empty();
        for (auto *block : loop->blocks())
          for (auto &i : *block) {
            ++size;
            fp += i.getOpcode() == Instruction::FAdd || i.getOpcode() == Instruction::FMul;
            if (!i.isTerminator() &&
                (i.mayHaveSideEffects() || i.mayReadFromMemory() || isa<CallBase>(i)))
              pure = false;
          }
        if (pure && size <= 12 && fp > 0 && fp <= 2) {
          formLCSSARecursively(*loop, dom, &loops, &se);
          UnrollLoopOptions options{4, true, true, false, true, false, nullptr, 128};
          UnrollLoop(loop, options, &loops, &se, &dom, &ac, &tti, nullptr, true);
          break;
        }
      }
    }
    if (verifyFunction(*small)) {
      small->eraseFromParent();
      zero->eraseFromParent();
      if (one)
        one->eraseFromParent();
      continue;
    }
    simplify(*small, true);
    auto speculatable = [](llvm::Function &fn) {
      if (fn.size() != 1)
        return false;
      for (auto &i : fn.getEntryBlock())
        if (!isa<ReturnInst>(i) &&
            (i.mayHaveSideEffects() || i.mayReadFromMemory() || !isSafeToSpeculativelyExecute(&i)))
          return false;
      return true;
    };
    const bool combine = one && speculatable(*zero) && speculatable(*one);
    auto *wrapper = llvm::Function::Create(original->getFunctionType(), original->getLinkage(),
                                           original->getName() + ".range-dispatch", module);
    wrapper->setAttributes(original->getAttributes());
    wrapper->setCallingConv(original->getCallingConv());
    IRBuilder<> b(BasicBlock::Create(ctx, "ordinary.zero-range", wrapper));
    auto *empty = BasicBlock::Create(ctx, "ordinary.zero", wrapper),
         *guard = BasicBlock::Create(ctx, "ordinary.range", wrapper),
         *fast = BasicBlock::Create(ctx, "ordinary.small", wrapper),
         *wide = BasicBlock::Create(ctx, "ordinary.full", wrapper);
    auto *singleGuard =
        !combine && one ? BasicBlock::Create(ctx, "ordinary.single-guard", wrapper) : guard;
    if (combine)
      b.CreateCondBr(b.CreateICmpULT(wrapper->getArg(count), b.getInt64(2)), empty, guard,
                     MDBuilder(ctx).createBranchWeights(2, 1));
    else
      b.CreateCondBr(b.CreateICmpEQ(wrapper->getArg(count), b.getInt64(0)), empty, singleGuard,
                     MDBuilder(ctx).createBranchWeights(1, 2));
    SmallVector<Value *, 8> args;
    for (auto &arg : wrapper->args())
      args.push_back(&arg);
    SmallVector<std::pair<BasicBlock *, llvm::Function *>, 3> bodies;
    if (combine) {
      // Both source base cases are pure and safe to speculate under LLVM's
      // floating environment. Select their original result bits, without a
      // second dispatch branch or any input-range restriction.
      b.SetInsertPoint(empty);
      auto *z = b.CreateCall(zero, args);
      auto *o = b.CreateCall(one, args);
      auto *value = b.CreateSelect(b.CreateICmpEQ(wrapper->getArg(count), b.getInt64(0)), z, o);
      cast<Instruction>(value)->setMetadata(LLVMContext::MD_unpredictable, MDNode::get(ctx, {}));
      b.CreateRet(value);
      InlineFunctionInfo zi, oi;
      if (!InlineFunction(*z, zi).isSuccess() || !InlineFunction(*o, oi).isSuccess())
        llvm_unreachable("pure base cases must inline");
      zero->eraseFromParent();
      one->eraseFromParent();
      bodies.emplace_back(fast, small);
    } else {
      auto *single = one ? BasicBlock::Create(ctx, "ordinary.single-step", wrapper) : nullptr;
      if (one) {
        b.SetInsertPoint(singleGuard);
        b.CreateCondBr(b.CreateICmpEQ(wrapper->getArg(count), b.getInt64(1)), single, guard,
                       MDBuilder(ctx).createBranchWeights(16, 1));
      }
      bodies.emplace_back(empty, zero);
      bodies.emplace_back(fast, small);
      if (one)
        bodies.emplace_back(single, one);
    }
    for (auto [block, callee] : bodies) {
      b.SetInsertPoint(block);
      auto *call = b.CreateCall(callee, args);
      call->setAttributes(callee->getAttributes().removeFnAttributes(ctx));
      call->setCallingConv(callee->getCallingConv());
      b.CreateRet(call);
      InlineFunctionInfo info;
      if (!InlineFunction(*call, info).isSuccess())
        llvm_unreachable("scalar range body must inline");
      if (callee->use_empty())
        callee->eraseFromParent();
    }
    b.SetInsertPoint(guard);
    Value *bits = b.getInt64(0);
    for (auto &arg : wrapper->args())
      if (arg.getType()->isIntegerTy(64))
        bits = b.CreateOr(bits, &arg);
    b.CreateCondBr(b.CreateICmpULT(bits, b.getInt64(65536)), fast, wide,
                   MDBuilder(ctx).createBranchWeights(2, 1));
    b.SetInsertPoint(wide);
    auto *wideCounted = ordinaryWideCountedBody(*original, count, simplify);
    if (wideCounted) {
      auto *counted = BasicBlock::Create(ctx, "ordinary.wide-counted", wrapper);
      auto *full = BasicBlock::Create(ctx, "ordinary.unrestricted", wrapper);
      b.CreateCondBr(
          b.CreateICmpULT(b.CreateSub(wrapper->getArg(count), b.getInt64(2)), b.getInt64(65534)),
          counted, full);
      b.SetInsertPoint(counted);
      auto *call = b.CreateCall(wideCounted, args);
      call->setCallingConv(wideCounted->getCallingConv());
      call->setAttributes(wideCounted->getAttributes().removeFnAttributes(ctx));
      call->setTailCallKind(CallInst::TCK_MustTail);
      b.CreateRet(call);
      b.SetInsertPoint(full);
    }
    auto *tail = b.CreateCall(original, args);
    tail->setCallingConv(original->getCallingConv());
    tail->setAttributes(original->getAttributes().removeFnAttributes(ctx));
    tail->setTailCallKind(CallInst::TCK_MustTail);
    b.CreateRet(tail);
    simplify(*wrapper, true);
    if (verifyFunction(*wrapper) || nativeStackBudgetError(*wrapper, module.getDataLayout())) {
      wrapper->eraseFromParent();
      if (wideCounted)
        wideCounted->eraseFromParent();
      continue;
    }
    auto name = original->getName().str();
    original->setName(name + ".full-range");
    original->setLinkage(GlobalValue::InternalLinkage);
    original->removeFnAttr("kalandra.ordinary.entry");
    original->addFnAttr(Attribute::NoInline);
    wrapper->setName(name);
    wrapper->addFnAttr("kalandra.ordinary.scalar-ranges");
    if (wideCounted)
      wrapper->addFnAttr("kalandra.ordinary.wide-counted");
    changed = true;
  }
  return changed;
}
} // namespace kalandra
