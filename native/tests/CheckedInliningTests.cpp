#include "../lib/NativeStack.h"
#include "../lib/NativeCallFoundation.h"
#include "../runtime/Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <stdexcept>
namespace kalandra {
std::optional<std::string> addCheckedInvocationABI(llvm::Module &, bool);
}
static void check(bool ok) {
  if (!ok) throw std::runtime_error("internal call convention regression");
}
static unsigned calls(llvm::Function &fn, llvm::StringRef target) {
  unsigned count = 0;
  for (auto &block : fn)
    for (auto &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        count += call->getCalledFunction() && call->getCalledFunction()->getName() == target;
  return count;
}
static std::unique_ptr<llvm::Module> windowsTailFixture(llvm::LLVMContext &context) {
  using namespace llvm;
  auto module = std::make_unique<Module>("windows-tail-packets", context);
  module->setTargetTriple(Triple("x86_64-pc-windows-msvc"));
  auto *i64 = Type::getInt64Ty(context);
  auto *ptr = PointerType::getUnqual(context);
  auto *shortType = FunctionType::get(i64, {ptr, i64, ptr, ptr, i64}, false);
  auto *wideType = FunctionType::get(i64, {ptr, i64, i64, i64, i64, i64, i64, i64, i64, i64,
                                         ptr, ptr, i64}, false);
  auto *a = Function::Create(shortType, GlobalValue::InternalLinkage, "windows.short", *module);
  auto *b = Function::Create(wideType, GlobalValue::InternalLinkage, "windows.wide", *module);
  auto *dispatch = Function::Create(FunctionType::get(i64, {ptr, ptr, i64, ptr, ptr, i64}, false),
                                     GlobalValue::InternalLinkage, "windows.indirect", *module);
  auto *floating = Function::Create(FunctionType::get(Type::getDoubleTy(context),
                      {Type::getDoubleTy(context), Type::getFloatTy(context), i64}, false),
                      GlobalValue::InternalLinkage, "windows.floating", *module);
  for (auto *fn : {a, b, dispatch, floating}) {
    fn->setCallingConv(CallingConv::Tail);
    fn->setUWTableKind(UWTableKind::Sync);
    fn->addFnAttr("frame-pointer", "all");
  }
  for (auto *fn : {a, b}) {
    IRBuilder<> ir(BasicBlock::Create(context, "entry", fn));
    auto *done = BasicBlock::Create(context, "done", fn);
    auto *next = BasicBlock::Create(context, "next", fn);
    ir.CreateCondBr(ir.CreateICmpEQ(fn->getArg(1), ir.getInt64(0)), done, next);
    ir.SetInsertPoint(done);
    ir.CreateRet(ir.getInt64(42));
    ir.SetInsertPoint(next);
    SmallVector<Value *, 16> args{fn->getArg(0), ir.CreateSub(fn->getArg(1), ir.getInt64(1))};
    if (fn == a) for (unsigned n = 0; n < 8; ++n) args.push_back(ir.getInt64(n));
    args.append({ConstantPointerNull::get(ptr), ConstantPointerNull::get(ptr), ir.getInt64(0)});
    auto *call = ir.CreateCall(fn == a ? b : a, args);
    call->setCallingConv(CallingConv::Tail);
    call->setTailCallKind(CallInst::TCK_MustTail);
    ir.CreateRet(call);
  }
  {
    IRBuilder<> ir(BasicBlock::Create(context, "entry", dispatch));
    auto *call = ir.CreateCall(shortType, dispatch->getArg(0),
        {dispatch->getArg(1), dispatch->getArg(2), dispatch->getArg(3), dispatch->getArg(4), dispatch->getArg(5)});
    call->setCallingConv(CallingConv::Tail);
    call->setTailCallKind(CallInst::TCK_MustTail);
    ir.CreateRet(call);
  }
  {
    IRBuilder<> ir(BasicBlock::Create(context, "entry", floating));
    auto *call = ir.CreateCall(a, {ConstantPointerNull::get(ptr), floating->getArg(2),
        ConstantPointerNull::get(ptr), ConstantPointerNull::get(ptr), ir.getInt64(0)});
    call->setCallingConv(CallingConv::Tail);
    ir.CreateRet(ir.CreateFAdd(ir.CreateSIToFP(call, ir.getDoubleTy()),
        ir.CreateFAdd(floating->getArg(0), ir.CreateFPExt(floating->getArg(1), ir.getDoubleTy()))));
  }
  auto *entry = Function::Create(FunctionType::get(i64, {i64}, false),
                                  GlobalValue::ExternalLinkage, "windows_tail_entry", *module);
  {
    IRBuilder<> ir(BasicBlock::Create(context, "entry", entry));
    auto *call = ir.CreateCall(dispatch, {a, ConstantPointerNull::get(ptr), entry->getArg(0),
        ConstantPointerNull::get(ptr), ConstantPointerNull::get(ptr), ir.getInt64(0)});
    call->setCallingConv(CallingConv::Tail);
    auto *fp = ir.CreateCall(floating, {ConstantFP::get(ir.getDoubleTy(), 0.5),
        ConstantFP::get(ir.getFloatTy(), 0.25), entry->getArg(0)});
    fp->setCallingConv(CallingConv::Tail);
    ir.CreateRet(ir.CreateAdd(call, ir.CreateFPToSI(fp, i64)));
  }
  check(!kalandra::normalizeWindowsTailPackets(*module));
  check(!verifyModule(*module, &errs()));
  for (auto &fn : *module)
    if (fn.getCallingConv() == CallingConv::Tail) {
      check(fn.arg_size() == 13 && fn.hasFnAttribute(Attribute::NoIPA));
      check(fn.hasFnAttribute(Attribute::NoInline) && !fn.hasFnAttribute(Attribute::AlwaysInline));
      for (auto &block : fn)
        for (auto &instruction : block)
          if (auto *call = dyn_cast<CallInst>(&instruction);
              call && call->getCallingConv() == CallingConv::Tail) check(call->arg_size() == 13);
    }
  check(entry->arg_size() == 1 && entry->getCallingConv() == CallingConv::C);
  return module;
}

int main(int argc, char **argv) {
  using namespace llvm;
  // Stack-passed AArch64 tailcc packets corrupt the unoptimized caller on
  // Linux and Windows too. Keep register-only calls fast, and choose the same
  // conservative ABI on every AArch64 OS (including mixed FP/GPR signatures).
  for (const char *triple : {"aarch64-apple-darwin", "aarch64-unknown-linux-gnu",
                             "aarch64-pc-windows-msvc"}) {
    LLVMContext context;
    auto *i64 = Type::getInt64Ty(context);
    auto *f32 = Type::getFloatTy(context);
    SmallVector<Type *, 16> parameters(8, i64);
    parameters.append(8, f32);
    auto convention = [&] {
      return kalandra::nativeInternalCallingConvention(
          FunctionType::get(i64, parameters, false), Triple(triple));
    };
    check(convention() == CallingConv::Tail);
    parameters.push_back(i64);
    check(convention() == CallingConv::C);
    parameters.pop_back();
    parameters.push_back(f32);
    check(convention() == CallingConv::C);
  }
  for (const char *triple : {"x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc"}) {
    LLVMContext context;
    Module module("windows-stack-high-water", context);
    module.setTargetTriple(Triple(triple));
    module.setDataLayout("e-p:64:64-i64:64-i128:128-n8:16:32:64-S128");
    auto *i64 = Type::getInt64Ty(context);
    auto *voidType = Type::getVoidTy(context);
    SmallVector<Type *, 256> types(200, i64);
    auto callee = module.getOrInsertFunction("wide", FunctionType::get(voidType, types, false));
    auto *fn = Function::Create(FunctionType::get(voidType, {i64}, false),
                                GlobalValue::ExternalLinkage, "serial", module);
    IRBuilder<> b(BasicBlock::Create(context, "entry", fn));
    SmallVector<Value *, 256> args(200, b.getInt64(1));
    for (unsigned n = 0; n < 100; ++n) b.CreateCall(callee, args);
    auto *ret = b.CreateRetVoid();
    check(!kalandra::nativeStackBudgetError(*fn, module.getDataLayout()));
    // A single oversized packet, true fixed storage, and accumulated SSA
    // pressure still fail. This does not enlarge the 16 KiB frame allowance.
    b.SetInsertPoint(ret);
    auto *large = b.CreateAlloca(b.getInt8Ty(), b.getInt64(17000));
    check(kalandra::nativeStackBudgetError(*fn, module.getDataLayout()).has_value());
    large->eraseFromParent();
    auto *dynamic = b.CreateAlloca(b.getInt8Ty(), fn->getArg(0));
    check(kalandra::nativeStackBudgetError(*fn, module.getDataLayout()).has_value());
    dynamic->eraseFromParent();
    Value *value = fn->getArg(0);
    for (unsigned n = 0; n < 2200; ++n) value = b.CreateAdd(value, fn->getArg(0));
    check(kalandra::nativeStackBudgetError(*fn, module.getDataLayout()).has_value());
    auto *oversized = Function::Create(FunctionType::get(voidType, {}, false),
                                 GlobalValue::ExternalLinkage, "oversized", module);
    IRBuilder<> big(BasicBlock::Create(context, "entry", oversized));
    SmallVector<Type *, 256> floatTypes(2050, big.getFloatTy());
    auto floats = module.getOrInsertFunction("many.floats", FunctionType::get(voidType, floatTypes, false));
    SmallVector<Value *, 256> floatArgs(2050, ConstantFP::get(big.getFloatTy(), 1));
    big.CreateCall(floats, floatArgs);
    big.CreateRetVoid();
    check(kalandra::nativeStackBudgetError(*oversized, module.getDataLayout()).has_value());
    check(kalandra::nativeStackBudgetError(*oversized, module.getDataLayout(), false, 17000).has_value());
    check(!verifyModule(module, &errs()));
  }
  for (bool reverse : {false, true}) {
    LLVMContext context;
    Module module("internal-calls", context);
    auto *i64 = Type::getInt64Ty(context);
    auto *ptr = PointerType::getUnqual(context);
    auto *signature = FunctionType::get(i64, {i64, i64}, false);
    auto *tree = Function::Create(signature, GlobalValue::ExternalLinkage, "kalandra_fn_tree", module);
    auto *other = Function::Create(signature, GlobalValue::ExternalLinkage, "kalandra_fn_other", module);
    auto *choose = Function::Create(signature, GlobalValue::ExternalLinkage, "kalandra_fn_choose", module);
    auto *nonTail = Function::Create(signature, GlobalValue::ExternalLinkage, "kalandra_fn_non_tail", module);
    auto *local = Function::Create(signature, GlobalValue::ExternalLinkage, "kalandra_fn_local", module);
    auto *leaf = Function::Create(FunctionType::get(i64, {i64}, false), GlobalValue::ExternalLinkage,
                                   "kalandra_fn_leaf", module);
    auto *callableType = FunctionType::get(i64, {ptr, i64, i64}, false);
    auto *a = Function::Create(callableType, GlobalValue::InternalLinkage, "kalandra_callable_a", module);
    auto *c = Function::Create(callableType, GlobalValue::InternalLinkage, "kalandra_callable_c", module);
    for (auto *fn : {tree, other, choose, nonTail, local}) {
      IRBuilder<> b(BasicBlock::Create(context, "entry", fn));
      if (fn == local) {
        auto *slot = b.CreateAlloca(i64);
        b.CreateStore(fn->getArg(1), slot);
        b.CreateRet(b.CreateCall(tree, {fn->getArg(0), b.CreateLoad(i64, slot)}));
        continue;
      }
      auto *done = BasicBlock::Create(context, "done", fn);
      auto *next = BasicBlock::Create(context, "next", fn);
      b.CreateCondBr(b.CreateICmpEQ(fn->getArg(0), b.getInt64(0)), done, next);
      b.SetInsertPoint(done);
      b.CreateRet(fn->getArg(1));
      b.SetInsertPoint(next);
      auto *n = b.CreateSub(fn->getArg(0), b.getInt64(1));
      Value *result;
      if (fn == choose) {
        auto *target = b.CreateSelect(b.CreateICmpEQ(fn->getArg(1), b.getInt64(0)), a, c);
        result = b.CreateCall(callableType, target,
                              {ConstantPointerNull::get(ptr), n, fn->getArg(1)});
      } else {
        auto *target = fn == tree ? other : tree;
        result = b.CreateCall(target, {n, fn->getArg(1)});
        if (fn == nonTail) result = b.CreateAdd(result, b.getInt64(1));
      }
      b.CreateRet(result);
    }
    for (auto *fn : {a, c}) {
      IRBuilder<> b(BasicBlock::Create(context, "entry", fn));
      b.CreateRet(b.CreateCall(fn == a ? tree : other, {fn->getArg(1), fn->getArg(2)}));
    }
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", leaf));
      b.CreateRet(b.CreateAdd(leaf->getArg(0), b.getInt64(1)));
    }
    auto *aggregate = StructType::get(i64, Type::getInt1Ty(context));
    auto *aggregateFn = Function::Create(FunctionType::get(aggregate, {aggregate}, false),
                                         GlobalValue::ExternalLinkage, "kalandra_fn_aggregate", module);
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", aggregateFn));
      b.CreateRet(b.CreateCall(aggregateFn, {aggregateFn->getArg(0)}));
    }
    auto *voidFn = Function::Create(FunctionType::get(Type::getVoidTy(context), {i64}, false),
                                    GlobalValue::ExternalLinkage, "kalandra_fn_void", module);
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", voidFn));
      b.CreateCall(voidFn, {voidFn->getArg(0)});
      b.CreateRetVoid();
    }
    if (reverse) {
      auto &functions = module.getFunctionList();
      functions.splice(functions.begin(), functions, voidFn->getIterator());
    }
    check(!kalandra::addCheckedInvocationABI(module, true));
    check(!verifyModule(module, &errs()));
    auto *entry = module.getFunction("kalandra_fn_tree");
    check(entry && entry->arg_size() == 3 && entry->getReturnType() == i64);
    check(calls(*entry, "kalandra_source_run_v4") == 1 &&
          calls(*entry, "kalandra_call_enter_v1") == 0 &&
          calls(*entry, "kalandra_call_leave_v1") == 0);
    check(module.getFunction("kalandra_fn_tree.boundary")->hasUWTable());
    check(module.getFunction("kalandra_fn_tree.body")->arg_size() == 6);
    check(module.getFunction("kalandra_fn_leaf.body.clear")->getFnAttribute("kalandra.effects")
              .getValueAsString() == "bounded-total");
    for (const auto *name : {"kalandra_fn_tree", "kalandra_fn_other", "kalandra_fn_choose",
                             "kalandra_fn_non_tail", "kalandra_fn_local", "kalandra_fn_aggregate",
                             "kalandra_fn_void"}) {
      for (const auto *mode : {"clear", "token", "observed"}) {
        auto *worker = module.getFunction(std::string(name) + ".body." + mode);
        check(worker && worker->hasLocalLinkage());
        check(calls(*worker, "kalandra_call_enter_v1") == 0 &&
              calls(*worker, "kalandra_call_leave_v1") == 0);
        bool tail = false, atomic = false;
        for (auto &block : *worker)
          for (auto &instruction : block) {
            if (auto *call = dyn_cast<CallInst>(&instruction)) tail |= call->isMustTailCall();
            if (auto *load = dyn_cast<LoadInst>(&instruction)) {
              atomic |= load->isAtomic() && load->getOrdering() == AtomicOrdering::Acquire;
              if (auto *gep = dyn_cast<GetElementPtrInst>(load->getPointerOperand());
                  gep && gep->getNumIndices() == 1)
                if (auto *offset = dyn_cast<ConstantInt>(gep->getOperand(1)))
                  check(offset->getZExtValue() != kalandra::runtime::InvocationABI::depth);
            }
          }
        const bool growing = StringRef(name).ends_with("non_tail") || StringRef(name).ends_with("local");
        check(tail != growing);
        check(atomic == (StringRef(mode) == "token"));
        check(!kalandra::nativeStackBudgetError(*worker, module.getDataLayout()));
      }
    }
    auto *chooseWorker = module.getFunction("kalandra_fn_choose.body.clear");
    bool indirectTail = false;
    for (auto &block : *chooseWorker)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction); call && !call->getCalledFunction())
          indirectTail |= call->isMustTailCall();
    check(indirectTail);
    auto *planned = Function::Create(FunctionType::get(i64, {ptr, i64, i64, ptr}, false),
                                      GlobalValue::InternalLinkage, "kalandra_callable_plan_test", module);
    IRBuilder<> plannedIR(BasicBlock::Create(context, "entry", planned));
    plannedIR.CreateRet(planned->getArg(1));
    auto *wrong = Function::Create(FunctionType::get(Type::getDoubleTy(context), {ptr, i64, i64, ptr}, false),
                                    GlobalValue::InternalLinkage, "kalandra_callable_plan_wrong", module);
    IRBuilder<> wrongIR(BasicBlock::Create(context, "entry", wrong));
    wrongIR.CreateRet(ConstantFP::get(Type::getDoubleTy(context), 0));
    kalandra::finalizeNativeCallResolvers(module);
    auto *adapter = module.getFunction("kalandra_callable_plan_test.native");
    check(adapter && calls(*adapter, planned->getName()) == 1);
    check(!module.getFunction("kalandra_callable_plan_wrong.native"));
    check(!verifyModule(module, &errs()));

  }
  {
    LLVMContext context;
    Module module("leaf-regions", context);
    auto *i64 = Type::getInt64Ty(context);
    auto *signature = FunctionType::get(i64, {i64, i64}, false);
    auto *fn = Function::Create(signature, GlobalValue::InternalLinkage,
                                "tree.body.clear", module);
    auto *entry = BasicBlock::Create(context, "entry", fn);
    auto *leaf = BasicBlock::Create(context, "leaf", fn);
    auto *a = BasicBlock::Create(context, "a", fn);
    auto *b = BasicBlock::Create(context, "b", fn);
    auto *slow = BasicBlock::Create(context, "slow", fn);
    auto *done = BasicBlock::Create(context, "done", fn);
    IRBuilder<> ir(entry);
    ir.CreateCondBr(ir.CreateICmpEQ(fn->getArg(0), ir.getInt64(0)), leaf, slow);
    ir.SetInsertPoint(leaf);
    auto *choice = ir.CreateSwitch(fn->getArg(1), a, 1);
    choice->addCase(ir.getInt64(1), b);
    ir.SetInsertPoint(a);
    auto *left = ir.CreateAdd(fn->getArg(1), ir.getInt64(3));
    ir.CreateBr(done);
    ir.SetInsertPoint(b);
    auto *right = ir.CreateMul(fn->getArg(1), ir.getInt64(7));
    ir.CreateBr(done);
    ir.SetInsertPoint(slow);
    auto *n = ir.CreateSub(fn->getArg(0), ir.getInt64(1));
    auto *first = ir.CreateCall(fn, {n, fn->getArg(1)});
    auto *second = ir.CreateCall(fn, {n, ir.CreateAdd(fn->getArg(1), ir.getInt64(1))});
    auto *sum = ir.CreateAdd(first, second);
    ir.CreateBr(done);
    ir.SetInsertPoint(done);
    auto *value = ir.CreatePHI(i64, 3);
    value->addIncoming(left, a);
    value->addIncoming(right, b);
    value->addIncoming(sum, slow);
    ir.CreateRet(value);
    kalandra::outlineNativeLeafPaths(module, [](llvm::Function &, bool) {});
    check(!verifyModule(module, &errs()));
    check(fn->hasFnAttribute(Attribute::AlwaysInline));
    auto *worker = module.getFunction("tree.body.clear.branch");
    check(worker && worker->hasFnAttribute(Attribute::NoInline));
    check(calls(*worker, fn->getName()) == 2);
    unsigned tails = 0;
    for (auto &block : *fn)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction)) tails += call->isMustTailCall();
    check(tails == 1);

    auto *sp = Function::Create(FunctionType::get(i64, {}, false),
                                GlobalValue::InternalLinkage, "stack-pointer", module);
    ir.SetInsertPoint(BasicBlock::Create(context, "entry", sp));
    ir.CreateRet(kalandra::nativeStackPointer(ir));
    check(!kalandra::nativeStackBudgetError(*sp, module.getDataLayout()));
    check(!verifyModule(module, &errs()));
  }
  {
    LLVMContext context;
    Module module("post-inline-stack-guards", context);
    auto *i64 = Type::getInt64Ty(context);
    auto *signature = FunctionType::get(i64, {i64}, false);
    auto *growing = Function::Create(signature, GlobalValue::ExternalLinkage, "growing", module);
    for (bool call : {false, true}) {
      auto *fn = Function::Create(signature, GlobalValue::ExternalLinkage,
                                  call ? "retained" : "removed", module);
      IRBuilder<> ir(BasicBlock::Create(context, "entry", fn));
      auto *accepted = BasicBlock::Create(context, "accepted", fn);
      auto *failed = BasicBlock::Create(context, "failed", fn);
      auto *condition = ir.CreateICmpUGT(kalandra::nativeStackPointer(ir), fn->getArg(0));
      auto *guard = ir.CreateCondBr(condition, accepted, failed);
      guard->setMetadata("kalandra.physical.guard", MDNode::get(context, {}));
      ir.SetInsertPoint(accepted);
      ir.CreateRet(call ? static_cast<Value *>(ir.CreateCall(growing, {fn->getArg(0)})) : fn->getArg(0));
      ir.SetInsertPoint(failed);
      ir.CreateRet(ir.getInt64(0));
    }
    check(kalandra::removeInlinedNativeStackGuards(module));
    check(!isa<CondBrInst>(module.getFunction("removed")->getEntryBlock().getTerminator()));
    check(isa<CondBrInst>(module.getFunction("retained")->getEntryBlock().getTerminator()));
    check(calls(*module.getFunction("removed"), "llvm.read_volatile_register.i64") == 0);
    check(calls(*module.getFunction("retained"), "llvm.read_volatile_register.i64") == 1);
    check(!verifyModule(module, &errs()));

    auto *fn = Function::Create(signature, GlobalValue::InternalLinkage,
                                "loop.body.clear", module);
    IRBuilder<> ir(BasicBlock::Create(context, "entry", fn));
    auto *leaf = BasicBlock::Create(context, "leaf", fn);
    auto *loop = BasicBlock::Create(context, "loop", fn);
    auto *done = BasicBlock::Create(context, "done", fn);
    auto *failed = BasicBlock::Create(context, "failed", fn);
    auto *slow = BasicBlock::Create(context, "slow", fn);
    ir.CreateCondBr(ir.CreateICmpEQ(fn->getArg(0), ir.getInt64(0)), leaf, slow);
    ir.SetInsertPoint(leaf);
    ir.CreateCondBr(ir.CreateICmpSLT(fn->getArg(0), ir.getInt64(-1)), failed, loop);
    ir.SetInsertPoint(loop);
    auto *index = ir.CreatePHI(i64, 2);
    index->addIncoming(fn->getArg(0), leaf);
    auto *next = ir.CreateAdd(index, ir.getInt64(1));
    index->addIncoming(next, loop);
    ir.CreateCondBr(ir.CreateICmpSLT(next, ir.getInt64(4)), loop, done);
    ir.SetInsertPoint(done);
    ir.CreateRet(next);
    ir.SetInsertPoint(failed);
    auto fault = module.getOrInsertFunction("kalandra_fault_v1", ir.getVoidTy(), i64);
    auto abort = module.getOrInsertFunction("kalandra_source_abort_v2", ir.getVoidTy());
    ir.CreateCall(fault, {ir.getInt64(4)});
    ir.CreateCall(abort);
    ir.CreateUnreachable();
    ir.SetInsertPoint(slow);
    auto *child = ir.CreateCall(fn, {ir.CreateSub(fn->getArg(0), ir.getInt64(1))});
    ir.CreateRet(ir.CreateAdd(child, ir.getInt64(1)));
    kalandra::outlineNativeLeafPaths(module, [](llvm::Function &, bool) {});
    check(fn->hasFnAttribute(Attribute::AlwaysInline));
    check(module.getFunction("loop.body.clear.branch"));
    check(!verifyModule(module, &errs()));
  }
  {
    LLVMContext context;
    Module module("arm64-checked-remainder", context);
    module.setTargetTriple(Triple("aarch64-apple-darwin"));
    auto *i64 = Type::getInt64Ty(context);
    auto *fn = Function::Create(FunctionType::get(i64, {i64, i64}, false),
                                GlobalValue::ExternalLinkage, "remainder", module);
    IRBuilder<> ir(BasicBlock::Create(context, "entry", fn));
    auto *divisor = ir.CreateSelect(ir.CreateICmpEQ(fn->getArg(1), ir.getInt64(-1)),
                                    ir.getInt64(1), fn->getArg(1));
    ir.CreateRet(ir.CreateSRem(fn->getArg(0), divisor));
    module.setTargetTriple(Triple("x86_64-pc-windows-msvc"));
    kalandra::lowerAArch64CheckedRemainders(module);
    check(isa<BinaryOperator>(cast<ReturnInst>(fn->back().getTerminator())->getReturnValue()));
    module.setTargetTriple(Triple("aarch64-apple-darwin"));
    kalandra::lowerAArch64CheckedRemainders(module);
    auto *value = dyn_cast<CallInst>(cast<ReturnInst>(fn->back().getTerminator())->getReturnValue());
    check(value && value->isInlineAsm() && value->getArgOperand(1) == fn->getArg(1));
    check(!verifyModule(module, &errs()));
  }
  {
    LLVMContext context;
    auto module = windowsTailFixture(context);
    check(argc == 1 || argc == 2);
    if (argc == 2) {
      std::error_code error;
      raw_fd_ostream output(argv[1], error);
      check(!error);
      module->print(output, nullptr);
    }
  }
  for (const char *triple : {"aarch64-pc-windows-msvc", "x86_64-pc-windows-msvc",
                             "aarch64-apple-darwin", "aarch64-unknown-linux-gnu"}) {
    LLVMContext context;
    Module module("cross-convention-stack-cleanup", context);
    module.setTargetTriple(Triple(triple));
    auto *type = FunctionType::get(Type::getInt64Ty(context), {Type::getInt64Ty(context)}, false);
    auto function = [&](const char *name, CallingConv::ID convention) {
      auto *fn = Function::Create(type, GlobalValue::InternalLinkage, name, module);
      fn->setCallingConv(convention);
      return fn;
    };
    auto *fast = function("fast.callee", CallingConv::Fast);
    IRBuilder<> f(BasicBlock::Create(context, "entry", fast));
    f.CreateRet(fast->getArg(0));
    auto *tail = function("tail.callee", CallingConv::Tail);
    IRBuilder<> t(BasicBlock::Create(context, "entry", tail));
    t.CreateRet(tail->getArg(0));
    auto bridge = [&](const char *name, CallingConv::ID convention, Function *callee,
                      CallInst::TailCallKind kind) {
      auto *fn = function(name, convention);
      IRBuilder<> ir(BasicBlock::Create(context, "entry", fn));
      auto *call = ir.CreateCall(callee, {fn->getArg(0)});
      call->setCallingConv(callee->getCallingConv());
      call->setTailCallKind(kind);
      ir.CreateRet(call);
      return fn;
    };
    auto *outgoing = bridge("tail.to.fast", CallingConv::Tail, fast, CallInst::TCK_Tail);
    auto *incoming = bridge("c.to.tail", CallingConv::C, tail, CallInst::TCK_Tail);
    auto *guaranteed = bridge("tail.to.tail", CallingConv::Tail, tail, CallInst::TCK_MustTail);
    check(!kalandra::normalizeWindowsTailPackets(module));
    const bool armWindows = module.getTargetTriple().isOSWindows() && module.getTargetTriple().isAArch64();
    for (auto *fn : {outgoing, incoming}) {
      auto *call = cast<CallInst>(&fn->front().front());
      check(call->getTailCallKind() == (armWindows ? CallInst::TCK_NoTail : CallInst::TCK_Tail));
    }
    check(cast<CallInst>(&guaranteed->front().front())->isMustTailCall());
    check(!verifyModule(module, &errs()));
  }
  {
    LLVMContext context;
    auto module = std::make_unique<Module>("native-helper-lifetime", context);
    module->setTargetTriple(Triple(KALANDRA_NATIVE_CPU_TARGET_TRIPLE));
    module->setDataLayout(KALANDRA_NATIVE_CPU_DATA_LAYOUT);
    auto *type = FunctionType::get(Type::getInt64Ty(context), {}, false);
    auto *helper = Function::Create(type, GlobalValue::InternalLinkage, "owned.helper", *module);
    helper->addFnAttr("kalandra.task.frame");
    IRBuilder<> h(BasicBlock::Create(context, "entry", helper));
    // Model a helper whose bookkeeping must finish before source abandonment.
    h.CreateRet(h.CreateCall(module->getOrInsertFunction("host.owned.work", type)));
    auto *source = Function::Create(type, GlobalValue::ExternalLinkage, "source", *module);
    source->addFnAttr("kalandra.native.pure-frame");
    IRBuilder<> b(BasicBlock::Create(context, "entry", source));
    b.CreateRet(b.CreateCall(helper));
    check(!kalandra::optimizeNativeCalls(module));
    check(!verifyModule(*module, &errs()));
    source = module->getFunction("source");
    helper = module->getFunction("owned.helper");
    check(source && helper && !helper->hasFnAttribute(Attribute::NoInline));
    unsigned barriers = 0;
    for (auto &block : *source)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction); call && call->getCalledFunction() == helper)
          barriers += call->hasFnAttr(Attribute::NoInline);
    const bool native = module->getTargetTriple().isOSDarwin() && module->getTargetTriple().isAArch64();
    check(barriers == unsigned(native)); // Helper-internal optimization remains allowed.
  }
  std::cout << "Internal convention: boundary admission, native returns, uniform environments, "
               "mutual/indirect/aggregate/void tail calls, local-frame fallback and token checkpoints passed\n";
}
