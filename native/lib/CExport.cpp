#include "kalandra/Backend.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace kalandra {
static std::expected<BackendOutput, std::string> lowerCEntries(const TypedModule &input,
                                                               CpuTarget target, Optimization level,
                                                               const llvm::StringSet<> *selected) {
  std::vector<std::string> roots;
  std::vector<const TypedFunction *> functions;
  for (const auto &fn : input.functions) {
    if (fn.cExportName.empty() || (selected && !selected->contains(fn.cExportName)))
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
    // A plain C signature has no channel for reporting execution faults.
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
    auto *recoverable = module->getFunction(prefix + "_ordinary_pair_v1");
    if (fn.checkedCExport &&
        (!body || !recoverable || !recoverable->hasFnAttribute("kalandra.ordinary.pair")))
      return std::unexpected("checked C export '" + fn.cExportName +
                             "' has no recoverable scalar native entry");
    if (!fn.checkedCExport && (!body || !pure(*body)))
      return std::unexpected("C export '" + fn.cExportName +
                             "' has no proven fault-free native entry");
    if (module->getNamedValue(fn.cExportName))
      return std::unexpected("C export conflicts with a compiler symbol: " + fn.cExportName);
    const bool unit = fn.resultType.builtin == BuiltinValue::Unit;
    auto nativeParams = body->getFunctionType()->params();
    llvm::SmallVector<llvm::Type *> parameters(nativeParams.begin(), nativeParams.end());
    if (fn.checkedCExport && !unit)
      parameters.push_back(llvm::PointerType::getUnqual(context));
    auto resultType = fn.checkedCExport ? llvm::Type::getInt32Ty(context)
                      : unit            ? llvm::Type::getVoidTy(context)
                                        : body->getReturnType();
    auto *type = llvm::FunctionType::get(resultType, parameters, false);
    auto *entry =
        llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, fn.cExportName, *module);
    if (module->getTargetTriple().isOSWindows())
      entry->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    // C promotes narrow scalar arguments and results in registers on Darwin
    // and SysV x86-64. Windows x64 only extends bool, and AAPCS64 leaves the
    // upper bits unspecified. Match Clang's ABI for the destination.
    const auto &triple = module->getTargetTriple();
    const auto extension = [&](llvm::Type *irType, const ValueType &type) {
      if (!irType->isIntegerTy() || irType->getIntegerBitWidth() >= 32)
        return llvm::Attribute::None;
      const bool boolean = type.builtin == BuiltinValue::Bool;
      if (!triple.isOSDarwin() &&
          !(triple.getArch() == llvm::Triple::x86_64 && (!triple.isOSWindows() || boolean)))
        return llvm::Attribute::None;
      return boolean || isUnsigned(type.element) ? llvm::Attribute::ZExt : llvm::Attribute::SExt;
    };
    for (unsigned arg = 0; arg < fn.parameters.size(); ++arg) {
      const auto attr = extension(type->getParamType(arg), input.symbols[fn.parameters[arg]].type);
      if (attr != llvm::Attribute::None)
        entry->addParamAttr(arg, attr);
    }
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", entry));
    llvm::SmallVector<llvm::Value *> args;
    for (unsigned arg = 0; arg < fn.parameters.size(); ++arg)
      args.push_back(entry->getArg(arg));
    if (fn.checkedCExport) {
      if (!unit) {
        auto *invalid = llvm::BasicBlock::Create(context, "invalid.output", entry);
        auto *invoke = llvm::BasicBlock::Create(context, "invoke", entry);
        builder.CreateCondBr(builder.CreateIsNull(entry->getArg(args.size())), invalid, invoke);
        builder.SetInsertPoint(invalid);
        builder.CreateRet(builder.getInt32(2));
        builder.SetInsertPoint(invoke);
      }
      // The ordinary pair carries status independently of every result bit,
      // including a successful NaN or signed minimum. Fault location storage
      // is local to this call. No host abort or thread-local state is needed.
      auto *fault = builder.CreateAlloca(llvm::ArrayType::get(builder.getInt64Ty(), 3));
      args.push_back(fault);
      auto *pair = builder.CreateCall(recoverable, args);
      pair->setCallingConv(recoverable->getCallingConv());
      auto *code = builder.CreateExtractValue(pair, 1);
      auto *failed = llvm::BasicBlock::Create(context, "fault", entry);
      auto *succeeded = llvm::BasicBlock::Create(context, "success", entry);
      builder.CreateCondBr(builder.CreateICmpNE(code, builder.getInt64(0)), failed, succeeded);
      builder.SetInsertPoint(failed);
      builder.CreateRet(builder.getInt32(1));
      builder.SetInsertPoint(succeeded);
      if (!unit) {
        auto *scalar = body->getReturnType();
        auto *bits = builder.CreateTruncOrBitCast(builder.CreateExtractValue(pair, 0),
                                                  builder.getIntNTy(scalar->getScalarSizeInBits()));
        auto *value = builder.CreateBitCast(bits, scalar);
        if (scalar->isIntegerTy(1))
          value = builder.CreateZExt(value, builder.getInt8Ty());
        builder.CreateStore(value, entry->getArg(fn.parameters.size()));
      }
      builder.CreateRet(builder.getInt32(0));
      published.insert(entry);
      continue;
    }
    auto *result = builder.CreateCall(body, args);
    result->setCallingConv(body->getCallingConv());
    result->setAttributes(body->getAttributes().removeFnAttributes(context));
    if (unit)
      builder.CreateRetVoid();
    else {
      const auto attr = extension(type->getReturnType(), fn.resultType);
      if (attr != llvm::Attribute::None)
        entry->addRetAttr(attr);
      builder.CreateRet(result);
    }
    published.insert(entry);
  }
  // Only these simple entries survive. Their bodies have no invocation or
  // scheduler dependency, so the compiler-private runtime layout anchor is unnecessary.
  llvm::removeFromUsedLists(*module, [](llvm::Constant *value) {
    return value->getName() == "kalandra.required.invocation.layout";
  });
  llvm::internalizeModule(
      *module, [&](const llvm::GlobalValue &value) { return published.contains(&value); });
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
std::expected<BackendOutput, std::string> lowerCLibrary(const TypedModule &input, CpuTarget target,
                                                        Optimization level) {
  return lowerCEntries(input, std::move(target), level, nullptr);
}

// Materialize a Clang block with by-value scalar fields. The native adapter
// copies the block when retaining it; the block never points into a Kalandra frame.
static std::optional<std::string> emitNativeBlock(llvm::Module &module, const ForeignCall &call,
                                                  const TypedFunction &source) {
  using namespace llvm;
  auto *registration = module.getFunction(call.symbol);
  if (!registration)
    return {}; // Dead registration.
  if (!registration->isDeclaration())
    return "duplicate native block registration entry";
  auto &ctx = module.getContext();
  IRBuilder<> b(ctx);
  auto *body = module.getFunction("kalandra_c_callback_" + source.cExportName);
  if (!body || source.captureCount > body->arg_size())
    return "missing native block invocation entry";
  const auto captureCount = source.captureCount, firstCapture = body->arg_size() - captureCount;
  const auto position = call.block->position;
  if (position + captureCount > registration->arg_size())
    return "invalid native block registration signature";
  auto *ptr = b.getPtrTy();
  if (call.block->byValue) {
    b.SetInsertPoint(BasicBlock::Create(ctx, "entry", registration));
    SmallVector<Value *> args{body};
    SmallVector<Type *> types{ptr};
    for (auto &value : registration->args()) {
      args.push_back(&value);
      types.push_back(value.getType());
    }
    auto callee = module.getOrInsertFunction(
        call.block->callee, FunctionType::get(registration->getReturnType(), types, false));
    b.CreateRet(b.CreateCall(callee, args));
    registration->setLinkage(GlobalValue::InternalLinkage);
    return {};
  }
  SmallVector<Type *> fields{ptr, b.getInt32Ty(), b.getInt32Ty(), ptr, ptr};
  SmallVector<Type *> invokeParams{ptr};
  for (auto &arg : body->args()) {
    if (arg.getArgNo() < firstCapture)
      invokeParams.push_back(arg.getType());
    else
      fields.push_back(arg.getType());
  }
  auto *layout = StructType::get(ctx, fields);
  auto *invoke =
      llvm::Function::Create(FunctionType::get(body->getReturnType(), invokeParams, false),
                             GlobalValue::InternalLinkage, "kalandra.block.invoke", module);
  b.SetInsertPoint(BasicBlock::Create(ctx, "entry", invoke));
  SmallVector<Value *> operands;
  for (auto &arg : invoke->args())
    if (arg.getArgNo())
      operands.push_back(&arg);
  for (unsigned i = 0; i < captureCount; ++i)
    operands.push_back(
        b.CreateLoad(fields[5 + i], b.CreateStructGEP(layout, invoke->getArg(0), 5 + i)));
  auto *result = b.CreateCall(body, operands);
  if (result->getType()->isVoidTy())
    b.CreateRetVoid();
  else
    b.CreateRet(result);

  b.SetInsertPoint(BasicBlock::Create(ctx, "entry", registration));
  auto *literal = b.CreateAlloca(layout);
  auto store = [&](unsigned index, Value *v) {
    b.CreateStore(v, b.CreateStructGEP(layout, literal, index));
  };
  auto *isa = module.getOrInsertGlobal("_NSConcreteStackBlock", ptr);
  if (module.getTargetTriple().isOSWindows())
    llvm::cast<GlobalVariable>(isa)->setDLLStorageClass(GlobalValue::DLLImportStorageClass);
  store(0, isa);
  store(1, b.getInt32(0));
  store(2, b.getInt32(0));
  store(3, invoke);
  // Clang uses C unsigned long here (LLP64 on Windows).
  auto *sizeType = module.getTargetTriple().isOSWindows() ? b.getInt32Ty() : b.getInt64Ty();
  auto bytes = module.getDataLayout().getTypeAllocSize(layout).getFixedValue();
  auto *description =
      ConstantStruct::getAnon({ConstantInt::get(sizeType, 0), ConstantInt::get(sizeType, bytes)});
  auto *descriptor =
      new GlobalVariable(module, description->getType(), true, GlobalValue::PrivateLinkage,
                         description, "kalandra.block.descriptor");
  store(4, descriptor);
  SmallVector<Value *> nativeArgs;
  for (unsigned i = 0; i < position; ++i)
    nativeArgs.push_back(registration->getArg(i));
  nativeArgs.push_back(literal);
  for (unsigned i = 0; i < captureCount; ++i) {
    auto *arg = registration->getArg(position + i);
    if (arg->getType() != fields[5 + i])
      return "native block captured argument type mismatch";
    store(5 + i, arg);
  }
  for (unsigned i = position + captureCount; i < registration->arg_size(); ++i)
    nativeArgs.push_back(registration->getArg(i));
  SmallVector<Type *> signature;
  for (auto *arg : nativeArgs)
    signature.push_back(arg->getType());
  auto callee = module.getOrInsertFunction(
      call.block->callee, FunctionType::get(registration->getReturnType(), signature, false));
  auto *registered = b.CreateCall(callee, nativeArgs);
  if (registered->getType()->isVoidTy())
    b.CreateRetVoid();
  else
    b.CreateRet(registered);
  registration->setLinkage(GlobalValue::InternalLinkage);
  return {};
}

// Pass addresses of private C entries to native code. Reuse the library ABI
// and its fault proof without exporting names into the process namespace.
std::optional<std::string> linkNativeCallbacks(BackendOutput &app, const TypedModule &input,
                                               CpuTarget target, Optimization level) {
  llvm::StringSet<> selected;
  std::vector<const ForeignCall *> blocks;
  std::optional<TypedModule> blockInput;
  for (const auto &expr : input.expressions)
    if (auto *call = std::get_if<ForeignCall>(&expr.operation)) {
      if (call->block) {
        if (!blockInput)
          blockInput.emplace(input);
        const auto id = call->block->function;
        auto &fn = blockInput->functions[id];
        if (fn.checkedCExport)
          return "a native block requires a proven fault-free function";
        if (fn.cExportName.empty())
          fn.cExportName = "kalandra.block.body." + std::to_string(id);
        selected.insert(fn.cExportName);
        blocks.push_back(call);
      }
      for (const auto &callback : call->callbacks)
        if (!callback.empty())
          selected.insert(callback);
    }
  if (selected.empty())
    return {};
  auto exports =
      lowerCEntries(blockInput ? *blockInput : input, std::move(target), level, &selected);
  if (!exports)
    return exports.error();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto application = llvm::parseAssemblyString(app.llvm, diagnostic, context);
  auto callbacks = llvm::parseAssemblyString(exports->llvm, diagnostic, context);
  if (!application || !callbacks)
    return "invalid native IR linking application C exports";
  for (const auto &item : selected) {
    auto *entry = callbacks->getFunction(item.getKey());
    if (!entry)
      return "missing proven native callback entry";
    entry->setName("kalandra_c_callback_" + item.getKey());
    entry->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
    auto *decl = application->getFunction(entry->getName());
    if (decl && (!decl->isDeclaration() || decl->getFunctionType() != entry->getFunctionType()))
      return "native callback prototype disagrees with the proven C entry";
  }
  if (llvm::Linker::linkModules(*application, std::move(callbacks)))
    return "cannot link callbacks into the application";
  for (const auto *block : blocks)
    if (auto error =
            emitNativeBlock(*application, *block, blockInput->functions[block->block->function]))
      return error;
  for (const auto &item : selected)
    if (auto *entry = application->getFunction(("kalandra_c_callback_" + item.getKey()).str()))
      entry->setLinkage(llvm::GlobalValue::InternalLinkage);
  if (llvm::verifyModule(*application, &llvm::errs()))
    return "invalid application callback IR";
  app.llvm.clear();
  llvm::raw_string_ostream stream(app.llvm);
  application->print(stream, nullptr);
  return {};
}
} // namespace kalandra
