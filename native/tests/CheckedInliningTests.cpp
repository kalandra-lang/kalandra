#include "../lib/NativeStack.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <stdexcept>
namespace kalandra {
std::optional<std::string> addCheckedInvocationABI(llvm::Module &, bool);
}
static void check(bool value) {
  if (!value)
    throw std::runtime_error("checked inlining regression");
}
static unsigned calls(llvm::Function &function, llvm::StringRef target) {
  unsigned count = 0;
  for (auto &block : function)
    for (auto &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        count += call->getCalledFunction() && call->getCalledFunction()->getName() == target;
  return count;
}
int main() {
  using namespace llvm;
  for (bool reverse : {false, true}) {
    LLVMContext context;
    Module module("checked-inline", context);
    auto *i64 = Type::getInt64Ty(context);
    auto *wide = Type::getInt128Ty(context);
    auto *tree = Function::Create(FunctionType::get(i64, {i64}, false),
                                  GlobalValue::ExternalLinkage, "kalandra_fn_tree", module);
    auto *leaf = Function::Create(FunctionType::get(wide, std::vector<Type *>(8, wide), false),
                                  GlobalValue::ExternalLinkage, "budget_leaf", module);
    auto *large = Function::Create(FunctionType::get(wide, {wide}, false),
                                   GlobalValue::ExternalLinkage, "kalandra_fn_large", module);
    auto *caller = Function::Create(FunctionType::get(wide, {wide}, false),
                                    GlobalValue::ExternalLinkage, "kalandra_fn_caller", module);
    auto *memory = new GlobalVariable(module, i64, false, GlobalValue::InternalLinkage,
                                      ConstantInt::get(i64, 7), "observed_memory");
    auto *impure = Function::Create(FunctionType::get(i64, {i64}, false),
                                    GlobalValue::InternalLinkage, "memory_leaf", module);
    auto *unsafe = Function::Create(FunctionType::get(i64, {i64}, false),
                                    GlobalValue::ExternalLinkage, "kalandra_fn_unsafe", module);
    auto *dense = Function::Create(FunctionType::get(i64, {i64}, false),
                                   GlobalValue::ExternalLinkage, "kalandra_fn_dense", module);
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", dense));
      Value *value = dense->getArg(0);
      for (unsigned i = 0; i < 50; ++i)
        value = b.CreateCall(tree, {value});
      b.CreateRet(value);
    }
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", impure));
      b.CreateRet(b.CreateAdd(b.CreateLoad(i64, memory), impure->getArg(0)));
    }
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", unsafe));
      auto *done = BasicBlock::Create(context, "done", unsafe);
      auto *recurse = BasicBlock::Create(context, "recurse", unsafe);
      b.CreateCondBr(b.CreateICmpEQ(unsafe->getArg(0), b.getInt64(0)), done, recurse);
      b.SetInsertPoint(done);
      b.CreateRet(b.CreateCall(impure, {unsafe->getArg(0)}));
      b.SetInsertPoint(recurse);
      b.CreateRet(b.CreateAdd(b.CreateCall(unsafe, {b.CreateSub(unsafe->getArg(0), b.getInt64(1))}),
                              b.getInt64(1)));
    }
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", tree));
      auto *done = BasicBlock::Create(context, "done", tree);
      auto *recurse = BasicBlock::Create(context, "recurse", tree);
      b.CreateCondBr(b.CreateICmpEQ(tree->getArg(0), b.getInt64(0)), done, recurse);
      b.SetInsertPoint(done);
      b.CreateRet(b.getInt64(1));
      b.SetInsertPoint(recurse);
      auto *n = b.CreateSub(tree->getArg(0), b.getInt64(1));
      auto *a = b.CreateCall(tree, {n});
      auto *c = b.CreateCall(tree, {n});
      b.CreateRet(b.CreateAdd(a, c));
    }
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", leaf));
      Value *value = leaf->getArg(0);
      for (unsigned i = 1; i < 8; ++i)
        value = b.CreateAdd(value, leaf->getArg(i));
      b.CreateRet(value);
    }
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", large));
      Value *value = large->getArg(0);
      for (unsigned i = 0; i < 41; ++i) {
        SmallVector<Value *, 8> arguments(8, value);
        value = b.CreateCall(leaf, arguments);
      }
      b.CreateRet(value);
    }
    {
      IRBuilder<> b(BasicBlock::Create(context, "entry", caller));
      Value *value = caller->getArg(0);
      for (unsigned i = 0; i < 4; ++i)
        value = b.CreateCall(large, {value});
      b.CreateRet(value);
    }
    if (reverse) {
      auto &functions = module.getFunctionList();
      functions.splice(tree->getIterator(), functions, large->getIterator());
      functions.splice(large->getIterator(), functions, caller->getIterator());
      functions.splice(module.begin(), functions, unsafe->getIterator());
    }
    check(!kalandra::addCheckedInvocationABI(module, false));
    check(!verifyModule(module, &errs()));
    auto &inlined = *module.getFunction("kalandra_fn_tree");
    // Exactly one frozen recursive level: four physical recursive calls, but
    // all three logical admission/return paths and six post-call polls remain.
    check(calls(inlined, "kalandra_recursive_dispatch_kalandra_fn_tree") == 4);
    check(calls(inlined, "kalandra_call_enter_v1") == 3);
    check(calls(inlined, "kalandra_call_leave_v1") >= 3);
    check(calls(inlined, "kalandra_poll_v1") == 6);
    for (const auto *mode : {"clear", "token"}) {
      const auto name = std::string("kalandra_fn_tree.recursive.") + mode;
      auto *worker = module.getFunction(name);
      check(worker && worker->hasLocalLinkage() && worker->hasFnAttribute(Attribute::NoInline));
      check(calls(*worker, name) == 4);
      check(calls(*worker, std::string("kalandra_recursive_enter_") + mode) == 1);
      check(calls(*worker, std::string("kalandra_recursive_inline_enter_") + mode) == 2);
      check(calls(*worker, std::string("kalandra_recursive_poll_") + mode) == 6);
      check(calls(*worker, "kalandra_recursive_leave_v1") >= 3);
      check(!kalandra::nativeStackBudgetError(*worker, module.getDataLayout()));
    }
    check(calls(*module.getFunction("kalandra_recursive_dispatch_kalandra_fn_tree"),
                "kalandra_fn_tree.recursive.observed") == 1);
    check(!kalandra::nativeStackBudgetError(
        *module.getFunction("kalandra_recursive_dispatch_kalandra_fn_tree"), module.getDataLayout(),
        false, 16 * 1024 - 512));
    auto *observed = module.getFunction("kalandra_fn_tree.recursive.observed");
    check(observed && calls(*observed, observed->getName()) == 4);
    check(calls(*observed, "kalandra_call_enter_v1") == 3);
    check(calls(*observed, "kalandra_poll_v1") == 6);
    auto *largeDispatch = module.getFunction("kalandra_fn_dense");
    check(largeDispatch && calls(*largeDispatch, "kalandra_fn_tree") >= 50);
    check(calls(*largeDispatch, "kalandra_recursive_dispatch_kalandra_fn_tree") == 0);
    check(!module.getFunction("kalandra_fn_unsafe.recursive.clear"));
    auto &poll = *module.getFunction("kalandra_recursive_poll_token");
    bool acquire = false;
    for (auto &block : poll)
      for (auto &instruction : block)
        if (auto *load = dyn_cast<LoadInst>(&instruction))
          acquire |= load->getOrdering() == AtomicOrdering::Acquire;
    check(acquire);
    check(calls(*module.getFunction("kalandra_recursive_inline_enter_clear"),
                "llvm.stacksave.p0") == 0);
#if defined(__APPLE__)
    check(calls(*module.getFunction("kalandra_recursive_enter_clear"), "llvm.stacksave.p0") == 1);
#endif
    // This candidate fits before inlining, but its four large callees would
    // exceed 16 KiB. Keep the original accepted source body, with four calls.
    auto &bounded = *module.getFunction("kalandra_fn_caller");
    check(calls(bounded, "kalandra_fn_large") == 4);
    check(calls(bounded, "budget_leaf") == 0);
    auto &eligible = *module.getFunction("kalandra_fn_large");
    unsigned size = 0;
    for (auto &block : eligible)
      size += block.size();
    check(size <= 250 && calls(eligible, "budget_leaf") == 41);
    check(!kalandra::nativeStackBudgetError(bounded, module.getDataLayout()));
    check(!kalandra::nativeStackBudgetError(inlined, module.getDataLayout()));
    unsigned sources = 0;
    for (auto &function : module)
      sources += function.getName().starts_with("kalandra_fn_");
    check(sources == 8);
  }
  std::cout << "Checked inlining: preserved admissions/polls, bounded growth, budget fallback and "
               "module order passed\n";
}
