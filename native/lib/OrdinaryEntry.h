#pragma once
#include "../runtime/Runtime.h"
#include "NativeStack.h"
#include "OrdinaryPool.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
inline void emitOrdinaryResult(llvm::IRBuilder<> &b, llvm::Function &function, llvm::Value *value,
                               llvm::Value *code, llvm::Value *line, llvm::Value *column,
                               llvm::Value *fault) {
  using namespace llvm;
  auto *failed = b.CreateICmpNE(code, b.getInt64(0));
  Value *ambiguous;
  auto *type = value->getType();
  if (type->isFloatingPointTy()) {
    const unsigned width = type->getPrimitiveSizeInBits();
    auto *bits = b.CreateBitCast(value, b.getIntNTy(width));
    const auto exponent = width == 64 ? 0x7ff0000000000000ULL : 0x7f800000ULL;
    const auto fraction = width == 64 ? 0x000fffffffffffffULL : 0x007fffffULL;
    ambiguous = b.CreateAnd(
        b.CreateICmpEQ(b.CreateAnd(bits, exponent), ConstantInt::get(bits->getType(), exponent)),
        b.CreateICmpNE(b.CreateAnd(bits, fraction), ConstantInt::get(bits->getType(), 0)));
    value = b.CreateSelect(failed, ConstantFP::getQNaN(type), value);
  } else {
    auto *sentinel = ConstantInt::get(type, APInt::getSignMask(type->getIntegerBitWidth()));
    ambiguous = b.CreateICmpEQ(value, sentinel);
    value = b.CreateSelect(failed, sentinel, value);
  }
  auto *publish = BasicBlock::Create(function.getContext(), "ordinary.fault-info", &function);
  auto *done = BasicBlock::Create(function.getContext(), "ordinary.return", &function);
  b.CreateCondBr(b.CreateOr(failed, ambiguous), publish, done);

  b.SetInsertPoint(publish);
  b.CreateStore(code, fault);
  b.CreateStore(line, b.CreateConstGEP1_64(b.getInt64Ty(), fault, 1));
  b.CreateStore(column, b.CreateConstGEP1_64(b.getInt64Ty(), fault, 2));
  b.CreateBr(done);
  b.SetInsertPoint(done);
  b.CreateRet(value);
}

inline void OrdinaryPool::initialize() {
  using namespace llvm;
  if (active)
    return;
  active = true;
  auto &module = *entry.getParent();
  auto &ctx = module.getContext();
  IRBuilder<> b(&*entry.getEntryBlock().getFirstInsertionPt());
  storage = b.CreateAlloca(ArrayType::get(b.getInt8Ty(), slots * bytes));
  storage->setAlignment(Align(64));
  mask = b.CreateAlloca(b.getInt8Ty());
  b.CreateStore(b.getInt8(0), mask);
  enabled = b.CreateAlloca(b.getInt1Ty());
  b.CreateStore(b.getFalse(), enabled);
  heap = llvm::Function::Create(entry.getFunctionType(), GlobalValue::InternalLinkage,
                                original.getName() + "_ordinary_heap_v1", module);
  heap->setAttributes(entry.getAttributes().removeFnAttributes(ctx));
  heap->addFnAttr(Attribute::NoInline);
  heap->addFnAttr("kalandra.ordinary.fallback");
  IRBuilder<> h(BasicBlock::Create(ctx, "entry", heap));
  auto *context = h.CreateAlloca(ArrayType::get(h.getInt8Ty(), sizeof(runtime::Invocation)));
  context->setAlignment(Align(alignof(runtime::Invocation)));
  auto *info = h.CreateAlloca(ArrayType::get(h.getInt64Ty(), 3));
  auto begin = module.getOrInsertFunction("kalandra_ordinary_begin_v1",
                                          FunctionType::get(h.getVoidTy(), {h.getPtrTy()}, false));
  auto end = module.getOrInsertFunction(
      "kalandra_ordinary_end_v1",
      FunctionType::get(h.getVoidTy(), {h.getPtrTy(), h.getPtrTy()}, false));
  h.CreateCall(begin, {context});
  SmallVector<Value *, 8> args;
  for (unsigned i = 0; i + 1 < heap->arg_size(); ++i)
    args.push_back(heap->getArg(i));
  args.push_back(context);
  auto *value = h.CreateCall(&original, args);
  value->setCallingConv(original.getCallingConv());
  value->setAttributes(original.getAttributes().removeFnAttributes(ctx));
  h.CreateCall(end, {context, info});
  auto *code = h.CreateLoad(h.getInt64Ty(), info);
  auto *line = h.CreateLoad(h.getInt64Ty(), h.CreateConstGEP1_64(h.getInt64Ty(), info, 1));
  auto *column = h.CreateLoad(h.getInt64Ty(), h.CreateConstGEP1_64(h.getInt64Ty(), info, 2));
  emitOrdinaryResult(h, *heap, value, code, line, column, heap->getArg(heap->arg_size() - 1));
  replay = BasicBlock::Create(ctx, "ordinary.heap", &entry);
  IRBuilder<> r(replay);
  args.clear();
  for (auto &arg : entry.args())
    args.push_back(&arg);
  auto *result = r.CreateCall(heap, args);
  result->setAttributes(heap->getAttributes().removeFnAttributes(ctx));
  r.CreateRet(result);
}
inline void OrdinaryPool::allocate(llvm::CallInst *call) {
  using namespace llvm;
  initialize();
  auto &module = *entry.getParent();
  auto &ctx = module.getContext();
  auto *before = call->getParent();
  auto *after = before->splitBasicBlock(call->getIterator(), "local.allocated");
  before->getTerminator()->eraseFromParent();
  auto *probe = BasicBlock::Create(ctx, "local.enable", &entry, after);
  auto *ready = BasicBlock::Create(ctx, "local.ready", &entry, after);
  auto *reserve = BasicBlock::Create(ctx, "local.reserve", &entry, after);
  auto *take = BasicBlock::Create(ctx, "local.take", &entry, after);
  IRBuilder<> b(before);
  b.CreateCondBr(b.CreateLoad(b.getInt1Ty(), enabled), reserve, probe);
  b.SetInsertPoint(probe);
  auto permission = module.getOrInsertFunction("kalandra_ordinary_local_enabled_v1",
                                               FunctionType::get(b.getInt64Ty(), false));
  auto *allowed = b.CreateICmpNE(b.CreateCall(permission), b.getInt64(0));
  b.CreateCondBr(allowed, ready, replay);
  b.SetInsertPoint(ready);
  b.CreateStore(b.getTrue(), enabled);
  b.CreateBr(reserve);
  b.SetInsertPoint(reserve);
  auto *used = b.CreateLoad(b.getInt8Ty(), mask);
  auto *available = b.CreateAnd(b.CreateNot(used), b.getInt8((1U << slots) - 1));
  b.CreateCondBr(b.CreateICmpNE(available, b.getInt8(0)), take, replay);
  b.SetInsertPoint(take);
  auto count = Intrinsic::getOrInsertDeclaration(&module, Intrinsic::cttz, {b.getInt8Ty()});
  auto *index = b.CreateCall(count, {available, b.getTrue()});
  auto *bit = b.CreateShl(b.getInt8(1), index);
  b.CreateStore(b.CreateOr(used, bit), mask);
  auto *offset = b.CreateMul(b.CreateZExt(index, b.getInt64Ty()), b.getInt64(bytes));
  auto *pointer = b.CreateInBoundsGEP(b.getInt8Ty(), storage, offset);
  call->replaceAllUsesWith(pointer);
  call->eraseFromParent();
  b.CreateBr(after);
}
inline void OrdinaryPool::release(llvm::CallInst *call) {
  using namespace llvm;
  initialize();
  auto &ctx = entry.getContext();
  auto *pointer = call->getArgOperand(1);
  auto *before = call->getParent();
  auto *after = before->splitBasicBlock(call->getIterator(), "local.released");
  before->getTerminator()->eraseFromParent();
  IRBuilder<> b(before);
  Value *bit = b.getInt8(0);
  for (unsigned i = 0; i < slots; ++i) {
    auto *base = b.CreateConstGEP1_64(b.getInt8Ty(), storage, i * bytes);
    bit = b.CreateOr(
        bit, b.CreateSelect(b.CreateICmpEQ(pointer, base), b.getInt8(1U << i), b.getInt8(0)));
  }
  auto *used = b.CreateLoad(b.getInt8Ty(), mask);
  auto *valid =
      b.CreateAnd(b.CreateICmpNE(bit, b.getInt8(0)), b.CreateICmpEQ(b.CreateAnd(used, bit), bit));
  auto *clear = BasicBlock::Create(ctx, "local.free", &entry, after);
  b.CreateCondBr(b.CreateOr(valid, b.CreateIsNull(pointer)), clear, replay);
  b.SetInsertPoint(clear);
  b.CreateStore(b.CreateAnd(used, b.CreateNot(bit)), mask);
  b.CreateBr(after);
  call->eraseFromParent();
}
// Private ordinary scalar ABI: the last pointer addresses three i64 fault
// words, not an Invocation. No token or resource owner can be supplied here.
// The single high-bit integer pattern or any NaN (floats) requires consulting the fault word. On
// those successful values it is explicitly cleared; every other success leaves
// the output untouched. No numeric representation is reserved or canonicalized.
inline bool addOrdinaryScalarEntries(llvm::Module &module,
                                     const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  using ABI = runtime::InvocationABI;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module) {
    const auto name = fn.getName();
    if (name != "_mlir_ciface_kalandra_test_entry" && name != "_mlir_ciface_kalandra_entry" &&
        name != "_mlir_ciface_kalandra_float64_entry" &&
        name != "_mlir_ciface_kalandra_bool_entry" && name != "_mlir_ciface_kalandra_int32_entry" &&
        name != "_mlir_ciface_kalandra_int64_entry" &&
        name != "_mlir_ciface_kalandra_uint32_entry" &&
        name != "_mlir_ciface_kalandra_uint64_entry")
      continue;
    auto *ret = fn.getReturnType();
    if (fn.isDeclaration() || fn.isVarArg() || fn.hasFnAttribute("kalandra.task.frame") ||
        fn.hasFnAttribute("kalandra.boundary") || !fn.getSection().empty() ||
        fn.getCallingConv() != CallingConv::C || fn.arg_empty() ||
        !fn.getArg(fn.arg_size() - 1)->getType()->isPointerTy() ||
        !(ret->isIntegerTy() || ret->isFloatTy() || ret->isDoubleTy()))
      continue;
    bool scalar = true;
    for (unsigned i = 0; i + 1 < fn.arg_size(); ++i)
      scalar &=
          fn.getArg(i)->getType()->isIntegerTy() || fn.getArg(i)->getType()->isFloatingPointTy();
    if (scalar && !module.getFunction((name + "_ordinary_v1").str()))
      roots.push_back(&fn);
  }
  bool changed = false;
  for (auto *root : roots) {
    ValueToValueMapTy map;
    auto *ordinary = CloneFunction(root, map);
    ordinary->setName(root->getName() + "_ordinary_v1");
    ordinary->setLinkage(GlobalValue::ExternalLinkage);
    ordinary->removeFnAttr("kalandra.unbound.host-entry");
    ordinary->removeFnAttr(Attribute::AlwaysInline);
    ordinary->removeFnAttr(Attribute::OptimizeNone);
    // Failure changes the floating result to a NaN, so result refinements from
    // the checked zero-on-fault ABI (range/nofpclass) do not transfer. Nor do
    // its memory effects: a successful ambiguous result now defines the fault output.
    SmallVector<Attribute, 8> discard;
    for (auto attribute : ordinary->getAttributes().getRetAttrs())
      if (!attribute.hasAttribute(Attribute::SExt) && !attribute.hasAttribute(Attribute::ZExt) &&
          !attribute.hasAttribute(Attribute::InReg))
        discard.push_back(attribute);
    for (auto attribute : discard)
      ordinary->removeRetAttr(attribute.getKindAsEnum());
    ordinary->removeFnAttr(Attribute::Memory);
    ordinary->removeFnAttr(Attribute::Speculatable);
    auto *fault = ordinary->getArg(ordinary->arg_size() - 1);
    // Context argument attributes do not describe this output buffer.
    ordinary->setAttributes(
        ordinary->getAttributes().removeParamAttributes(module.getContext(), fault->getArgNo()));
    IRBuilder<> b(&*ordinary->getEntryBlock().getFirstInsertionPt());
    auto *context = b.CreateAlloca(ArrayType::get(b.getInt8Ty(), sizeof(runtime::Invocation)));
    fault->replaceAllUsesWith(context);
    b.CreateMemSet(context, b.getInt8(0), sizeof(runtime::Invocation),
                   MaybeAlign(alignof(runtime::Invocation)));
    auto at = [&](IRBuilder<> &builder, std::size_t offset) {
      return builder.CreateConstGEP1_64(builder.getInt8Ty(), context, offset);
    };
    b.CreateStore(b.getInt32(ABI::controlReady), at(b, ABI::control));
    auto emitReturn = [&](IRBuilder<> &b, Value *value) {
      auto *code = b.CreateLoad(b.getInt64Ty(), at(b, ABI::faultCode));
      auto *line = b.CreateLoad(b.getInt64Ty(), at(b, ABI::faultLine));
      auto *column = b.CreateLoad(b.getInt64Ty(), at(b, ABI::faultColumn));
      emitOrdinaryResult(b, *ordinary, value, code, line, column, fault);
    };
    SmallVector<ReturnInst *, 8> returns;
    for (auto &block : *ordinary)
      if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
        returns.push_back(ret);
    for (auto *ret : returns) {
      b.SetInsertPoint(ret);
      emitReturn(b, ret->getReturnValue());
      ret->eraseFromParent();
    }
    // Turn only this private fresh invocation's local fault calls into stores.
    // Remaining external calls/resource/stack operations reject the companion.
    OrdinaryPool pool(*ordinary, *root, fault);
    bool withinBudget = true;
    for (unsigned round = 0; round < 8; ++round) {
      unsigned size = 0;
      for (auto &block : *ordinary)
        size += block.size();
      if (size > 8192) {
        withinBudget = false;
        break;
      }
      SmallVector<WeakTrackingVH, 16> calls;
      for (auto &block : *ordinary)
        for (auto &i : block)
          if (auto *call = dyn_cast<CallInst>(&i); call && call->getCalledFunction())
            calls.push_back(call);
      for (auto &handle : calls) {
        auto *call = dyn_cast_or_null<CallInst>(handle);
        if (!call)
          continue;
        auto *callee = call->getCalledFunction();
        if (callee->getName() == "kalandra_poll_slow_v1") {
          // This invocation is private, unbound and unobserved. Unknown APIs
          // that could install a token/observer/state reject the final graph.
          IRBuilder<> c(call);
          auto *code = c.CreateLoad(
              c.getInt64Ty(),
              c.CreateConstGEP1_64(c.getInt8Ty(), call->getArgOperand(0), ABI::faultCode));
          call->replaceAllUsesWith(
              c.CreateSelect(c.CreateICmpNE(code, c.getInt64(0)), c.getInt64(-1), c.getInt64(0)));
          call->eraseFromParent();
        } else if (callee->getName() == "kalandra_record_cleanup_v1") {
          // No record/environment allocator or publication may survive the
          // final effect proof. Numeric pool buffers require no record ledger.
          call->eraseFromParent();
        } else if (callee->getName() == "kalandra_source_run_v4") {
          auto *callback = dyn_cast<llvm::Function>(call->getArgOperand(1)->stripPointerCasts());
          if (!callback || !callback->hasFnAttribute("kalandra.boundary"))
            continue;
          IRBuilder<> c(call);
          auto *direct = c.CreateCall(callback, {call->getArgOperand(2)});
          direct->setCallingConv(callback->getCallingConv());
          direct->setAttributes(callback->getAttributes().removeFnAttributes(module.getContext()));
          call->replaceAllUsesWith(direct);
          call->eraseFromParent();
          InlineFunctionInfo info;
          InlineFunction(*direct, info);
        } else if (callee->getName() == "kalandra_source_abort_v2" &&
                   isa<UnreachableInst>(call->getParent()->getTerminator())) {
          auto *block = call->getParent();
          auto cursor = call->getIterator();
          while (cursor != block->end())
            cursor = cursor->eraseFromParent();
          IRBuilder<> c(block);
          emitReturn(c, Constant::getNullValue(ordinary->getReturnType()));
        } else if (callee->getName() == "kalandra_fault_v1") {
          IRBuilder<> c(call);
          // The closed unbound proof already requires a local exit after fault.
          auto *destination =
              c.CreateConstGEP1_64(c.getInt8Ty(), call->getArgOperand(0), ABI::faultCode);
          auto *previous = c.CreateLoad(c.getInt64Ty(), destination);
          auto *first = c.CreateICmpEQ(previous, c.getInt64(0));
          auto *code = call->getArgOperand(1);
          auto *normalized =
              c.CreateSelect(c.CreateICmpEQ(code, c.getInt64(0)), c.getInt64(3), code);
          c.CreateStore(c.CreateSelect(first, normalized, previous), destination);
          for (auto [offset, index] :
               {std::pair{ABI::faultLine, 2U}, std::pair{ABI::faultColumn, 3U}}) {
            auto *slot = c.CreateConstGEP1_64(c.getInt8Ty(), call->getArgOperand(0), offset);
            c.CreateStore(c.CreateSelect(first, call->getArgOperand(index),
                                         c.CreateLoad(c.getInt64Ty(), slot)),
                          slot);
          }
          c.CreateStore(c.getInt8(2),
                        c.CreateConstGEP1_64(c.getInt8Ty(), call->getArgOperand(0), ABI::pollMode));
          call->eraseFromParent();
        } else if (!callee->isDeclaration() && !callee->hasFnAttribute("kalandra.task.frame") &&
                   !callee->hasFnAttribute("kalandra.ordinary.fallback")) {
          InlineFunctionInfo info;
          InlineFunction(*call, info);
        }
      }
      // These guards belonged to source calls being inlined into this private
      // ordinary clone. Acceptance still requires no growing source/foreign
      // call, a bounded static frame and the separately checked replay worker.
      SmallVector<CondBrInst *, 16> guards;
      for (auto &block : *ordinary)
        if (auto *branch = dyn_cast<CondBrInst>(block.getTerminator());
            branch && branch->getMetadata("kalandra.physical.guard"))
          if (auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
              cmp && cmp->getPredicate() == ICmpInst::ICMP_UGT)
            if (auto *sp = dyn_cast<CallInst>(cmp->getOperand(0));
                sp && sp->getCalledFunction() &&
                sp->getCalledFunction()->getIntrinsicID() == Intrinsic::read_volatile_register)
              guards.push_back(branch);
      for (auto *guard : guards) {
        auto *condition = cast<ICmpInst>(guard->getCondition());
        auto *sp = cast<CallInst>(condition->getOperand(0));
        auto *accepted = guard->getSuccessor(0);
        guard->getSuccessor(1)->removePredecessor(guard->getParent());
        UncondBrInst::Create(accepted, guard->getIterator());
        guard->eraseFromParent();
        if (condition->use_empty())
          condition->eraseFromParent();
        if (sp->use_empty())
          sp->eraseFromParent();
      }
      simplify(*ordinary, true);
    }
    // Finish graph inlining before introducing pool state. Promotion/simplify
    // can erase allocas, so no rewriter may retain their pointers across it.
    SmallVector<WeakTrackingVH, 16> allocationCalls;
    for (auto &block : *ordinary)
      for (auto &i : block)
        if (auto *call = dyn_cast<CallInst>(&i);
            call && call->getCalledFunction() &&
            (call->getCalledFunction()->getName() == "kalandra_alloc_v1" ||
             call->getCalledFunction()->getName() == "kalandra_free_v1"))
          allocationCalls.push_back(call);
    std::uint64_t largest = 0;
    bool constantAllocations = true;
    for (auto &handle : allocationCalls) {
      auto *call = dyn_cast_or_null<CallInst>(handle);
      if (!call || call->getCalledFunction()->getName() != "kalandra_alloc_v1")
        continue;
      auto *size = dyn_cast<ConstantInt>(call->getArgOperand(1));
      if (!size || size->getValue().getActiveBits() > 12 || size->isZero())
        constantAllocations = false;
      else
        largest = std::max(largest, size->getZExtValue());
    }
    if (constantAllocations && pool.configure(largest))
      for (auto &handle : allocationCalls) {
        auto *call = dyn_cast_or_null<CallInst>(handle);
        if (!call)
          continue;
        if (call->getCalledFunction()->getName() == "kalandra_free_v1")
          pool.release(call);
        else
          pool.allocate(call);
      }
    {
      LoopAnalysisManager loops;
      FunctionAnalysisManager functions;
      CGSCCAnalysisManager calls;
      ModuleAnalysisManager modules;
      PassBuilder passes;
      passes.registerLoopAnalyses(loops);
      passes.registerFunctionAnalyses(functions);
      passes.registerCGSCCAnalyses(calls);
      passes.registerModuleAnalyses(modules);
      passes.crossRegisterProxies(loops, functions, calls, modules);
      FunctionPassManager promote;
      promote.addPass(SROAPass(SROAOptions{}));
      promote.run(*ordinary, functions);
    }
    simplify(*ordinary, true);
    bool viable = withinBudget && !verifyFunction(*ordinary);
    // Cyclic loop phis are accepted only when every non-cyclic origin is a
    // private stack object. No caller pointer, global or loaded pointer roots it.
    std::function<bool(Value *, SmallPtrSetImpl<Value *> &)> privatePointer =
        [&](Value *value, SmallPtrSetImpl<Value *> &seen) -> bool {
      if (isa<AllocaInst>(value))
        return true;
      if (!seen.insert(value).second)
        return true;
      if (auto *gep = dyn_cast<GEPOperator>(value))
        return privatePointer(gep->getPointerOperand(), seen);
      if (auto *cast = dyn_cast<BitCastInst>(value))
        return privatePointer(cast->getOperand(0), seen);
      if (auto *phi = dyn_cast<PHINode>(value)) {
        for (auto &incoming : phi->incoming_values())
          if (!privatePointer(incoming, seen))
            return false;
        return true;
      }
      if (auto *select = dyn_cast<SelectInst>(value))
        return privatePointer(select->getTrueValue(), seen) &&
               privatePointer(select->getFalseValue(), seen);
      return false;
    };
    auto local = [&](Value *value) {
      SmallPtrSet<Value *, 16> seen;
      return privatePointer(value, seen);
    };
    auto faultRange = [&](Value *value, std::uint64_t bytes) {
      std::int64_t offset = 0;
      return GetPointerBaseWithConstantOffset(value, offset, module.getDataLayout()) == fault &&
             offset >= 0 && offset <= 24 && bytes <= 24 - std::uint64_t(offset);
    };
    std::uint64_t stackBytes = 0;
    for (auto &block : *ordinary)
      for (auto &i : block) {
        if (i.isAtomic() || isa<InvokeInst>(i) || isa<IndirectBrInst>(i) || isa<PtrToIntInst>(i) ||
            i.isEHPad() || isa<ResumeInst>(i))
          viable = false;
        if (auto *allocation = dyn_cast<AllocaInst>(&i)) {
          auto *count = dyn_cast<ConstantInt>(allocation->getArraySize());
          if (!pool.active || !allocation->isStaticAlloca() || !count ||
              count->getValue().getActiveBits() > 12)
            viable = false;
          else
            stackBytes += module.getDataLayout().getTypeAllocSize(allocation->getAllocatedType()) *
                          count->getZExtValue();
        }
        if (auto *load = dyn_cast<LoadInst>(&i))
          if (!load->isSimple() || !local(load->getPointerOperand()))
            viable = false;
        if (auto *store = dyn_cast<StoreInst>(&i)) {
          if (!store->isSimple() || (!local(store->getPointerOperand()) &&
                                     !(store->getValueOperand()->getType()->isIntegerTy(64) &&
                                       faultRange(store->getPointerOperand(), 8))))
            viable = false;
        }
        if (auto *clear = dyn_cast<MemSetInst>(&i)) {
          auto *length = dyn_cast<ConstantInt>(clear->getLength());
          if (clear->isVolatile() ||
              (!local(clear->getDest()) && !(length && length->getValue().getActiveBits() <= 5 &&
                                             faultRange(clear->getDest(), length->getZExtValue()))))
            viable = false;
        } else if (auto *copy = dyn_cast<MemCpyInst>(&i)) {
          if (copy->isVolatile() || !local(copy->getDest()) || !local(copy->getSource()))
            viable = false;
        } else if (auto *call = dyn_cast<CallBase>(&i)) {
          auto *f = call->getCalledFunction();
          if (f && pool.active &&
              (f == pool.heap || f->getName() == "kalandra_ordinary_local_enabled_v1"))
            continue;
          if (!f || !f->isIntrinsic() || call->mayReadOrWriteMemory() || call->mayHaveSideEffects())
            viable = false;
        }
      }
    viable &= stackBytes <= OrdinaryPool::stackBudget;
    viable &= !nativeStackBudgetError(*ordinary, module.getDataLayout()).has_value();
    if (!viable) {
      ordinary->eraseFromParent();
      if (pool.heap && pool.heap->use_empty())
        pool.heap->eraseFromParent();
      continue;
    }
    if (pool.active)
      ordinary->addFnAttr("kalandra.ordinary.local-buffers");
    ordinary->addFnAttr("kalandra.ordinary.entry");
    ordinary->addFnAttr("frame-pointer", "none");
    changed = true;
  }
  return changed;
}
} // namespace kalandra
