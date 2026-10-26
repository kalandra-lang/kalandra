#pragma once
#include "../runtime/Runtime.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
// A bounded leaf needs no recoverable native stack: no calls, allocations or
// recursive frames can be abandoned. The checked host entry polls before and
// after it. Unknown work, watched contexts and nested entries retain v4.
inline bool addBoundedEntries(llvm::Module &module,
                             const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  using ABI = runtime::InvocationABI;
  if (!module.getTargetTriple().isAArch64() &&
      module.getTargetTriple().getArch() != Triple::x86_64) return false;
  SmallVector<CallInst *, 16> entries;
  for (auto &fn : module)
    if (!fn.isDeclaration() && !fn.hasFnAttribute("kalandra.task.frame") &&
        (fn.getReturnType()->isIntegerTy() || fn.getReturnType()->isFloatingPointTy()))
      for (auto &b : fn) for (auto &i : b)
        if (auto *call = dyn_cast<CallInst>(&i); call && call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "kalandra_source_run_v4") entries.push_back(call);
  bool changed = false;
  for (auto *entry : entries) {
    auto *original = dyn_cast<llvm::Function>(entry->getArgOperand(1));
    if (!original || !original->hasFnAttribute("kalandra.boundary")) continue;
    std::int64_t packetOffset = 0;
    auto *packet = GetPointerBaseWithConstantOffset(entry->getArgOperand(2), packetOffset,
                                                   module.getDataLayout());
    if (!isa<AllocaInst>(packet)) continue;
    DominatorTree entryDom(*entry->getFunction());
    DenseMap<std::int64_t, unsigned> initializedScalars;
    std::optional<std::int64_t> contextOffset;
    for (auto &b : *entry->getFunction()) for (auto &i : b)
      if (auto *store = dyn_cast<StoreInst>(&i);
          store && store->isSimple() && entryDom.dominates(store, entry)) {
        std::int64_t offset = 0;
        if (GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset,
                                            module.getDataLayout()) == packet) {
          if (store->getValueOperand() == entry->getArgOperand(0))
            contextOffset = offset - packetOffset;
          if (auto *type = dyn_cast<IntegerType>(store->getValueOperand()->getType()))
            initializedScalars[offset-packetOffset] = type->getBitWidth();
        }
      }
    if (!contextOffset) continue;
    ValueToValueMapTy mapping;
    auto *body = CloneFunction(original, mapping);
    body->setName(original->getName() + ".bounded");
    body->setSection("");
    body->removeFnAttr("kalandra.boundary");
    body->removeFnAttr("kalandra.native.pure-frame");
    body->addFnAttr("kalandra.bounded.entry");
    bool viable = true;
    for (unsigned round = 0; round < 12; ++round) {
      SmallVector<LoadInst *, 8> modes;
      for (auto &b : *body) for (auto &i : b)
        if (auto *load = dyn_cast<LoadInst>(&i); load && load->getType()->isIntegerTy(8)) {
          std::int64_t offset = 0;
          auto *base = GetPointerBaseWithConstantOffset(load->getPointerOperand(), offset,
                                                        module.getDataLayout());
          auto *ctx = dyn_cast<LoadInst>(base);
          std::int64_t position = 0;
          if (offset == std::int64_t(ABI::pollMode) && load->isSimple() && ctx &&
              ctx->isSimple() && ctx->getType()->isPointerTy() &&
              GetPointerBaseWithConstantOffset(ctx->getPointerOperand(), position,
                  module.getDataLayout()) == body->getArg(0) && position == *contextOffset)
            modes.push_back(load);
        }
      for (auto *load : modes) {
        load->replaceAllUsesWith(ConstantInt::get(load->getType(), 0)); load->eraseFromParent();
      }
      simplify(*body, true);
      SmallVector<CallInst *, 8> calls;
      unsigned instructions = 0;
      for (auto &b : *body) for (auto &i : b) {
        ++instructions;
        if (auto *call = dyn_cast<CallInst>(&i); call && call->getCalledFunction() &&
            !call->getCalledFunction()->isDeclaration() &&
            call->getCalledFunction()->getName().contains(".body.clear")) calls.push_back(call);
      }
      if (instructions > 2048) { viable = false; break; }
      if (calls.empty()) break;
      for (auto *call : calls) {
        unsigned size = 0;
        for (auto &block : *call->getCalledFunction()) size += block.size();
        if (instructions + size > 2048) { viable = false; break; }
        instructions += size;
        InlineFunctionInfo info;
        if (!InlineFunction(*call, info).isSuccess()) { viable = false; break; }
      }
      if (!viable) break;
    }
    SmallVector<CallInst *, 8> aborts, faults;
    for (auto &b : *body) for (auto &i : b) {
      if (isa<AllocaInst>(i) || isa<InvokeInst>(i) || isa<IndirectBrInst>(i) ||
          i.isAtomic()) viable = false;
      if (auto *load = dyn_cast<LoadInst>(&i); load && !load->isSimple()) viable = false;
      if (auto *store = dyn_cast<StoreInst>(&i); store && !store->isSimple()) viable = false;
      if (auto *store = dyn_cast<StoreInst>(&i)) {
        std::int64_t offset = 0;
        auto *base = GetPointerBaseWithConstantOffset(store->getPointerOperand(),offset,
                                                      module.getDataLayout());
        if (base != body->getArg(0)) {
          auto *ctx = dyn_cast<LoadInst>(base); std::int64_t position = 0;
          if (!ctx || !ctx->getType()->isPointerTy() ||
              GetPointerBaseWithConstantOffset(ctx->getPointerOperand(),position,
                  module.getDataLayout()) != body->getArg(0) || position != *contextOffset)
            viable = false;
        }
      }
      if (i.getOpcode() == Instruction::UDiv || i.getOpcode() == Instruction::SDiv ||
          i.getOpcode() == Instruction::URem || i.getOpcode() == Instruction::SRem)
        if (i.getType()->isIntegerTy() && i.getType()->getIntegerBitWidth() > 64) viable = false;
      if (auto *call = dyn_cast<CallBase>(&i)) {
        auto *target = call->getCalledFunction();
        auto *plain = dyn_cast<CallInst>(call);
        if (!target || !plain) { viable = false; continue; }
        if (target->getName() == "kalandra_source_abort_v2") {
          if (!isa<UnreachableInst>(plain->getParent()->getTerminator())) viable = false;
          aborts.push_back(plain);
        }
        else if (target->getName() == "kalandra_fault_v1") faults.push_back(plain);
        else if (target->isIntrinsic()) {
          switch (target->getIntrinsicID()) {
          case Intrinsic::lifetime_start: case Intrinsic::lifetime_end:
          case Intrinsic::assume: case Intrinsic::expect: case Intrinsic::expect_with_probability:
          case Intrinsic::smax: case Intrinsic::smin: case Intrinsic::umax: case Intrinsic::umin:
          case Intrinsic::abs: case Intrinsic::bswap: case Intrinsic::bitreverse:
          case Intrinsic::ctpop: case Intrinsic::ctlz: case Intrinsic::cttz: break;
          default: viable = false;
          }
        } else viable = false;
      }
    }
    if (!viable) { body->eraseFromParent(); continue; }
    // Version small scalar packets, then narrow division only when LLVM proves
    // both operands and the result have the narrower operation's semantics.
    // The host admission guard below establishes every load range attached here.
    SmallVector<std::int64_t, 8> smallInputs;
    for (auto &block : *body) for (auto &instruction : block)
      if (auto *load = dyn_cast<LoadInst>(&instruction);
          load && load->isSimple() && load->getType()->isIntegerTy(64)) {
        std::int64_t offset = 0;
        if (GetPointerBaseWithConstantOffset(load->getPointerOperand(),offset,
              module.getDataLayout()) != body->getArg(0) ||
            initializedScalars.lookup(offset) != 64) continue;
        bool unchanged = true;
        for (auto &b : *body) for (auto &i : b)
          if (auto *store = dyn_cast<StoreInst>(&i)) {
            std::int64_t position = 0;
            if (GetPointerBaseWithConstantOffset(store->getPointerOperand(),position,
                  module.getDataLayout()) == body->getArg(0)) {
              auto bytes = module.getDataLayout().getTypeStoreSize(store->getValueOperand()->getType());
              if (position < offset+8 && offset < position+std::int64_t(bytes)) unchanged = false;
            }
          }
        if (!unchanged) continue;
        load->setMetadata(LLVMContext::MD_range,MDBuilder(module.getContext()).createRange(
            APInt(64,0), APInt(64,65536)));
        if (!llvm::is_contained(smallInputs,offset)) smallInputs.push_back(offset);
      }
    if (smallInputs.size() > 8) { body->eraseFromParent(); continue; }
    if (!smallInputs.empty()) {
      simplify(*body,true);
      {
        FunctionAnalysisManager facts; PassBuilder builder; builder.registerFunctionAnalyses(facts);
        auto &lvi = facts.getResult<LazyValueAnalysis>(*body);
        struct PositiveRemainder { PHINode *phi; BinaryOperator *remainder; std::uint64_t modulus; };
        SmallVector<PositiveRemainder,4> positive;
        for (auto &block : *body) for (auto &instruction : block) {
          auto *phi = dyn_cast<PHINode>(&instruction);
          if (!phi || !phi->getType()->isIntegerTy(64)) continue;
          BinaryOperator *remainder = nullptr; std::uint64_t modulus = 0;
          bool safe = true; unsigned bases = 0;
          for (unsigned n = 0; n < phi->getNumIncomingValues(); ++n) {
            Value *value = phi->getIncomingValue(n);
            if (auto *extend = dyn_cast<SExtInst>(value)) value = extend->getOperand(0);
            auto *op = dyn_cast<BinaryOperator>(value);
            auto *constant = op && op->getOpcode() == Instruction::SRem
                ? dyn_cast<ConstantInt>(op->getOperand(1)) : nullptr;
            if (constant && !constant->isNegative() && !constant->isZero()) {
              if (remainder) { safe = false; break; }
              remainder = op; modulus = constant->getZExtValue();
            }
          }
          if (!remainder) continue;
          for (unsigned n = 0; n < phi->getNumIncomingValues(); ++n) {
            Value *value = phi->getIncomingValue(n);
            if (auto *extend = dyn_cast<SExtInst>(value)) value = extend->getOperand(0);
            if (value == remainder) continue;
            auto range = lvi.getConstantRange(phi->getIncomingValue(n),
                phi->getIncomingBlock(n)->getTerminator(),false);
            safe &= !range.isEmptySet() && !range.getSignedMin().isNegative() &&
                    range.getUnsignedMax().ult(APInt(64,modulus));
            ++bases;
          }
          std::function<ConstantRange(Value *, unsigned)> evaluate = [&](Value *value, unsigned depth) {
            if (value == phi) return ConstantRange(APInt(64,0),APInt(64,modulus));
            if (depth < 8) {
              if (auto *op = dyn_cast<BinaryOperator>(value)) {
                if (op->getOpcode() == Instruction::Add)
                  return evaluate(op->getOperand(0),depth+1).add(evaluate(op->getOperand(1),depth+1));
                if (op->getOpcode() == Instruction::Mul)
                  return evaluate(op->getOperand(0),depth+1).multiply(evaluate(op->getOperand(1),depth+1));
              }
              if (auto *cast = dyn_cast<CastInst>(value);
                  cast && (isa<TruncInst>(cast) || isa<SExtInst>(cast) || isa<ZExtInst>(cast))) {
                auto input = evaluate(cast->getOperand(0),depth+1);
                auto width = cast->getType()->getIntegerBitWidth();
                if (isa<TruncInst>(cast)) return input.truncate(width);
                if (isa<SExtInst>(cast)) return input.signExtend(width);
                if (isa<ZExtInst>(cast)) return input.zeroExtend(width);
              }
            }
            return lvi.getConstantRange(value,remainder,false);
          };
          auto numerator = evaluate(remainder->getOperand(0),0);
          if (safe && bases && !numerator.isEmptySet() && !numerator.getSignedMin().isNegative())
            positive.push_back({phi,remainder,modulus});
        }
        facts.clear();
        SmallPtrSet<BinaryOperator *,4> rewritten;
        for (auto proof : positive) {
          IRBuilder<> b(&*proof.phi->getParent()->getFirstInsertionPt());
          b.CreateAssumption(b.CreateICmpULT(proof.phi,ConstantInt::get(proof.phi->getType(),proof.modulus)));
          if (!rewritten.insert(proof.remainder).second) continue;
          b.SetInsertPoint(proof.remainder);
          auto *value = b.CreateURem(proof.remainder->getOperand(0),proof.remainder->getOperand(1));
          proof.remainder->replaceAllUsesWith(value); proof.remainder->eraseFromParent();
        }
      }
      simplify(*body,true);
      FunctionAnalysisManager analyses; PassBuilder passes; passes.registerFunctionAnalyses(analyses);
      auto &ranges = analyses.getResult<LazyValueAnalysis>(*body);
      SmallVector<BinaryOperator *, 8> narrow;
      DenseMap<BinaryOperator *,unsigned> widths;
      for (auto &block : *body) for (auto &instruction : block)
        if (auto *op = dyn_cast<BinaryOperator>(&instruction);
            op && op->getType()->isIntegerTy(64) &&
            (op->getOpcode() == Instruction::SRem || op->getOpcode() == Instruction::SDiv ||
             op->getOpcode() == Instruction::URem || op->getOpcode() == Instruction::UDiv)) {
          auto left = ranges.getConstantRange(op->getOperand(0),op,false);
          auto right = ranges.getConstantRange(op->getOperand(1),op,false);
          auto fits = [](const ConstantRange &range) {
            return !range.isEmptySet() && range.getSignedMin().sge(APInt(64,std::uint64_t(INT32_MIN))) &&
                   range.getSignedMax().sle(APInt(64,INT32_MAX));
          };
          bool isSigned = op->getOpcode() == Instruction::SRem || op->getOpcode() == Instruction::SDiv;
          bool safeUnsigned = !left.isEmptySet() && !right.isEmptySet() &&
              left.getUnsignedMax().ule(APInt(64,UINT32_MAX)) && right.getUnsignedMax().ule(APInt(64,UINT32_MAX));
          if ((!isSigned && safeUnsigned) || (isSigned && fits(left) && fits(right) &&
              !(left.contains(APInt(64,std::uint64_t(INT32_MIN))) && right.contains(APInt::getAllOnes(64))))) {
            narrow.push_back(op);
            widths[op] = isSigned ? 32 : std::max(8u,std::max(left.getUnsignedMax().getActiveBits(),
                                                            right.getUnsignedMax().getActiveBits()));
          }
        }
      analyses.clear();
      for (auto *op : narrow) {
        IRBuilder<> b(op);
        auto *type = b.getIntNTy(widths.lookup(op));
        auto *left = b.CreateTrunc(op->getOperand(0),type);
        auto *right = b.CreateTrunc(op->getOperand(1),type);
        auto *value = b.CreateBinOp(static_cast<Instruction::BinaryOps>(op->getOpcode()),left,right);
        const bool isSigned = op->getOpcode() == Instruction::SRem || op->getOpcode() == Instruction::SDiv;
        op->replaceAllUsesWith(isSigned ? b.CreateSExt(value,b.getInt64Ty()) : b.CreateZExt(value,b.getInt64Ty()));
        op->eraseFromParent();
      }
      simplify(*body,true);
      aborts.clear(); faults.clear();
      for (auto &block : *body) for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction); call && call->getCalledFunction()) {
          if (call->getCalledFunction()->getName() == "kalandra_source_abort_v2") aborts.push_back(call);
          if (call->getCalledFunction()->getName() == "kalandra_fault_v1") faults.push_back(call);
        }
    }
    // Recording a fault materializes control on the exceptional path. Do not
    // inspect a cold invocation's intentionally uninitialized fault fields.
    for (auto *call : aborts) {
      auto *block = call->getParent(); auto it = call->getIterator();
      while (it != block->end()) it = it->eraseFromParent();
      IRBuilder<> b(block); b.CreateRet(b.getFalse());
    }
    simplify(*body, true);
    auto *guard = llvm::Function::Create(body->getFunctionType(), GlobalValue::InternalLinkage,
                                   original->getName() + ".bounded.admit", module);
    guard->addFnAttr(Attribute::AlwaysInline);
    IRBuilder<> g(BasicBlock::Create(module.getContext(), "entry", guard));
    Value *small = g.getTrue();
    for (auto offset : smallInputs) {
      auto *address = g.CreateConstGEP1_64(g.getInt8Ty(),guard->getArg(0),offset);
      auto *value = g.CreateLoad(g.getInt64Ty(),address); value->setAlignment(Align(1));
      small = g.CreateAnd(small,g.CreateICmpULT(value,g.getInt64(65536)));
    }
    DenseMap<Value *, Value *> projected;
    projected[body->getArg(0)] = guard->getArg(0);
    unsigned projectionBudget = 128;
    std::function<Value *(Value *)> project = [&](Value *value) -> Value * {
      if (auto *constant = dyn_cast<Constant>(value)) return constant;
      if (auto found = projected.find(value); found != projected.end()) return found->second;
      if (!projectionBudget) return nullptr;
      --projectionBudget;
      Value *result = nullptr;
      if (auto *gep = dyn_cast<GetElementPtrInst>(value); gep && gep->hasAllConstantIndices()) {
        if (auto *base = project(gep->getPointerOperand())) {
          SmallVector<Value *> indices(gep->indices());
          result = g.CreateGEP(gep->getSourceElementType(), base, indices);
        }
      } else if (auto *load = dyn_cast<LoadInst>(value);
                 load && load->isSimple() && load->getType()->isIntegerTy()) {
        // Admission only reads scalar arguments in the compiler's own packet.
        std::int64_t offset = 0;
        auto *base = GetPointerBaseWithConstantOffset(load->getPointerOperand(), offset,
                                                     module.getDataLayout());
        bool immutable = true;
        for (auto &block : *body) for (auto &instruction : block)
          if (auto *store = dyn_cast<StoreInst>(&instruction)) {
            std::int64_t position = 0;
            if (GetPointerBaseWithConstantOffset(store->getPointerOperand(), position,
                  module.getDataLayout()) == body->getArg(0)) {
              const auto bytes = module.getDataLayout().getTypeStoreSize(store->getValueOperand()->getType());
              const auto loadedBytes = module.getDataLayout().getTypeStoreSize(load->getType());
              if (position < offset + std::int64_t(loadedBytes) &&
                  offset < position + std::int64_t(bytes)) immutable = false;
            }
          }
        if (base == body->getArg(0) && immutable &&
            initializedScalars.lookup(offset) == load->getType()->getIntegerBitWidth())
          if (auto *address = project(load->getPointerOperand())) {
            auto *copy = g.CreateLoad(load->getType(), address);
            copy->setAlignment(load->getAlign()); result = copy;
          }
      }
      if (result) projected[value] = result;
      return result;
    };
    {
      FunctionAnalysisManager analyses; PassBuilder passes; passes.registerFunctionAnalyses(analyses);
      auto &loops = analyses.getResult<LoopAnalysis>(*body);
      auto &dom = analyses.getResult<DominatorTreeAnalysis>(*body);
      auto &se = analyses.getResult<ScalarEvolutionAnalysis>(*body);
      Loop *loop = loops.empty() ? nullptr : *loops.begin();
      if (std::distance(loops.begin(),loops.end()) > 1 ||
          (loop && !loop->getSubLoops().empty())) viable = false;
      // Reject irreducible/nested cycles, even when LoopInfo has no natural loop.
      for (auto it = scc_begin(body); !it.isAtEnd(); ++it)
        if (it.hasCycle())
          for (auto *block : *it) if (!loop || !loop->contains(block)) viable = false;
      unsigned fixed = 0, work = 0;
      for (auto &b : *body) (loop && loop->contains(&b) ? work : fixed) += b.size();
      Value *admit = g.getTrue();
      if (fixed > 256 || work > 256) viable = false;
      if (loop && viable) {
        auto *latch = loop->getLoopLatch();
        if (!latch || loop->getNumBackEdges() != 1) viable = false;
        SmallVector<BasicBlock *> exits; loop->getExitingBlocks(exits);
        const SCEV *count = nullptr;
        for (auto *exit : exits)
          if (latch && dom.dominates(exit, latch)) {
            auto *candidate = se.getExitCount(loop, exit);
            if (!isa<SCEVCouldNotCompute>(candidate)) { count = candidate; break; }
          }
        // After removing its one latch->header edge the loop must be acyclic.
        DenseMap<BasicBlock *, unsigned> colors;
        std::function<bool(BasicBlock *)> acyclic = [&](BasicBlock *block) {
          if (colors.lookup(block) == 1) return false;
          if (colors.lookup(block) == 2) return true;
          colors[block] = 1;
          for (auto *next : successors(block)) {
            if (block == latch && next == loop->getHeader()) continue;
            if (loop->contains(next) && !acyclic(next)) return false;
          }
          colors[block] = 2; return true;
        };
        if (!count || !work || (256-fixed)/work < 1 || !acyclic(loop->getHeader())) viable = false;
        std::function<Value *(const SCEV *)> expand = [&](const SCEV *value) -> Value * {
          if (!projectionBudget) return nullptr;
          --projectionBudget;
          if (auto *constant = dyn_cast<SCEVConstant>(value)) return constant->getValue();
          if (auto *unknown = dyn_cast<SCEVUnknown>(value)) return project(unknown->getValue());
          if (auto *cast = dyn_cast<SCEVCastExpr>(value)) {
            auto *input = expand(cast->getOperand()); if (!input) return nullptr;
            if (isa<SCEVSignExtendExpr>(cast)) return g.CreateSExt(input, cast->getType());
            if (isa<SCEVZeroExtendExpr>(cast)) return g.CreateZExt(input, cast->getType());
            if (isa<SCEVTruncateExpr>(cast)) return g.CreateTrunc(input, cast->getType());
            return nullptr;
          }
          if (auto *op = dyn_cast<SCEVNAryExpr>(value)) {
            if (!isa<SCEVAddExpr>(op) && !isa<SCEVMulExpr>(op)) return nullptr;
            Value *result = nullptr;
            for (const SCEV *operand : op->operands()) {
              auto *next = expand(operand); if (!next) return nullptr;
              result = !result ? next : isa<SCEVAddExpr>(op) ? g.CreateAdd(result,next) : g.CreateMul(result,next);
            }
            return result;
          }
          return nullptr;
        };
        if (viable) {
          auto *value = expand(count);
          if (!value || !value->getType()->isIntegerTy() || value->getType()->getIntegerBitWidth()>64) viable = false;
          else admit = g.CreateICmpULE(value, ConstantInt::get(value->getType(), (256-fixed)/work-1));
        }
      }
      if (viable) g.CreateRet(g.CreateAnd(admit,small));
    }
    if (!viable) { guard->eraseFromParent(); body->eraseFromParent(); continue; }
    DenseMap<std::int64_t,StoreInst *> argumentStores;
    SmallVector<StoreInst *,16> packetStores;
    for (auto &block : *entry->getFunction()) for (auto &instruction : block)
      if (auto *store = dyn_cast<StoreInst>(&instruction); store && store->isSimple()) {
        std::int64_t offset = 0;
        if (GetPointerBaseWithConstantOffset(store->getPointerOperand(),offset,
              module.getDataLayout()) != packet || !entryDom.dominates(store,entry)) continue;
        packetStores.push_back(store);
      }
    for (auto *store : packetStores) {
      auto *type = store->getValueOperand()->getType();
      if (!type->isIntegerTy() && !type->isFloatingPointTy() && !type->isPointerTy()) continue;
      std::int64_t offset = 0;
      GetPointerBaseWithConstantOffset(store->getPointerOperand(),offset,module.getDataLayout());
      offset -= packetOffset;
      const auto bytes = std::int64_t(module.getDataLayout().getTypeStoreSize(type));
      bool immutable = true;
      for (auto *other : packetStores) if (other != store) {
        std::int64_t position = 0;
        GetPointerBaseWithConstantOffset(other->getPointerOperand(),position,module.getDataLayout());
        position -= packetOffset;
        auto size = std::int64_t(module.getDataLayout().getTypeStoreSize(other->getValueOperand()->getType()));
        if (position < offset+bytes && offset < position+size) immutable = false;
      }
      for (auto &block : *body) for (auto &instruction : block)
        if (auto *write = dyn_cast<StoreInst>(&instruction)) {
          std::int64_t position = 0;
          if (GetPointerBaseWithConstantOffset(write->getPointerOperand(),position,
                module.getDataLayout()) == body->getArg(0)) {
            auto size = std::int64_t(module.getDataLayout().getTypeStoreSize(write->getValueOperand()->getType()));
            if (position < offset+bytes && offset < position+size) immutable = false;
          }
        }
      if (immutable) argumentStores[offset] = store;
    }
    // v4 retains its packet only for the synchronous callback. Its argument
    // cannot escape when that callback also proves noncapturing. This keeps
    // atomic token polls from making private packet fields appear shared.
    if (!PointerMayBeCaptured(original->getArg(0), true, 256))
      entry->addParamAttr(2, Attribute::getWithCaptureInfo(module.getContext(), CaptureInfo::none()));
    body->addFnAttr(Attribute::NoUnwind);
    body->addFnAttr(Attribute::NoInline);
    auto *before = entry->getParent();
    auto *slow = before->splitBasicBlock(entry->getIterator(), "entry.slow");
    auto *after = slow->splitBasicBlock(std::next(entry->getIterator()), "entry.resume");
    auto *function = before->getParent();
    auto &c = module.getContext();
    auto make = [&](StringRef name) { return BasicBlock::Create(c,name,function,slow); };
    auto *check = make("entry.bounded.check"), *run = make("entry.bounded.run");
    auto *stopped = make("entry.bounded.stopped"), *exit = make("entry.bounded.exit");
    auto *publish = make("entry.bounded.publish");
    IRBuilder<> b(before->getTerminator());
    auto *ctx = entry->getArgOperand(0);
    auto field = [&](std::size_t offset) { return b.CreateConstGEP1_64(b.getInt8Ty(),ctx,offset); };
    auto *mode = b.CreateLoad(b.getInt8Ty(),field(ABI::pollMode));
    auto *control = b.CreateLoad(b.getInt32Ty(),field(ABI::control));
    auto *readyControl = b.CreateICmpNE(b.CreateAnd(control,b.getInt32(ABI::controlReady)),b.getInt32(0));
    auto *readDepth = make("entry.depth.read");
    auto *depthReady = make("entry.depth.ready");
    before->getTerminator()->eraseFromParent();
    b.SetInsertPoint(before); b.CreateCondBr(readyControl,readDepth,depthReady);
    b.SetInsertPoint(readDepth);
    auto *storedDepth = b.CreateLoad(b.getInt64Ty(),field(ABI::depth));
    b.CreateBr(depthReady);
    b.SetInsertPoint(depthReady);
    auto *depth = b.CreatePHI(b.getInt64Ty(),2);
    depth->addIncoming(b.getInt64(0),before); depth->addIncoming(storedDepth,readDepth);
    auto *bound = b.CreateCall(guard,{entry->getArgOperand(2)});
    auto *eligible = b.CreateAnd(b.CreateICmpULE(mode,b.getInt8(1)),b.CreateIsNull(depth));
    eligible = b.CreateAnd(eligible,bound);
    auto *dispatch = b.CreateCondBr(eligible,check,slow);
    b.SetInsertPoint(check);
    auto *boundToken = b.CreateICmpEQ(mode,b.getInt8(1));
    auto *token = b.CreateLoad(b.getPtrTy(),field(ABI::cancellation));
    auto *poll = make("entry.bounded.poll");
    b.CreateCondBr(boundToken,poll,run);
    auto tokenStopped = [&]() {
      auto *load = b.CreateLoad(b.getInt8Ty(),token);
      load->setAtomic(AtomicOrdering::Acquire); load->setAlignment(Align(1));
      return b.CreateIsNotNull(load);
    };
    b.SetInsertPoint(poll); b.CreateCondBr(tokenStopped(),stopped,run);
    b.SetInsertPoint(stopped); b.CreateBr(after);
    b.SetInsertPoint(run);
    auto *complete = b.CreateCall(body,{entry->getArgOperand(2)});
    b.CreateCondBr(b.CreateAnd(complete,boundToken),exit,publish);
    b.SetInsertPoint(exit); auto *ready = b.CreateNot(tokenStopped()); b.CreateBr(publish);
    b.SetInsertPoint(publish);
    auto *done = b.CreatePHI(b.getInt1Ty(),2);
    done->addIncoming(complete,run); done->addIncoming(ready,exit); b.CreateBr(after);
    b.SetInsertPoint(&*after->begin()); auto *joined = b.CreatePHI(b.getInt1Ty(),3);
    entry->replaceAllUsesWith(joined);
    joined->addIncoming(entry,slow); joined->addIncoming(b.getFalse(),stopped); joined->addIncoming(done,publish);
    InlineFunctionInfo info;
    const bool guardInlined = InlineFunction(*bound,info).isSuccess();
    InlineFunctionInfo bodyInfo;
    const bool bodyInlined = InlineFunction(*complete,bodyInfo).isSuccess();
    if (!guardInlined || !bodyInlined) dispatch->setCondition(b.getFalse());
    // These are immutable value arguments, not shared state to reload after an
    // acquire poll. Forward the caller's SSA values through the private packet.
    SmallVector<LoadInst *,16> argumentLoads;
    DominatorTree forwardedDom(*function);
    for (auto &block : *function) for (auto &instruction : block)
      if (auto *load = dyn_cast<LoadInst>(&instruction); load && load->isSimple()) {
        std::int64_t offset = 0;
        if (GetPointerBaseWithConstantOffset(load->getPointerOperand(),offset,
              module.getDataLayout()) == packet) {
          auto found = argumentStores.find(offset-packetOffset);
          if (found != argumentStores.end() &&
              forwardedDom.dominates(found->second,load) &&
              found->second->getValueOperand()->getType() == load->getType()) argumentLoads.push_back(load);
        }
      }
    for (auto *load : argumentLoads) {
      std::int64_t offset = 0;
      GetPointerBaseWithConstantOffset(load->getPointerOperand(),offset,module.getDataLayout());
      load->replaceAllUsesWith(argumentStores.lookup(offset-packetOffset)->getValueOperand());
      load->eraseFromParent();
    }
    simplify(*function,true);
    changed = true;
  }
  return changed;
}
} // namespace kalandra
