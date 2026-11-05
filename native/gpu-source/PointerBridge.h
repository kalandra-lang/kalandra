#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <stdexcept>
#include <vector>

namespace kalandra {
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
