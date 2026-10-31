#include "NativeLayout.h"
#include "NativeStack.h"
#include "NativeCallFoundation.h"
#include "NativeFastABI.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kalandra {
namespace {
// Keep cancellation calls outside short, vectorizable counted loops. The inner
// loop executes at most 256 iterations; the outer loop polls before each chunk.
bool chunkCheckedLoop(llvm::Loop &loop, llvm::Function &function, llvm::FunctionCallee status,
                      llvm::Value *invocation, llvm::BasicBlock *failed) {
  using namespace llvm;
  if (!loop.getSubLoops().empty() || loop.getNumBlocks() != 2)
    return false;
  auto *header = loop.getHeader();
  auto *preheader = loop.getLoopPreheader();
  auto *latch = loop.getLoopLatch();
  auto *branch = dyn_cast<CondBrInst>(header->getTerminator());
  if (!preheader || !latch || !branch || !loop.contains(branch->getSuccessor(0)) ||
      loop.contains(branch->getSuccessor(1)))
    return false;
  auto *condition = dyn_cast<ICmpInst>(branch->getCondition());
  if (!condition || (condition->getPredicate() != ICmpInst::ICMP_SLT &&
                     condition->getPredicate() != ICmpInst::ICMP_ULT))
    return false;
  auto *iv = dyn_cast<PHINode>(condition->getOperand(0));
  auto *bound = condition->getOperand(1);
  if (!iv || iv->getParent() != header || iv->getNumIncomingValues() != 2 ||
      !iv->getType()->isIntegerTy(64) || !loop.isLoopInvariant(bound))
    return false;
  for (auto &phi : header->phis())
    if (&phi != iv)
      return false;
  auto *start = dyn_cast<ConstantInt>(iv->getIncomingValueForBlock(preheader));
  auto *step = dyn_cast<BinaryOperator>(iv->getIncomingValueForBlock(latch));
  if (!start || !start->isZero() || !step || step->getOpcode() != Instruction::Add ||
      step->getOperand(0) != iv || !isa<ConstantInt>(step->getOperand(1)) ||
      !cast<ConstantInt>(step->getOperand(1))->isOne())
    return false;
  // Re-entering the header at a chunk boundary must not duplicate observable
  // accesses. Irregular/calling loops retain their original polling scheme.
  for (auto &instruction : *header)
    if (instruction.mayReadOrWriteMemory())
      return false;
  for (auto *block : loop.blocks())
    for (auto &instruction : *block) {
      if (isa<CallBase>(instruction))
        return false;
      if (auto *load = dyn_cast<LoadInst>(&instruction); load && !load->isSimple())
        return false;
      if (auto *store = dyn_cast<StoreInst>(&instruction); store && !store->isSimple())
        return false;
    }
  auto &context = function.getContext();
  auto *exit = branch->getSuccessor(1);
  auto *chunks = BasicBlock::Create(context, "loop.chunks", &function, header);
  auto *complete = BasicBlock::Create(context, "loop.chunk.complete", &function, exit);
  IRBuilder<> outer(chunks);
  auto *first = outer.CreatePHI(iv->getType(), 2, "chunk.start");
  first->addIncoming(start, preheader);
  auto *remaining = outer.CreateSub(bound, first);
  auto *small =
      outer.CreateICmp(condition->getPredicate(), remaining, ConstantInt::get(iv->getType(), 256));
  auto *length = outer.CreateSelect(small, remaining, ConstantInt::get(iv->getType(), 256));
  auto *end = outer.CreateAdd(first, length, "chunk.end");
  auto *code = outer.CreateCall(status, {invocation});
  code->setMetadata("kalandra.checkpoint", MDNode::get(context, {}));
  outer.CreateCondBr(outer.CreateICmpNE(code, ConstantInt::get(code->getType(), 0)), failed,
                     header);
  preheader->getTerminator()->replaceSuccessorWith(header, chunks);
  iv->setIncomingBlock(iv->getBasicBlockIndex(preheader), chunks);
  iv->setIncomingValueForBlock(chunks, first);
  condition->setOperand(1, end);
  branch->setSuccessor(1, complete);
  IRBuilder<> next(complete);
  next.CreateCondBr(next.CreateICmp(condition->getPredicate(), end, bound), chunks, exit);
  first->addIncoming(end, complete);
  for (auto &phi : exit->phis())
    phi.replaceIncomingBlockWith(header, complete);
  return true;
}
void simplifyCheckedStackIR(llvm::Function &function, bool prune = false) {
  using namespace llvm;
  // Simplify only after checked allocation/fault/poll calls are installed, so
  // optimizations cannot assume an unchecked fault path continues to execute.
  LoopAnalysisManager loopAnalyses;
  FunctionAnalysisManager functionAnalyses;
  CGSCCAnalysisManager callAnalyses;
  ModuleAnalysisManager moduleAnalyses;
  PassBuilder passes;
  passes.registerLoopAnalyses(loopAnalyses);
  passes.registerFunctionAnalyses(functionAnalyses);
  passes.registerCGSCCAnalyses(callAnalyses);
  passes.registerModuleAnalyses(moduleAnalyses);
  passes.crossRegisterProxies(loopAnalyses, functionAnalyses, callAnalyses, moduleAnalyses);
  FunctionPassManager simplify;
  simplify.addPass(InstCombinePass());
  simplify.addPass(EarlyCSEPass());
  simplify.addPass(DCEPass());
  if (prune) {
    simplify.addPass(SimplifyCFGPass());
    simplify.addPass(InstCombinePass());
    simplify.addPass(DCEPass());
  }
  simplify.run(function, functionAnalyses);
}
// Tail position is a property of control flow, independent of scalar types or
// direct self recursion. Reject frame-local storage: outgoing references must
// not outlive a frame that is about to be reused.
bool forwardedTailReturn(llvm::CallInst &call) {
  using namespace llvm;
  auto &fn = *call.getFunction();
  for (auto &block : fn)
    for (auto &instruction : block)
      if (isa<AllocaInst>(instruction))
        return false;
  SmallPtrSet<BasicBlock *, 16> visited;
  SmallPtrSet<Instruction *, 16> allowed;
  SmallVector<Value *, 8> values{&call};
  Value *value = &call;
  auto *block = call.getParent();
  auto at = std::next(call.getIterator());
  visited.insert(block);
  while (at != block->end()) {
    auto *instruction = &*at++;
    if (auto *ret = dyn_cast<ReturnInst>(instruction)) {
      if (ret->getReturnValue() != (call.getType()->isVoidTy() ? nullptr : value))
        return false;
      allowed.insert(ret);
      for (auto *forwarded : values)
        for (auto *user : forwarded->users())
          if (!allowed.contains(dyn_cast<Instruction>(user)))
            return false;
      return true;
    }
    auto *branch = dyn_cast<UncondBrInst>(instruction);
    if (!branch)
      return false;
    auto *previous = block;
    block = branch->getSuccessor(0);
    if (!visited.insert(block).second)
      return false;
    for (auto &phi : block->phis()) {
      allowed.insert(&phi);
      if (phi.getIncomingValueForBlock(previous) == value) {
        value = &phi;
        values.push_back(value);
      }
    }
    at = block->getFirstNonPHIIt();
  }
  return false;
}
} // namespace
std::optional<std::string> addCheckedInvocationABI(llvm::Module &module, bool functionValues) {
  using namespace llvm;
  // Structural simplification must use the same target layout as storage and
  // stack admission, including i64/i128 alignment on the pinned ARM64 target.
  if (module.getDataLayoutStr().empty()) module.setDataLayout(nativeCpuDataLayout);
  if (module.getTargetTriple().str().empty())
    module.setTargetTriple(llvm::Triple(KALANDRA_NATIVE_CPU_TARGET_TRIPLE));
  auto &context = module.getContext();
  auto *pointer = PointerType::getUnqual(context);
  auto *i64 = Type::getInt64Ty(context);
  auto *voidType = Type::getVoidTy(context);
  auto allocate = module.getOrInsertFunction("kalandra_alloc_v1", pointer, pointer, i64);
  auto release = module.getOrInsertFunction("kalandra_free_v1", voidType, pointer, pointer);
  auto fault = module.getOrInsertFunction("kalandra_fault_v1", voidType, pointer, i64, i64, i64);
  auto status = module.getOrInsertFunction("kalandra_poll_v1", i64, pointer);
  module.getOrInsertFunction("kalandra_call_enter_v1", Type::getInt1Ty(context), pointer, i64, i64);
  module.getOrInsertFunction("kalandra_call_leave_v1", voidType, pointer);
  auto abort = module.getOrInsertFunction("kalandra_source_abort_v2", voidType);
  cast<Function>(abort.getCallee())->addFnAttr(Attribute::NoReturn);
  cast<Function>(abort.getCallee())->addFnAttr(Attribute::Cold);
  auto boundary = module.getOrInsertFunction("kalandra_source_run_v4",
      Type::getInt1Ty(context), pointer, pointer, pointer);
  cast<Function>(boundary.getCallee())->addFnAttr(Attribute::NoUnwind);
  std::vector<Function *> originals;
  for (auto &function : module)
    if (!function.isDeclaration())
      originals.push_back(&function);
  std::unordered_map<Function *, Function *> checked;
  // Prove total, bounded, context-free subgraphs bottom-up. Recursive cycles,
  // loops, memory operations and runtime effects cannot enter this set.
  SmallPtrSet<Function *, 16> statusFree;
  std::unordered_map<Function *, unsigned> boundedWork;
  bool changed;
  do {
    changed = false;
    for (auto *fn : originals) {
      if (statusFree.contains(fn))
        continue;
      DominatorTree dominators(*fn);
      LoopInfo loops(dominators);
      bool total = loops.empty();
      unsigned work = 0;
      for (auto &block : *fn)
        for (auto &instruction : block) {
          work = std::min(257U, work + 1);
          if (auto *call = dyn_cast<CallInst>(&instruction)) {
            auto *callee = call->getCalledFunction();
            if (callee && boundedWork.contains(callee))
              work = std::min(257U, work + boundedWork.at(callee));
            total &= callee && (statusFree.contains(callee) ||
                        (callee->isIntrinsic() && !call->mayHaveSideEffects() &&
                         !call->mayReadOrWriteMemory()));
          } else
            total &= !instruction.mayReadOrWriteMemory() && !isa<AllocaInst>(instruction);
        }
      if (total && work <= 256) {
        statusFree.insert(fn);
        boundedWork.emplace(fn, work);
        changed = true;
      }
    }
  } while (changed);
  SmallPtrSet<CallInst *, 16> tailReturns;
  for (auto *fn : originals)
    for (auto &block : *fn)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction);
            call && (!call->getCalledFunction() || !call->getCalledFunction()->isDeclaration()) &&
            forwardedTailReturn(*call))
          tailReturns.insert(call);
  std::unordered_map<Function *, Function *> entries;
  for (auto *old : originals) {
    auto name = old->getName().str();
    old->setName(name + ".source");
    std::vector<Type *> parameters(old->getFunctionType()->params().begin(),
                                   old->getFunctionType()->params().end());
    parameters.push_back(pointer);
    auto *entryType = FunctionType::get(old->getReturnType(), parameters, false);
    const bool callable = name.starts_with("kalandra_callable_");
    if (!callable)
      parameters.insert(parameters.begin(), pointer); // Uniform environment slot.
    parameters.push_back(pointer);
    parameters.push_back(i64);
    const bool indirectResult = old->getReturnType()->isAggregateType();
    if (indirectResult)
      parameters.insert(parameters.begin(), pointer);
    auto *function = Function::Create(FunctionType::get(indirectResult ? voidType : old->getReturnType(), parameters, false),
                                      GlobalValue::InternalLinkage, name + ".body", module);
    SmallVector<AttributeSet, 12> parameterAttributes(parameters.size());
    for (unsigned n = 0; n != old->arg_size(); ++n)
      parameterAttributes[n + (callable ? 0 : 1) + unsigned(indirectResult)] =
          old->getAttributes().getParamAttrs(n);
    auto returnAttributes = old->getAttributes().getRetAttrs()
                                .removeAttribute(context, Attribute::SExt)
                                .removeAttribute(context, Attribute::ZExt);
    function->setAttributes(AttributeList::get(context, old->getAttributes().getFnAttrs(),
        indirectResult ? AttributeSet{} : returnAttributes, parameterAttributes));
    function->setCallingConv(nativeInternalCallingConvention(function->getFunctionType(), module.getTargetTriple()));
    function->removeFnAttr(Attribute::NoUnwind);
    function->setUWTableKind(UWTableKind::Sync);
    function->addFnAttr("kalandra.effects", statusFree.contains(old) ? "bounded-total" : "checked");
    function->addFnAttr("kalandra.native.pure-frame");
    function->splice(function->begin(), old);
    for (auto &argument : old->args())
      argument.replaceAllUsesWith(function->getArg(argument.getArgNo() + (callable ? 0 : 1) + unsigned(indirectResult)));
    function->getArg(function->arg_size() - 3)->setName("invocation");
    function->getArg(function->arg_size() - 2)->setName("cancellation");
    function->getArg(function->arg_size() - 1)->setName("call.mode");
    checked.emplace(old, function);
    auto *entry = Function::Create(entryType, old->getLinkage(), name, module);
    entry->setAttributes(old->getAttributes());
    entry->setCallingConv(old->getCallingConv());
    entry->addFnAttr(Attribute::NoInline);
    entries.emplace(old, entry);
  }
  // Function values cross worker/transport boundaries using stable checked
  // entries. Resolve their code to this module's internal ABI at invocation;
  // stored values never capture an execution mode or a private worker address.
  std::unordered_map<FunctionType *, Function *> resolvers;
  auto resolver = [&](FunctionType *type) {
    if (auto found = resolvers.find(type); found != resolvers.end())
      return found->second;
    SmallVector<Type *, 12> internalParameters(type->params());
    internalParameters.append({pointer, pointer, i64});
    const bool indirectResult = type->getReturnType()->isAggregateType();
    if (indirectResult)
      internalParameters.insert(internalParameters.begin(), pointer);
    auto *internalType = FunctionType::get(indirectResult ? voidType : type->getReturnType(), internalParameters, false);
    SmallVector<std::pair<Function *, Function *>, 8> targets;
    SmallVector<Metadata *, 16> registry;
    for (auto *old : originals)
      if (old->getFunctionType() == type && checked.at(old)->getFunctionType() == internalType) {
        targets.emplace_back(entries.at(old), checked.at(old));
        registry.push_back(ValueAsMetadata::get(entries.at(old)));
        registry.push_back(ValueAsMetadata::get(checked.at(old)));
      }
    auto *fn = Function::Create(FunctionType::get(pointer, {pointer}, false),
                                GlobalValue::InternalLinkage, "kalandra_call_resolve", module);
    fn->addFnAttr(Attribute::AlwaysInline);
    auto *signature = Function::Create(type, GlobalValue::ExternalLinkage,
                                       "kalandra_call_signature", module);
    populateNativeCallResolver(*fn, targets);
    fn->setMetadata("kalandra.call.signature", MDNode::get(context, ValueAsMetadata::get(signature)));
    fn->setMetadata("kalandra.call.targets", MDNode::get(context, registry));
    resolvers.emplace(type, fn);
    return fn;
  };
  for (auto *old : originals) {
    auto *function = checked.at(old);
    auto *invocation = function->getArg(function->arg_size() - 3);
    auto *failed = BasicBlock::Create(context, "invocation.failed", function);
    IRBuilder<> failure(failed);
    failure.CreateCall(abort);
    failure.CreateUnreachable();
    const bool returnsAggregate = old->getReturnType()->isAggregateType();
    // Result packets are copied into SSA immediately after each synchronous
    // call. The private ABI cannot retain the packet address, so calls of the
    // same result type share scratch storage without overlapping lifetimes.
    std::unordered_map<Type *, AllocaInst *> resultSlots;
    std::vector<CallInst *> calls;
    for (auto &block : *function)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction);
            call && call->getParent() != failed)
          calls.push_back(call);
    for (auto *call : calls) {
      auto *target = call->getCalledFunction();
      const bool internal = !target || checked.contains(target);
      const bool tail = tailReturns.contains(call);
      const bool aggregate = internal && call->getType()->isAggregateType();
      IRBuilder<> builder(call);
      Value *resultSlot = nullptr;
      if (aggregate) {
        if (tail)
          resultSlot = function->getArg(0);
        else {
          auto &slot = resultSlots[call->getType()];
          if (!slot) {
            IRBuilder<> entry(&*function->getEntryBlock().getFirstInsertionPt());
            slot = entry.CreateAlloca(call->getType(), nullptr, "call.result");
          }
          resultSlot = slot;
        }
      }
      std::vector<Value *> arguments(call->args().begin(), call->args().end());
      FunctionCallee callee;
      bool alwaysFail = false;
      if (!target) {
        if (!functionValues || call->getFunctionType()->isVarArg())
          return "indirect call has no checked invocation convention";
        std::vector<Type *> parameters(call->getFunctionType()->param_begin(),
                                       call->getFunctionType()->param_end());
        parameters.push_back(pointer);
        parameters.push_back(pointer);
        parameters.push_back(i64);
        auto *resultType = aggregate ? voidType : call->getType();
        if (aggregate)
          parameters.insert(parameters.begin(), pointer);
        auto *resolved = builder.CreateCall(resolver(call->getFunctionType()),
                                             {call->getCalledOperand()});
        auto *before = call->getParent();
        auto *ready = before->splitBasicBlock(call->getIterator(), "call.resolved");
        before->getTerminator()->eraseFromParent();
        auto *invalid = BasicBlock::Create(context, "call.invalid", function);
        builder.SetInsertPoint(invalid);
        builder.CreateCall(fault, {invocation, builder.getInt64(3), builder.getInt64(0),
                                   builder.getInt64(0)});
        builder.CreateBr(failed);
        builder.SetInsertPoint(before);
        builder.CreateCondBr(builder.CreateIsNotNull(resolved), ready, invalid);
        builder.SetInsertPoint(call);
        callee = FunctionCallee(FunctionType::get(resultType, parameters, false), resolved);
        arguments.push_back(invocation);
        arguments.push_back(function->getArg(function->arg_size() - 2));
        arguments.push_back(function->getArg(function->arg_size() - 1));
      } else if (auto found = checked.find(target); found != checked.end()) {
        callee = found->second;
        if (!target->getName().starts_with("kalandra_callable_"))
          arguments.insert(arguments.begin(), ConstantPointerNull::get(pointer));
        arguments.push_back(invocation);
        arguments.push_back(function->getArg(function->arg_size() - 2));
        arguments.push_back(function->getArg(function->arg_size() - 1));
      } else if (target->getName() == "_mlir_memref_to_llvm_alloc") {
        callee = allocate;
        arguments.insert(arguments.begin(), invocation);
      } else if (target->getName() == "_mlir_memref_to_llvm_free") {
        callee = release;
        arguments.insert(arguments.begin(), invocation);
      } else if (target->getName() == "kalandra_fault") {
        callee = fault;
        arguments.insert(arguments.begin(), invocation);
        alwaysFail = true;
      } else if (target->getName() == "kalandra_record_alloc" ||
                 target->getName() == "kalandra_recursive_make" ||
                 target->getName() == "kalandra_recursive_read" ||
                 target->getName() == "kalandra_recursive_transfer" ||
                 target->getName() == "kalandra_recursive_environment_bind" ||
                 target->getName() == "kalandra_recursive_environment_read" ||
                 target->getName() == "kalandra_recursive_function_dependency" ||
                 target->getName() == "kalandra_recursive_function_array_dependency" ||
                 target->getName() == "kalandra_recursive_record_array_dependency" ||
                 target->getName() == "kalandra_recursive_reference_dependency" ||
                 target->getName() == "kalandra_recursive_tag_check" ||
                 target->getName() == "kalandra_recursive_string_dependency" ||
                 target->getName() == "kalandra_recursive_string_array_dependency" ||
                 target->getName() == "kalandra_recursive_tensor_dependency" ||
                 target->getName() == "kalandra_recursive_bool_dependency" ||
                 target->getName() == "kalandra_environment_alloc" ||
                 target->getName() == "kalandra_record_cleanup" ||
                 target->getName() == "kalandra_record_validate" ||
                 target->getName() == "kalandra_record_array_validate" ||
                 target->getName() == "kalandra_string_literal" ||
                 target->getName() == "kalandra_string_validate" ||
                 target->getName() == "kalandra_string_array_validate" ||
                 target->getName() == "kalandra_string_concat" ||
                 target->getName() == "kalandra_string_equal" ||
                 target->getName() == "kalandra_string_length" ||
                 target->getName() == "kalandra_string_byte_length" ||
                 target->getName() == "kalandra_string_index" ||
                 target->getName() == "kalandra_string_slice") {
        std::vector<Type *> parameters{pointer};
        parameters.insert(parameters.end(), target->getFunctionType()->param_begin(),
                          target->getFunctionType()->param_end());
        callee = module.getOrInsertFunction(
            target->getName().str() + "_v1",
            FunctionType::get(target->getReturnType(), parameters, false));
        arguments.insert(arguments.begin(), invocation);
      } else if (target->isIntrinsic())
        continue;
      else
        return "external call has no checked invocation convention: " + target->getName().str();
      if (aggregate)
        arguments.insert(arguments.begin(), resultSlot);
      auto *replacement = builder.CreateCall(callee, arguments);
      replacement->setCallingConv(internal ? nativeInternalCallingConvention(callee.getFunctionType(), module.getTargetTriple()) : call->getCallingConv());
      if (!internal)
        replacement->addFnAttr(Attribute::NoUnwind);
      if (!call->getType()->isVoidTy()) {
        Value *value = aggregate ? (tail ? static_cast<Value *>(UndefValue::get(call->getType())) :
                                   builder.CreateLoad(call->getType(), resultSlot)) :
                                   static_cast<Value *>(replacement);
        call->replaceAllUsesWith(value);
      }
      call->eraseFromParent();
      if (tail) {
        auto *block = replacement->getParent();
        block->splitBasicBlock(std::next(replacement->getIterator()), "tail.unreachable");
        block->getTerminator()->eraseFromParent();
        IRBuilder<> finish(block);
        // tailcc supports differing parameter lists. Aggregate result storage
        // is explicitly forwarded, never automatically converted to sret.
        if (replacement->getType() == function->getReturnType() &&
            replacement->getCallingConv() == function->getCallingConv() &&
            (function->getCallingConv() == CallingConv::Tail ||
             replacement->getFunctionType() == function->getFunctionType()))
          replacement->setTailCallKind(CallInst::TCK_MustTail);
        else
          replacement->setTailCallKind(CallInst::TCK_Tail);
        if (function->getReturnType()->isVoidTy())
          finish.CreateRetVoid();
        else
          finish.CreateRet(replacement);
        continue;
      }
      if (internal)
        continue; // Native unwinding propagates rare failure, with no hot-path protocol.
      auto *block = replacement->getParent();
      auto *continuation =
          block->splitBasicBlock(std::next(replacement->getIterator()), "invocation.continue");
      block->getTerminator()->eraseFromParent();
      builder.SetInsertPoint(block);
      if (alwaysFail)
        builder.CreateBr(failed);
      else {
        auto *code = builder.CreateCall(status, {invocation});
        builder.CreateCondBr(builder.CreateICmpNE(code, ConstantInt::get(i64, 0)), failed,
                             continuation);
      }
    }
    if (returnsAggregate) {
      SmallVector<ReturnInst *, 8> returns;
      for (auto &block : *function)
        if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator()); ret && ret->getReturnValue())
          returns.push_back(ret);
      for (auto *ret : returns) {
        IRBuilder<> finish(ret);
        finish.CreateStore(ret->getReturnValue(), function->getArg(0));
        finish.CreateRetVoid();
        ret->eraseFromParent();
      }
    }
    // Poll every generated loop header, including bufferization/copy loops.
    // Cancellation exits through the checked ABI; the invocation ledger owns
    // any live buffers until its task acknowledges and settles the request.
    DominatorTree dominators(*function);
    LoopInfo loops(dominators);
    std::vector<Loop *> pending(loops.begin(), loops.end());
    std::vector<Loop *> headers;
    while (!pending.empty()) {
      auto *loop = pending.back();
      pending.pop_back();
      headers.push_back(loop);
      pending.insert(pending.end(), loop->begin(), loop->end());
    }
    for (auto *loop : headers) {
      if (chunkCheckedLoop(*loop, *function, status, invocation, failed))
        continue;
      auto *header = loop->getHeader();
      IRBuilder<> counter(header, header->getFirstInsertionPt());
      auto *tick = counter.CreatePHI(i64, pred_size(header), "poll.tick");
      auto *nextTick = counter.CreateAdd(tick, ConstantInt::get(i64, 1), "poll.next");
      for (auto *predecessor : predecessors(header))
        tick->addIncoming(loop->contains(predecessor) ? nextTick : ConstantInt::get(i64, 0),
                          predecessor);
      auto *body = header->splitBasicBlock(header->getFirstInsertionPt(), "loop.body");
      header->getTerminator()->eraseFromParent();
      auto *pollBlock = BasicBlock::Create(context, "loop.poll", function);
      IRBuilder<> gate(header);
      // Bound response latency without a runtime call for every scalar element.
      auto *due = gate.CreateICmpEQ(gate.CreateAnd(tick, ConstantInt::get(i64, 255)),
                                    ConstantInt::get(i64, 0));
      gate.CreateCondBr(due, pollBlock, body);
      IRBuilder<> poll(pollBlock);
      auto *code = poll.CreateCall(status, {invocation});
      code->setMetadata("kalandra.checkpoint", MDNode::get(context, {}));
      poll.CreateCondBr(poll.CreateICmpNE(code, ConstantInt::get(i64, 0)), failed, body);
    }
  }
  // Keep stored code identities compatible with the worker/transport ABI.
  // Direct calls and resolved indirect calls already target internal bodies.
  for (auto *old : originals)
    old->replaceAllUsesWith(entries.at(old));
  SmallVector<Function *, 16> bodies;
  for (auto *old : originals) {
    auto *body = checked.at(old);
    bodies.push_back(body);
    auto *entry = entries.at(old);
    // A portable C++ catch boundary owns admission and catches only the private
    // source-abort marker. Argument/result storage stays live above that frame.
    SmallVector<Type *, 12> fields;
    const bool hasResult = !entry->getReturnType()->isVoidTy();
    if (hasResult)
      fields.push_back(entry->getReturnType());
    fields.append(entry->getFunctionType()->param_begin(), entry->getFunctionType()->param_end());
    auto *frameType = StructType::get(context, fields);
    auto *callback = Function::Create(FunctionType::get(Type::getInt1Ty(context), {pointer}, false),
        GlobalValue::InternalLinkage, entry->getName() + ".boundary", module);
    callback->setUWTableKind(UWTableKind::Sync);
    callback->addRetAttr(Attribute::ZExt);
    callback->addFnAttr("kalandra.boundary");
    IRBuilder<> cb(BasicBlock::Create(context, "entry", callback));
    SmallVector<Value *, 12> args;
    const bool indirectResult = entry->getReturnType()->isAggregateType();
    if (indirectResult)
      args.push_back(cb.CreateStructGEP(frameType, callback->getArg(0), 0));
    if (!entry->getName().starts_with("kalandra_callable_"))
      args.push_back(ConstantPointerNull::get(pointer));
    for (unsigned n = 0; n != entry->arg_size(); ++n)
      args.push_back(cb.CreateLoad(entry->getArg(n)->getType(),
          cb.CreateStructGEP(frameType, callback->getArg(0), n + unsigned(hasResult))));
    auto *invocation = args.back();
    auto field = [&](std::size_t offset, Type *type) {
      return cb.CreateLoad(type, cb.CreateConstGEP1_64(cb.getInt8Ty(), invocation, offset));
    };
    args.push_back(field(runtime::InvocationABI::cancellation, pointer));
    args.push_back(cb.CreateZExt(field(runtime::InvocationABI::pollMode, cb.getInt8Ty()), i64));
    auto *result = cb.CreateCall(body, args);
    result->setCallingConv(body->getCallingConv());
    if (hasResult && !indirectResult)
      cb.CreateStore(result, cb.CreateStructGEP(frameType, callback->getArg(0), 0));
    cb.CreateRet(cb.getTrue());
    entry->addFnAttr(Attribute::NoUnwind);
    IRBuilder<> b(BasicBlock::Create(context, "entry", entry));
    auto *frame = b.CreateAlloca(frameType);
    if (hasResult)
      b.CreateStore(Constant::getNullValue(entry->getReturnType()),
                    b.CreateStructGEP(frameType, frame, 0));
    for (unsigned n = 0; n != entry->arg_size(); ++n)
      b.CreateStore(entry->getArg(n), b.CreateStructGEP(frameType, frame, n + unsigned(hasResult)));
    auto *complete = b.CreateCall(boundary,
        {entry->getArg(entry->arg_size() - 1), callback, frame});
    if (hasResult)
      b.CreateRet(b.CreateSelect(complete,
          b.CreateLoad(entry->getReturnType(), b.CreateStructGEP(frameType, frame, 0)),
          Constant::getNullValue(entry->getReturnType())));
    else
      b.CreateRetVoid();
    simplifyCheckedStackIR(*body, true);
    const bool numericLeaf = body->getName().starts_with("kalandra_fn_matmul_wide_");
    if (auto error = nativeStackBudgetError(*body, module.getDataLayout(), numericLeaf))
      return error;
    bool nonLeaf = false;
    for (auto &block : *body)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction)) {
          auto *target = call->getCalledFunction();
          nonLeaf |= !target || (!target->isIntrinsic() && target != fault.getCallee());
        }
    // Check cancellation at recursive/calling progress points, independently
    // of physical stack growth. A bounded total DAG needs no extra checkpoint.
    if (nonLeaf && !statusFree.contains(old)) {
      auto *start = &body->getEntryBlock();
      auto *guard = BasicBlock::Create(context, "checkpoint.entry", body, start);
      auto *stopped = BasicBlock::Create(context, "checkpoint.denied", body);
      auto *bound = BasicBlock::Create(context, "checkpoint.token", body);
      auto *observed = BasicBlock::Create(context, "checkpoint.observed", body);
      IRBuilder<> at(guard);
      auto *ctx = body->getArg(body->arg_size() - 3);
      auto *token = body->getArg(body->arg_size() - 2);
      auto *dispatch = at.CreateSwitch(body->getArg(body->arg_size() - 1), observed, 2);
      dispatch->addCase(at.getInt64(0), start);
      dispatch->addCase(at.getInt64(1), bound);
      at.SetInsertPoint(bound);
      auto *cancelled = at.CreateLoad(at.getInt8Ty(), token);
      cancelled->setAtomic(AtomicOrdering::Acquire);
      cancelled->setAlignment(Align(1));
      at.CreateCondBr(at.CreateICmpEQ(cancelled, at.getInt8(0)), start, stopped);
      at.SetInsertPoint(observed);
      auto *code = at.CreateCall(status, {ctx});
      at.CreateCondBr(at.CreateICmpEQ(code, at.getInt64(0)), start, stopped);
      at.SetInsertPoint(stopped);
      at.CreateCall(abort);
      at.CreateUnreachable();
    }
    // A leaf path cannot accumulate native frames. Guard actual stack-growing
    // source calls instead of forcing every base case through a full prologue.
    // Guaranteed tail calls reuse the caller's frame and need no new admission.
    SmallVector<CallInst *, 8> growing;
    for (auto &block : *body)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction); call && !call->isMustTailCall()) {
          auto *target = call->getCalledFunction();
          if (!target || llvm::is_contained(bodies, target) ||
              (target && target->getName().ends_with(".body")))
            growing.push_back(call);
        }
    if (!growing.empty()) {
      bool fixedFrame = true;
      for (auto &block : *body)
        for (auto &instruction : block)
          fixedFrame &= !isa<AllocaInst>(instruction);
      AllocaInst *admitted = nullptr;
      if (fixedFrame && growing.size() > 1) {
        IRBuilder<> init(&*body->getEntryBlock().getFirstInsertionPt());
        admitted = init.CreateAlloca(init.getInt1Ty(), nullptr, "physical.admitted");
        init.CreateStore(init.getFalse(), admitted);
      }
      auto *ctx = body->getArg(body->arg_size() - 3);
      auto *exhausted = BasicBlock::Create(context, "physical.exhausted", body);
      IRBuilder<> at(exhausted);
      at.CreateCall(fault, {ctx, at.getInt64(6), at.getInt64(0), at.getInt64(0)});
      at.CreateCall(abort);
      at.CreateUnreachable();
      for (auto *call : growing) {
        auto *before = call->getParent();
        auto *next = before->splitBasicBlock(call->getIterator(), "physical.call");
        before->getTerminator()->eraseFromParent();
        at.SetInsertPoint(before);
        BasicBlock *accepted = next;
        if (admitted) {
          auto *check = BasicBlock::Create(context, "physical.check", body, next);
          accepted = BasicBlock::Create(context, "physical.accepted", body, next);
          at.CreateCondBr(at.CreateLoad(at.getInt1Ty(), admitted), next, check);
          at.SetInsertPoint(accepted);
          at.CreateStore(at.getTrue(), admitted);
          at.CreateBr(next);
          at.SetInsertPoint(check);
        }
        auto load = [&](std::size_t offset) {
          return at.CreateLoad(i64, at.CreateConstGEP1_64(at.getInt8Ty(), ctx, offset));
        };
        auto *low = load(runtime::InvocationABI::stackLow);
        auto *sp = nativeStackPointer(at);
        // Entry checked the executing thread's full bounds. A synchronous
        // source chain cannot migrate or switch stacks; only downward growth
        // can exhaust it. Fixed frames keep admission across returned calls.
        auto *guard = at.CreateCondBr(at.CreateICmpUGT(sp, low), accepted, exhausted);
        guard->setMetadata("kalandra.physical.guard", MDNode::get(context, {}));
      }
      if (admitted) {
        DominatorTree dominators(*body);
        PromoteMemToReg({admitted}, dominators);
      }
    }
    simplifyCheckedStackIR(*body, true);
    old->eraseFromParent();
  }
  for (const auto *name : {"kalandra_fault",
                           "_mlir_memref_to_llvm_alloc",
                           "_mlir_memref_to_llvm_free",
                           "kalandra_record_alloc",
                           "kalandra_recursive_make",
                           "kalandra_recursive_read",
                           "kalandra_recursive_transfer",
                           "kalandra_recursive_environment_bind",
                           "kalandra_recursive_environment_read",
                           "kalandra_recursive_function_dependency",
                           "kalandra_recursive_function_array_dependency",
                           "kalandra_recursive_record_array_dependency",
                           "kalandra_recursive_reference_dependency",
                           "kalandra_recursive_tag_check",
                           "kalandra_recursive_string_dependency",
                           "kalandra_recursive_string_array_dependency",
                           "kalandra_recursive_tensor_dependency",
                           "kalandra_recursive_bool_dependency",
                           "kalandra_environment_alloc",
                           "kalandra_record_cleanup",
                           "kalandra_record_validate",
                           "kalandra_record_array_validate",
                           "kalandra_string_literal",
                           "kalandra_string_validate",
                           "kalandra_string_array_validate",
                           "kalandra_string_concat",
                           "kalandra_string_equal",
                           "kalandra_string_length",
                           "kalandra_string_byte_length",
                           "kalandra_string_index",
                           "kalandra_string_slice"})
    if (auto *old = module.getFunction(name); old && old->use_empty())
      old->eraseFromParent();
  specializeInternalCallModes(module, bodies, simplifyCheckedStackIR);
  defineNativeFastABI(module);
  // Runtime helpers are noexcept; source workers and callbacks may unwind to
  // the portable boundary. Emit unwind records on all native source frames.
  for (auto &function : module) {
    if (function.isDeclaration() && &function != abort.getCallee())
      function.addFnAttr(Attribute::NoUnwind);
    if (!function.isDeclaration())
      function.setUWTableKind(UWTableKind::Sync);
    if (!function.isDeclaration())
      function.addFnAttr("frame-pointer", "all");
  }
  cast<Function>(fault.getCallee())->addFnAttr(Attribute::Cold);
  return std::nullopt;
}
} // namespace kalandra
