#include "../lib/LocalStorage.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <stdexcept>
int main() {
  using namespace llvm;
  LLVMContext context;
  Module module("local-environments", context);
  IRBuilder<> b(context);
  auto *pointer = b.getPtrTy();
  auto allocate = module.getOrInsertFunction("kalandra_environment_alloc_v1", pointer,
                                             pointer, b.getInt64Ty(), pointer);
  auto foreign = module.getOrInsertFunction("foreign", b.getVoidTy(), pointer);
  SmallVector<Function *, 12> functions;
  for (unsigned kind = 0; kind < 9; ++kind) {
    auto *fn = Function::Create(FunctionType::get(pointer, {pointer}, false),
                                GlobalValue::ExternalLinkage, "case" + std::to_string(kind), module);
    functions.push_back(fn);
    b.SetInsertPoint(BasicBlock::Create(context, "entry", fn));
    if (kind == 4) {
      auto *loop = BasicBlock::Create(context, "loop", fn);
      b.CreateBr(loop);
      b.SetInsertPoint(loop);
    }
    auto *memory = b.CreateCall(allocate, {fn->getArg(0), b.getInt64(kind == 6 ? 512 : 8), fn});
    b.CreateStore(b.getInt64(42), memory);
    b.CreateLoad(b.getInt64Ty(), memory);
    if (kind == 1) { b.CreateRet(memory); continue; }
    if (kind == 2) b.CreateStore(memory, fn->getArg(0));
    if (kind == 3) b.CreateCall(foreign, {memory});
    if (kind == 4) {
      auto *done = BasicBlock::Create(context, "done", fn);
      b.CreateCondBr(b.CreateIsNull(fn->getArg(0)), b.GetInsertBlock(), done);
      b.SetInsertPoint(done);
    }
    if (kind == 5) b.CreatePtrToInt(memory, b.getInt64Ty());
    if (kind == 7) b.CreateStore(memory, memory);
    if (kind == 8) fn->addFnAttr("kalandra.task.frame");
    b.CreateRet(ConstantPointerNull::get(pointer));
  }
  if (verifyModule(module, &errs()) || !kalandra::promoteLocalEnvironments(module) ||
      verifyModule(module, &errs())) return 1;
  for (unsigned kind = 0; kind < functions.size(); ++kind) {
    unsigned allocations = 0;
    for (auto &block : *functions[kind])
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction))
          allocations += call->getCalledFunction() == allocate.getCallee();
    if (allocations != (kind == 0 ? 0u : 1u))
      throw std::runtime_error("local storage escaped its admission proof: " + std::to_string(kind));
  }
  std::cout << "Local storage: local scalar captures promoted; returns, publication, foreign "
               "calls, CFG cycles, pointer identity, large values, self references and task "
               "frames preserve heap ownership\n";
}
