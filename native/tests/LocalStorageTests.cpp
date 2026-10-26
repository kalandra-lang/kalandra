#include "../lib/LocalStorage.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <stdexcept>
int main() {
  using namespace llvm;
  LLVMContext context;
  Module module("local-environments", context);
  module.setDataLayout("e-p:64:64");
  IRBuilder<> b(context);
  auto *pointer = b.getPtrTy();
  auto allocate = module.getOrInsertFunction("kalandra_environment_alloc_v1", pointer,
                                             pointer, b.getInt64Ty(), pointer);
  auto foreign = module.getOrInsertFunction("foreign", b.getVoidTy(), pointer);
  auto slowPoll = module.getOrInsertFunction("kalandra_poll_slow_v1", b.getInt64Ty(), pointer);
  SmallVector<Function *, 12> functions;
  for (unsigned kind = 0; kind < 16; ++kind) {
    auto *fn = Function::Create(FunctionType::get(pointer, {pointer}, false),
                                GlobalValue::ExternalLinkage, "case" + std::to_string(kind), module);
    functions.push_back(fn);
    if (kind >= 9) fn->setName(fn->getName().str() + (kind == 11 ? ".body.token" : ".body.clear"));
    b.SetInsertPoint(BasicBlock::Create(context, "entry", fn));
    if (kind == 4) {
      auto *loop = BasicBlock::Create(context, "loop", fn);
      b.CreateBr(loop);
      b.SetInsertPoint(loop);
    }
    auto *memory = b.CreateCall(allocate, {fn->getArg(0), b.getInt64(kind == 6 ? 512 : 8), fn});
    if (kind >= 9) {
      if (kind == 13) b.CreateStore(b.getInt8(1), fn->getArg(0));
      auto *address = b.CreateConstGEP1_64(b.getInt8Ty(), fn->getArg(0), kind == 10 ? 217 : 216);
      auto *mode = b.CreateLoad(b.getInt8Ty(), address);
      auto *observe = BasicBlock::Create(context, "observe", fn);
      auto *token = BasicBlock::Create(context, "token", fn);
      auto *join = BasicBlock::Create(context, "join", fn);
      auto *ready = BasicBlock::Create(context, "ready", fn);
      auto *failed = BasicBlock::Create(context, "failed", fn);
      auto *dispatch = b.CreateSwitch(mode, observe, 2);
      dispatch->addCase(b.getInt8(0), ready); dispatch->addCase(b.getInt8(1), token);
      b.SetInsertPoint(observe);
      if (kind == 14) b.CreateCall(foreign, {fn->getArg(0)});
      auto *status = b.CreateCall(slowPoll, {kind == 15 ? static_cast<Value *>(fn) : fn->getArg(0)});
      b.CreateBr(join);
      b.SetInsertPoint(token); b.CreateBr(join);
      b.SetInsertPoint(join);
      auto *combined = b.CreatePHI(b.getInt64Ty(), 2);
      combined->addIncoming(status, observe); combined->addIncoming(b.getInt64(0), token);
      b.CreateCondBr(b.CreateIsNull(combined), ready, failed);
      b.SetInsertPoint(failed); b.CreateRet(ConstantPointerNull::get(pointer));
      b.SetInsertPoint(ready);
      b.CreateStore(b.getInt64(42), memory); b.CreateLoad(b.getInt64Ty(), memory);
      b.CreateRet(kind == 12 ? memory : static_cast<Value *>(ConstantPointerNull::get(pointer)));
      continue;
    }
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
  if (verifyModule(module, &errs()) || !kalandra::promoteLocalEnvironments(module, 216) ||
      verifyModule(module, &errs())) return 1;
  for (unsigned kind = 0; kind < functions.size(); ++kind) {
    unsigned allocations = 0;
    for (auto &block : *functions[kind])
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction))
          allocations += call->getCalledFunction() == allocate.getCallee();
    const bool local = kind == 0 || (kind >= 9 && kind != 12);
    if (allocations != (local ? 0u : 1u))
      throw std::runtime_error("local storage escaped its admission proof: " + std::to_string(kind));
    if (kind >= 9 && isa<UncondBrInst>(functions[kind]->getEntryBlock().getTerminator()) != (kind == 9))
      throw std::runtime_error("allocation status diamond escaped its proof: " + std::to_string(kind));
  }
  std::cout << "Local storage: local scalar captures promoted; returns, publication, foreign "
               "calls, CFG cycles, pointer identity, large values, self references and task "
               "frames preserve heap ownership\n";
}
