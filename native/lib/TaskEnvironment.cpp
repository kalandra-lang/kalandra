#include "TaskEnvironment.h"
#include "RecursiveRecordOps.h"
#include "RecursiveTypeKey.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "llvm/Linker/Linker.h"
#include <stdexcept>
namespace kalandra {
std::optional<std::string> addCheckedInvocationABI(llvm::Module &, bool);
std::map<std::size_t, llvm::Function *>
emitTaskEnvironmentBoxes(const TypedModule &input, std::span<const std::size_t> targets,
                         llvm::Module &destination) {
  if (targets.empty())
    return {};
  TypedModule ownership;
  ownership.records = input.records;
  std::map<std::size_t, std::size_t> records;
  for (auto id : targets) {
    const auto &function = input.functions.at(id);
    if (!function.captureCount || function.captureCount > function.parameters.size())
      throw std::runtime_error("invalid task capture metadata");
    TypedRecord environment;
    environment.name = "$task_environment_" + std::to_string(id);
    for (auto n = function.parameters.size() - function.captureCount;
         n < function.parameters.size(); ++n) {
      auto type = input.symbols.at(function.parameters[n]).type;
      if (!workerClosureCaptureTypeSupported(input, type))
        throw std::runtime_error("unsupported managed task capture");
      environment.fields.push_back({"capture_" + std::to_string(n), type, {}});
    }
    records.emplace(id, ownership.records.size());
    ownership.records.push_back(std::move(environment));
  }
  mlir::DialectRegistry registry;
  registry.insert<mlir::LLVM::LLVMDialect>();
  mlir::registerBuiltinDialectTranslation(registry);
  mlir::registerLLVMDialectTranslation(registry);
  mlir::MLIRContext context(registry);
  context.loadAllAvailableDialects();
  mlir::OpBuilder builder(&context);
  const auto loc = builder.getUnknownLoc();
  mlir::OwningOpRef<mlir::ModuleOp> module(mlir::ModuleOp::create(loc));
  auto types = lowerRecordTypes(ownership, context);
  auto storage = planRecordStorage(ownership);
  if (!types || !storage)
    throw std::runtime_error("task environment physical planning failed");
  RecursiveRecordOps boxes(ownership, *storage, *types, builder, *module);
  const auto pointer = mlir::LLVM::LLVMPointerType::get(&context);
  std::map<std::size_t, std::string> names;
  for (auto [id, record] : records) {
    auto name = "kalandra_task_environment_" + std::to_string(id);
    while (destination.getNamedValue(name))
      name += "_";
    names.emplace(id, name);
    builder.setInsertionPointToEnd(module->getBody());
    auto helper = mlir::LLVM::LLVMFuncOp::create(
        builder, loc, name, mlir::LLVM::LLVMFunctionType::get(pointer, {pointer}));
    auto *entry = helper.addEntryBlock(builder);
    builder.setInsertionPointToStart(entry);
    auto value = mlir::LLVM::LoadOp::create(builder, loc, (*types)[record], entry->getArgument(0));
    auto boxed = boxes.box(loc, value, record);
    if (!boxed)
      throw std::runtime_error(boxed.error());
    mlir::LLVM::ReturnOp::create(builder, loc, mlir::ValueRange{*boxed});
  }
  if (auto error = boxes.finish())
    throw std::runtime_error(*error);
  if (mlir::failed(mlir::verify(*module)))
    throw std::runtime_error("task environment MLIR verification failed");
  auto native = mlir::translateModuleToLLVMIR(*module, destination.getContext());
  if (!native)
    throw std::runtime_error("task environment LLVM translation failed");
  if (auto error = addCheckedInvocationABI(*native, false))
    throw std::runtime_error(*error);
  // Original nominal identities must resolve to the destination's tokens.
  // Synthetic environment identities remain private to this helper module.
  std::vector<llvm::GlobalVariable *> shared;
  for (std::size_t id = 0; id < input.records.size(); ++id)
    if (auto *token = native->getNamedGlobal("kalandra_recursive_type_" + std::to_string(id))) {
      auto *canonical = recursiveTypeKey(destination, id);
      token->setInitializer(nullptr);
      token->setLinkage(llvm::GlobalValue::ExternalLinkage);
      canonical->setLinkage(llvm::GlobalValue::ExternalLinkage);
      shared.push_back(canonical);
    }
  // Internal symbols are renamed by LLVM's linker when another helper already
  // owns the same synthetic ID; their addresses must never be merged by value.
  const bool failed = llvm::Linker::linkModules(destination, std::move(native));
  for (auto *token : shared)
    token->setLinkage(llvm::GlobalValue::InternalLinkage);
  if (failed)
    throw std::runtime_error("task environment helper linkage failed");
  std::map<std::size_t, llvm::Function *> result;
  for (const auto &[id, name] : names) {
    auto *function = destination.getFunction(name);
    function->setLinkage(llvm::GlobalValue::InternalLinkage);
    result.emplace(id, function);
  }
  return result;
}
} // namespace kalandra
