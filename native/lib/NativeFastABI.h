#pragma once
#include "../runtime/Runtime.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace kalandra {
// Inline polling at checked runtime-helper boundaries. Source progress checks
// are specialized with the internal call graph; admission stays in the runtime.
inline void defineNativeFastABI(llvm::Module &module) {
  using namespace llvm;
  using Layout = runtime::InvocationABI;
  auto &context = module.getContext();
  auto *i8 = Type::getInt8Ty(context);
  auto *i64 = Type::getInt64Ty(context);
  auto *ptr = PointerType::getUnqual(context);
  auto *poll = module.getFunction("kalandra_poll_v1");
  auto prepare = [&](Function *fn) {
    fn->setLinkage(GlobalValue::InternalLinkage);
    fn->addFnAttr(Attribute::AlwaysInline);
    fn->addFnAttr(Attribute::NoUnwind);
    return BasicBlock::Create(context, "entry", fn);
  };
  auto address = [&](IRBuilder<> &b, Value *invocation, std::size_t offset) {
    return b.CreateConstGEP1_64(i8, invocation, offset);
  };
  auto load = [&](IRBuilder<> &b, Type *type, Value *invocation, std::size_t offset) {
    return b.CreateLoad(type, address(b, invocation, offset));
  };
  {
    IRBuilder<> b(prepare(poll));
    auto *invocation = poll->getArg(0);
    auto *mode = load(b, i8, invocation, Layout::pollMode);
    auto *fast = BasicBlock::Create(context, "simple", poll);
    auto *tokenCheck = BasicBlock::Create(context, "token.check", poll);
    auto *tokenLoad = BasicBlock::Create(context, "token.load", poll);
    auto *slow = BasicBlock::Create(context, "observable", poll);
    b.CreateCondBr(b.CreateICmpEQ(mode, b.getInt8(0)), fast, tokenCheck);
    b.SetInsertPoint(fast);
    b.CreateRet(b.getInt64(0));
    b.SetInsertPoint(tokenCheck);
    // Armed mode has the same healthy-token invariant as mode 1. Faults,
    // observers and settlement switch to mode 2 before returning to source.
    auto *healthy = b.CreateOr(b.CreateICmpEQ(mode, b.getInt8(1)),
                              b.CreateICmpEQ(mode, b.getInt8(3)));
    auto *tokenPresent = BasicBlock::Create(context, "token.present", poll);
    b.CreateCondBr(healthy, tokenPresent, slow);
    b.SetInsertPoint(tokenPresent);
    auto *token = load(b, ptr, invocation, Layout::cancellation);
    b.CreateCondBr(b.CreateIsNull(token), fast, tokenLoad);
    b.SetInsertPoint(tokenLoad);
    auto *value = b.CreateLoad(i8, token);
    value->setAtomic(AtomicOrdering::Acquire);
    value->setAlignment(Align(1));
    b.CreateRet(b.CreateSelect(b.CreateICmpNE(value, b.getInt8(0)), b.getInt64(-1), b.getInt64(0)));
    b.SetInsertPoint(slow);
    // Keep fault/probe/token handling out of every source function's hot body.
    // A probe, fault or settlement permanently selects the observed path.
    auto fallback = module.getOrInsertFunction("kalandra_poll_slow_v1", i64, ptr);
    b.CreateRet(b.CreateCall(fallback, {invocation}));
  }
}
} // namespace kalandra
