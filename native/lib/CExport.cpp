#include "kalandra/Backend.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace kalandra {
std::expected<BackendOutput, std::string> lowerCLibrary(const TypedModule &input,
                                                         CpuTarget target, Optimization level) {
  std::vector<std::string> roots;
  std::vector<const TypedFunction *> functions;
  for (const auto &fn : input.functions) {
    if (fn.cExportName.empty())
      continue;
    roots.push_back(fn.name);
    functions.push_back(&fn);
  }
  if (roots.empty())
    return std::unexpected("the library has no export \"C\" functions");
  auto output = lowerNative(input, {}, roots, ArithmeticMode::Hardware, false, target, level);
  if (!output)
    return output;
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(output->llvm, diagnostic, context);
  if (!module)
    return std::unexpected("invalid internal library IR");

  llvm::SmallPtrSet<const llvm::GlobalValue *, 16> published;
  for (std::size_t i = 0; i < functions.size(); ++i) {
    const auto &fn = *functions[i];
    const auto prefix = "_mlir_ciface_kalandra_cpu_" + std::to_string(i);
    auto *body = module->getFunction(prefix + "_ordinary_direct_v1");
    // A direct C signature cannot carry Kalandra faults. Only publish a proven
    // context-free body with no remaining fault exit or foreign effects.
    const auto pure = [](const llvm::Function &body) {
      for (const auto &block : body)
        for (const auto &inst : block) {
          if (llvm::isa<llvm::UnreachableInst>(inst))
            return false;
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
            auto *callee = call->getCalledFunction();
            if (!callee || !callee->isIntrinsic() || !callee->doesNotAccessMemory() ||
                callee->hasFnAttribute(llvm::Attribute::NoReturn))
              return false;
          }
        }
      return true;
    };
    if (!body || !pure(*body))
      return std::unexpected("C export '" + fn.cExportName +
                             "' has no proven fault-free native entry");
    if (module->getNamedValue(fn.cExportName))
      return std::unexpected("C export conflicts with a compiler symbol: " + fn.cExportName);
    const bool unit = fn.resultType.builtin == BuiltinValue::Unit;
    auto *type = llvm::FunctionType::get(unit ? llvm::Type::getVoidTy(context) : body->getReturnType(),
                                         body->getFunctionType()->params(), false);
    auto *entry = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                          fn.cExportName, *module);
    if (module->getTargetTriple().isOSWindows())
      entry->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", entry));
    llvm::SmallVector<llvm::Value *> args;
    for (auto &arg : entry->args())
      args.push_back(&arg);
    auto *result = builder.CreateCall(body, args);
    result->setCallingConv(body->getCallingConv());
    result->setAttributes(body->getAttributes().removeFnAttributes(context));
    if (unit)
      builder.CreateRetVoid();
    else {
      // Narrow results carry the platform-visible sign/zero extension.
      if (type->getReturnType()->isIntegerTy() && type->getReturnType()->getIntegerBitWidth() < 32)
        entry->addRetAttr(fn.resultType.builtin == BuiltinValue::Bool || isUnsigned(fn.resultType.element)
                              ? llvm::Attribute::ZExt : llvm::Attribute::SExt);
      builder.CreateRet(result);
    }
    published.insert(entry);
  }
  // Only these simple entries survive. Their bodies have no invocation or
  // scheduler dependency, so the compiler-private runtime layout anchor is unnecessary.
  llvm::removeFromUsedLists(*module, [](llvm::Constant *value) {
    return value->getName() == "kalandra.required.invocation.layout";
  });
  llvm::internalizeModule(*module, [&](const llvm::GlobalValue &value) {
    return published.contains(&value);
  });
  llvm::ModuleAnalysisManager analyses;
  llvm::PassBuilder passes;
  passes.registerModuleAnalyses(analyses);
  llvm::GlobalDCEPass().run(*module, analyses);
  if (llvm::verifyModule(*module, &llvm::errs()))
    return std::unexpected("invalid C library IR");
  output->llvm.clear();
  llvm::raw_string_ostream stream(output->llvm);
  module->print(stream, nullptr);
  return output;
}
} // namespace kalandra
