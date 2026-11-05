#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <stdexcept>
#include <vector>
#include <span>
#include <set>

namespace kalandra {
struct CpuClosureTarget {
  llvm::Function *code;
  std::uint64_t tag;
  bool captures;
};
// Map an owned native closure carrier to a compiler-local device target tag.
// The environment remains a borrowed host pointer for checked capture reading;
// neither native pointer is serialized. Emit before CPU projection renaming so
// identity comparisons follow LLVM symbol references through linking.
inline llvm::Function *emitCpuClosureBridge(llvm::Module &module, llvm::StringRef name,
                                           std::span<const CpuClosureTarget> targets) {
  if (targets.empty() || targets.size() > 300000 || module.getNamedValue(name))
    throw std::invalid_argument("CPU closure bridge requires a bounded target set and unique name");
  std::set<llvm::Function *> codes;
  std::set<std::uint64_t> tags;
  for (const auto &target : targets)
    if (!target.code || target.code->getParent() != &module || target.code->isDeclaration() ||
        !target.tag || !codes.insert(target.code).second || !tags.insert(target.tag).second)
      throw std::invalid_argument("CPU closure bridge has an invalid or repeated target identity");
  auto &context = module.getContext();
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *tagType = llvm::Type::getInt64Ty(context);
  auto *carrier = llvm::StructType::get(context, {pointer, pointer});
  auto *bridge = llvm::Function::Create(
      llvm::FunctionType::get(tagType, {pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", bridge));
  auto *null = llvm::ConstantPointerNull::get(pointer);
  builder.CreateStore(null, bridge->getArg(1));
  auto *value = builder.CreateLoad(carrier, bridge->getArg(0));
  auto *code = builder.CreateExtractValue(value, 0);
  auto *environment = builder.CreateExtractValue(value, 1);
  for (const auto &target : targets) {
    // Pointer identity is observable at this private boundary.
    target.code->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    auto *matched = llvm::BasicBlock::Create(context, "matched", bridge);
    auto *next = llvm::BasicBlock::Create(context, "next", bridge);
    auto *validEnvironment = target.captures ? builder.CreateICmpNE(environment, null)
                                              : builder.CreateICmpEQ(environment, null);
    builder.CreateCondBr(builder.CreateAnd(builder.CreateICmpEQ(code, target.code), validEnvironment),
                          matched, next);
    builder.SetInsertPoint(matched);
    builder.CreateStore(environment, bridge->getArg(1));
    builder.CreateRet(llvm::ConstantInt::get(tagType, target.tag));
    builder.SetInsertPoint(next);
  }
  builder.CreateRet(llvm::ConstantInt::get(tagType, 0));
  return bridge;
}
// Private native bridge for generated host code. Aggregate parameters are
// addresses of exact LLVM-native slots; aggregate returns use an explicit first
// output pointer. This avoids relying on platform C++ struct argument/return
// classification. It changes no ownership or source admission rules.
inline llvm::Function *emitCpuPointerBridge(llvm::Function &target, llvm::StringRef name) {
  auto *module = target.getParent();
  if (!module || target.isVarArg() || module->getNamedValue(name))
    throw std::invalid_argument("CPU pointer bridge requires a unique, fixed native signature");
  auto &context = module->getContext();
  auto *pointer = llvm::PointerType::getUnqual(context);
  const bool aggregateResult = target.getReturnType()->isStructTy();
  std::vector<llvm::Type *> parameters;
  if (aggregateResult)
    parameters.push_back(pointer);
  for (auto *type : target.getFunctionType()->params())
    parameters.push_back(type->isStructTy() ? pointer : type);
  auto *bridge = llvm::Function::Create(llvm::FunctionType::get(aggregateResult
                                                                    ? llvm::Type::getVoidTy(context)
                                                                    : target.getReturnType(),
                                                                parameters, false),
                                        llvm::GlobalValue::ExternalLinkage, name, *module);
  // Pointer adaptation adds loads/stores. Memory/capture attributes from the
  // original by-value signature are not valid promises on the new function.
  for (auto attribute : {llvm::Attribute::ZExt, llvm::Attribute::SExt}) {
    if (!aggregateResult && target.hasRetAttribute(attribute))
      bridge->addRetAttr(attribute);
    for (unsigned n = 0; n < target.arg_size(); ++n)
      if (!target.getFunctionType()->getParamType(n)->isStructTy() &&
          target.hasParamAttribute(n, attribute))
        bridge->addParamAttr(n + unsigned(aggregateResult), attribute);
  }
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", bridge));
  std::vector<llvm::Value *> args;
  for (unsigned n = 0; n < target.arg_size(); ++n) {
    auto *type = target.getFunctionType()->getParamType(n);
    auto *argument = bridge->getArg(n + unsigned(aggregateResult));
    args.push_back(type->isStructTy() ? builder.CreateLoad(type, argument)
                                      : static_cast<llvm::Value *>(argument));
  }
  auto *result = builder.CreateCall(&target, args);
  result->setCallingConv(target.getCallingConv());
  result->setAttributes(target.getAttributes());
  if (aggregateResult) {
    builder.CreateStore(result, bridge->getArg(0));
    builder.CreateRetVoid();
  } else if (target.getReturnType()->isVoidTy())
    builder.CreateRetVoid();
  else
    builder.CreateRet(result);
  return bridge;
}
} // namespace kalandra
