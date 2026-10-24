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
int main() {
  using namespace llvm;
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
    check(calls(*entry, "kalandra_source_run_v3") == 1 &&
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
  std::cout << "Internal convention: boundary admission, native returns, uniform environments, "
               "mutual/indirect/aggregate/void tail calls, local-frame fallback and token checkpoints passed\n";
}
