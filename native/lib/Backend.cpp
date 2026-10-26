#include "kalandra/Backend.h"
#include "CollectionMetadata.h"
#include "NativeLayout.h"
#include "NativeStack.h"
#include "NativeCallFoundation.h"
#include "LocalStorage.h"
#include "IntegerRanges.h"
#include "BoundedEntry.h"
#include "../runtime/Runtime.h"
#include "RecordLLVMTypes.h"
#include "RecordLayout.h"
#include "RecursiveAdmission.h"
#include "RecursiveRecordOps.h"
#include "kalandra/NumericConversion.h"
#include "kalandra/StrictFloat.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Target/LLVMIR/Dialect/All.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Target/LLVMIR/TypeToLLVM.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>

namespace kalandra {
std::expected<BackendOutput, std::string>
lowerTaskNative(const TypedModule &, std::optional<std::string>, ArithmeticMode);
std::optional<std::string> addCheckedInvocationABI(llvm::Module &module, bool functionValues);
std::optional<std::string> optimizeNativeCalls(std::unique_ptr<llvm::Module> &module) {
  // Fault exits are already explicit: optimizing unchecked fault helpers could
  // execute operations after failure. Optimize the complete internal call graph
  // now, before sealing the final physical-frame boundaries for code generation.
  finalizeNativeCallResolvers(*module);
  auto candidate = llvm::CloneModule(*module);
  // Task descriptors are also trusted diagnostic/test entry points. Keep their
  // identities when optimizing the composed module, including unselected roots.
  llvm::SmallVector<llvm::GlobalValue *, 16> descriptors;
  for (auto &global : candidate->globals())
    if (global.getName().starts_with("task.entry.") ||
        global.getName().starts_with("value.layout.")) descriptors.push_back(&global);
  if (!descriptors.empty()) llvm::appendToCompilerUsed(*candidate, descriptors);
  // Preserve the separately lowered task-controller IR. Its frame/state
  // layouts and checked entries have their own admission and optimization
  // policy; only synchronous native bodies and their adapters are optimized.
  for (auto &fn : *candidate)
    if (fn.hasFnAttribute("kalandra.task.frame")) {
      unsigned original = unsigned(fn.hasFnAttribute(llvm::Attribute::NoInline)) |
                          (unsigned(fn.hasFnAttribute(llvm::Attribute::OptimizeNone)) << 1) |
                          (unsigned(fn.hasFnAttribute(llvm::Attribute::AlwaysInline)) << 2);
      fn.addFnAttr("kalandra.task.preserve", std::to_string(original));
      fn.removeFnAttr(llvm::Attribute::AlwaysInline);
      fn.addFnAttr(llvm::Attribute::NoInline);
      fn.addFnAttr(llvm::Attribute::OptimizeNone);
    }
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder passes;
  passes.registerLoopAnalyses(loops);
  passes.registerFunctionAnalyses(functions);
  passes.registerCGSCCAnalyses(calls);
  passes.registerModuleAnalyses(modules);
  passes.crossRegisterProxies(loops, functions, calls, modules);
  auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  pipeline.run(*candidate, modules);
  auto simplifyLeaf = [](llvm::Function &fn, bool) {
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager calls;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder passes;
    passes.registerLoopAnalyses(loops);
    passes.registerFunctionAnalyses(functions);
    passes.registerCGSCCAnalyses(calls);
    passes.registerModuleAnalyses(modules);
    passes.crossRegisterProxies(loops, functions, calls, modules);
    auto simplify = passes.buildFunctionSimplificationPipeline(llvm::OptimizationLevel::O2,
                                                               llvm::ThinOrFullLTOPhase::None);
    simplify.run(fn, functions);
  };
  if (removeInlinedNativeStackGuards(*candidate))
    for (auto &fn : *candidate)
      if (!fn.isDeclaration() && !fn.hasFnAttribute("kalandra.task.frame"))
        simplifyLeaf(fn, true);
  outlineNativeLeafPaths(*candidate, simplifyLeaf);
  modules.clear();
  auto finish = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
  finish.run(*candidate, modules);
  // The complete graph pipeline can expose a local environment only on its
  // final inlining round. Promote afterwards, then replace its loads/stores
  // with SSA values without re-running unrelated module optimizations.
  if (promoteLocalEnvironments(*candidate, runtime::InvocationABI::pollMode))
    for (auto &fn : *candidate) {
      bool local = false;
      if (!fn.isDeclaration())
        for (auto &instruction : fn.getEntryBlock())
          local |= llvm::isa<llvm::AllocaInst>(instruction) &&
                   instruction.getName().starts_with("local.environment");
      if (local) {
        functions.invalidate(fn, llvm::PreservedAnalyses::none());
        llvm::FunctionPassManager scalarize;
        scalarize.addPass(llvm::SROAPass(llvm::SROAOptions{}));
        scalarize.run(fn, functions);
        simplifyLeaf(fn, true);
      }
    }
  if (removeInlinedNativeStackGuards(*candidate))
    for (auto &fn : *candidate)
      if (!fn.isDeclaration() && !fn.hasFnAttribute("kalandra.task.frame"))
        simplifyLeaf(fn, true);
  for (auto &fn : *candidate) {
    if (fn.isDeclaration() || fn.hasFnAttribute("kalandra.task.frame") ||
        fn.hasFnAttribute(llvm::Attribute::OptimizeNone)) continue;
    // Promotion/simplification above changes value facts. Build fresh analyses
    // rather than consuming a cached range result from the module pipeline.
    llvm::FunctionAnalysisManager ranges;
    passes.registerFunctionAnalyses(ranges);
    auto &lvi = ranges.getResult<llvm::LazyValueAnalysis>(fn);
    auto &dom = ranges.getResult<llvm::DominatorTreeAnalysis>(fn);
    const bool changed = foldProvenIntegerComparisons(fn, lvi, dom);
    ranges.clear();
    if (changed) simplifyLeaf(fn, true);
  }
  // At the outermost callback there is no source caller left to unwind.
  // Return an explicit boundary failure instead. Keep the bit even if a host
  // resets its cancellation token before the runtime checks final readiness.
  for (auto &fn : *candidate) {
    if (!fn.hasFnAttribute("kalandra.boundary")) continue;
    llvm::SmallVector<llvm::CallInst *, 4> aborts;
    for (auto &block : fn)
      for (auto &instruction : block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
            call && call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "kalandra_source_abort_v2" &&
            llvm::isa<llvm::UnreachableInst>(block.getTerminator())) {
          aborts.push_back(call);
          break;
        }
    for (auto *call : aborts) {
      auto *block = call->getParent();
      auto erase = call->getIterator();
      while (erase != block->end()) erase = erase->eraseFromParent();
      llvm::IRBuilder<> finish(block);
      finish.CreateRet(finish.getFalse());
    }
    if (!aborts.empty()) fn.removeFnAttr(llvm::Attribute::NoReturn);
  }
  addBoundedEntries(*candidate, simplifyLeaf);
  lowerAArch64CheckedRemainders(*candidate);
  if (auto error = normalizeWindowsTailPackets(*candidate)) return error;
  bool fits = !llvm::verifyModule(*candidate);
  for (auto &fn : *candidate) {
    if (fn.isDeclaration() || fn.hasFnAttribute("kalandra.task.frame")) continue;
    const auto name = fn.getName();
    // The numerical leaf allowance belongs to the computation worker, not
    // its checked entry, callback packet or mode dispatcher.
    const bool numericalWorker = name.starts_with("kalandra_fn_matmul_wide_") &&
        (name.contains(".body.clear") || name.contains(".body.token") ||
         name.contains(".body.observed"));
    fits &= !nativeStackBudgetError(fn, candidate->getDataLayout(), numericalWorker);
  }
  // Optimization must not reject a previously admissible source program.
  if (fits)
    module = std::move(candidate);
  else if (auto error = normalizeWindowsTailPackets(*module)) return error;
  if (module->getTargetTriple().isOSWindows())
    for (auto &fn : *module)
      if (!fn.isDeclaration() && fn.getCallingConv() == llvm::CallingConv::Tail)
        if (auto error = nativeStackBudgetError(fn, module->getDataLayout(),
              fn.getName().starts_with("kalandra_fn_matmul_wide_") &&
              (fn.getName().contains(".body.clear") || fn.getName().contains(".body.token") ||
               fn.getName().contains(".body.observed")))) return error;
  for (auto &fn : *module)
    if (!fn.isDeclaration() && !fn.hasFnAttribute("kalandra.task.frame") &&
        !fn.hasFnAttribute(llvm::Attribute::AlwaysInline))
      fn.addFnAttr(llvm::Attribute::NoInline);
  // The next Clang pipeline may optimize task helpers under their own policy.
  // Keep their lifetime/admission/TLS bookkeeping OUTSIDE abandonable frames,
  // even when an internal adapter was inlined during this module's pipeline.
  // Call-site barriers preserve inlining among non-source task helpers.
  if (module->getTargetTriple().isOSDarwin() && module->getTargetTriple().isAArch64())
    for (auto &fn : *module)
      if (!fn.isDeclaration() &&
          (fn.hasFnAttribute("kalandra.native.pure-frame") || fn.hasFnAttribute("kalandra.boundary")))
        for (auto &block : fn)
          for (auto &instruction : block)
            if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
              if (auto *callee = call->getCalledFunction(); callee && !callee->isDeclaration() &&
                  callee->hasFnAttribute("kalandra.task.frame"))
                call->addFnAttr(llvm::Attribute::NoInline);
  for (auto &fn : *module) {
    if (fn.hasFnAttribute("kalandra.task.preserve")) {
      unsigned original = 0;
      fn.getFnAttribute("kalandra.task.preserve").getValueAsString().getAsInteger(10, original);
      if (!(original & 1)) fn.removeFnAttr(llvm::Attribute::NoInline);
      if (!(original & 2)) fn.removeFnAttr(llvm::Attribute::OptimizeNone);
      if (original & 4) fn.addFnAttr(llvm::Attribute::AlwaysInline);
      fn.removeFnAttr("kalandra.task.preserve");
    }
    fn.removeFnAttr("kalandra.task.frame");
    if (!fn.isDeclaration() && module->getTargetTriple().isOSDarwin() &&
        module->getTargetTriple().isAArch64() &&
        (fn.hasFnAttribute("kalandra.native.pure-frame") ||
         fn.hasFnAttribute("kalandra.boundary"))) {
      // The linker supplies exact section limits, including final prologues,
      // epilogues and optimizer-created fragments. Never whitelist runtime or
      // task-controller frames by a symbol-name approximation.
      fn.setSection("__TEXT,__kalandra,regular,pure_instructions");
      auto features = fn.getFnAttribute("target-features").getValueAsString().str();
      if (!features.empty()) features += ',';
      fn.addFnAttr("target-features", features + "+reserve-x28");
      fn.addFnAttr(llvm::Attribute::NoRedZone);
    }
  }
  return {};
}
namespace {
using namespace mlir;
struct LoweringFailure {
  std::string message;
};
class Lowerer {
  ArithmeticMode arithmetic_;
  bool arrayTailLoops_;

public:
  Lowerer(const TypedModule &input, MLIRContext &context, ArithmeticMode arithmetic,
          bool arrayTailLoops)
      : arithmetic_(arithmetic), arrayTailLoops_(arrayTailLoops), input_(input), builder_(&context),
        module_(ModuleOp::create(builder_.getUnknownLoc())) {}
  OwningOpRef<ModuleOp> run(const std::optional<std::string> &testEntry,
                            const std::vector<std::string> &exports) {
    recursiveClosures_ = hasRecursiveStorage(*planRecordStorage(input_));
    const TypedModule *ownership = &input_;
    if (recursiveClosures_) {
      ownershipInput_ = input_;
      environmentRecords_.resize(input_.functions.size());
      for (std::size_t id = 0; id < input_.functions.size(); ++id) {
        const auto &function = input_.functions[id];
        if (!function.captureCount)
          continue;
        TypedRecord environment;
        environment.name = "$closure_" + std::to_string(id);
        for (std::size_t i = function.parameters.size() - function.captureCount;
             i < function.parameters.size(); ++i)
          environment.fields.push_back(
              {"capture_" + std::to_string(i), input_.symbols[function.parameters[i]].type, {}});
        environmentRecords_[id] = ownershipInput_->records.size();
        ownershipInput_->records.push_back(std::move(environment));
      }
      ownership = &*ownershipInput_;
    }
    auto records = lowerRecordTypes(*ownership, *builder_.getContext());
    if (!records)
      throw LoweringFailure{records.error().message};
    recordStorage_ = *planRecordStorage(*ownership);
    recursiveTypes_ = *records;
    recursive_ = std::make_unique<RecursiveRecordOps>(*ownership, recordStorage_, recursiveTypes_,
                                                      builder_, *module_);
    for (std::size_t id = 0; id < records->size(); ++id)
      recordTypes_.emplace(id, (*records)[id]);
    builder_.setInsertionPointToEnd(module_->getBody());
    auto fault = func::FuncOp::create(
        builder_, builder_.getUnknownLoc(), "kalandra_fault",
        builder_.getFunctionType(
            {builder_.getI64Type(), builder_.getI64Type(), builder_.getI64Type()}, {}));
    fault.setPrivate();
    if (hasRecordBacking(input_) || hasOrdinaryStrings(input_)) {
      const auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
      for (const auto &[name, signature] : std::vector<std::pair<std::string, FunctionType>>{
               {"kalandra_record_alloc", builder_.getFunctionType({builder_.getI64Type()}, {ptr})},
               {"kalandra_recursive_environment_bind", builder_.getFunctionType({ptr, ptr}, {ptr})},
               {"kalandra_recursive_environment_read",
                builder_.getFunctionType({ptr, builder_.getI64Type(), ptr}, {ptr})},
               {"kalandra_environment_alloc",
                builder_.getFunctionType({builder_.getI64Type(), ptr}, {ptr})},
               {"kalandra_record_cleanup", builder_.getFunctionType({}, {})},
               {"kalandra_record_array_validate",
                builder_.getFunctionType({ptr, builder_.getI64Type(), builder_.getI64Type()}, {})},
               {"kalandra_record_validate",
                builder_.getFunctionType({ptr, builder_.getI64Type(), builder_.getI64Type()},
                                         {})}}) {
        auto fn = func::FuncOp::create(builder_, builder_.getUnknownLoc(), name, signature);
        fn.setPrivate();
      }
    }
    if (hasOrdinaryStrings(input_)) {
      const auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
      for (const auto &[name, signature] : std::vector<std::pair<std::string, FunctionType>>{
               {"kalandra_string_literal",
                builder_.getFunctionType({ptr, ptr, builder_.getI64Type()}, {})},
               {"kalandra_string_validate", builder_.getFunctionType({ptr}, {})},
               {"kalandra_string_array_validate", builder_.getFunctionType({ptr}, {})},
               {"kalandra_string_slice",
                builder_.getFunctionType({ptr, ptr, builder_.getI64Type(), builder_.getI64Type()},
                                         {})},
               {"kalandra_string_index",
                builder_.getFunctionType({ptr, ptr, builder_.getI64Type()}, {})},
               {"kalandra_string_length", builder_.getFunctionType({ptr}, {builder_.getI64Type()})},
               {"kalandra_string_byte_length",
                builder_.getFunctionType({ptr}, {builder_.getI64Type()})},
               {"kalandra_string_concat", builder_.getFunctionType({ptr, ptr, ptr}, {})},
               {"kalandra_string_equal",
                builder_.getFunctionType({ptr, ptr}, {builder_.getI64Type()})}}) {
        auto fn = func::FuncOp::create(builder_, builder_.getUnknownLoc(), name, signature);
        fn.setPrivate();
      }
    }
    // Explicit bridge exports define the CPU reachability roots. Empty slots
    // preserve interface ordinals for a mixed backend without lowering its
    // GPU-only numerical functions onto the CPU.
    std::vector<bool> reachable(input_.functions.size(), exports.empty());
    std::vector<FunctionId> pending;
    if (!exports.empty())
      for (const auto &name : exports) {
        if (name.empty())
          continue;
        for (FunctionId id = 0; id < input_.functions.size(); ++id)
          if (input_.functions[id].name == name)
            pending.push_back(id);
      }
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      if (reachable.at(id))
        continue;
      reachable[id] = true;
      for (auto value : input_.functions[id].expressions) {
        const auto &op = input_.expressions.at(value).operation;
        if (auto call = std::get_if<FunctionCall>(&op))
          pending.push_back(call->function);
        else if (auto map = std::get_if<ArrayMap>(&op))
          pending.push_back(map->function);
        else if (auto reference = std::get_if<FunctionReference>(&op))
          pending.push_back(reference->function);
        else if (std::holds_alternative<FunctionInvoke>(op)) {
          // Open callable flow remains conservative; do not drop a potential
          // CPU callback merely because it is not a direct call edge.
          std::fill(reachable.begin(), reachable.end(), true);
          pending.clear();
          break;
        }
      }
    }
    functions_.resize(input_.functions.size());
    for (FunctionId id = 0; id < input_.functions.size(); ++id) {
      if (!reachable[id])
        continue;
      const auto &function = input_.functions[id];
      SmallVector<Type> parameters;
      for (auto parameter : function.parameters)
        parameters.push_back(type(input_.symbols[parameter].type));
      const auto result = type(function.declaredResult.value_or(function.resultType));
      functions_[id] = func::FuncOp::create(builder_, location(function.declaration),
                                            "kalandra_fn_" + std::to_string(id),
                                            builder_.getFunctionType(parameters, {result}));
    }
    for (FunctionId id = 0; id < input_.functions.size(); ++id) {
      if (!reachable[id])
        continue;
      if (input_.functions[id].name.starts_with("__step_"))
        functions_[id]->setAttr("llvm.emit_c_interface", builder_.getUnitAttr());
      body(id);
    }
    if (auto error = recursive_->finish())
      throw LoweringFailure{*error};
    if (!exports.empty()) {
      for (std::size_t i = 0; i < exports.size(); ++i)
        if (!exports[i].empty())
          interfaceWrapper(exports[i], "kalandra_cpu_" + std::to_string(i));
      return std::move(module_);
    }
    auto main =
        std::ranges::find_if(input_.functions, [](const auto &f) { return f.name == "main"; });
    if (main == input_.functions.end() || !main->parameters.empty() ||
        (main->resultType.builtin != BuiltinValue::Numeric &&
         main->resultType.builtin != BuiltinValue::Bool &&
         main->resultType.builtin != BuiltinValue::String) ||
        (main->resultType.tensor && main->resultType.shape.size() > cli::maximumCollectionRank))
      throw LoweringFailure{
          "native entry requires a parameterless main returning a supported CPU scalar, "
          "String or numeric array/tensor"};
    builder_.setInsertionPointToEnd(module_->getBody());
    const auto entryName =
        main->resultType.tensor                            ? "kalandra_collection_entry"
        : main->resultType.builtin == BuiltinValue::String ? "kalandra_string_entry"
        : main->resultType.builtin == BuiltinValue::Bool   ? "kalandra_bool_entry"
        : main->resultType.element == NumericType::Float64 ? "kalandra_float64_entry"
        : (main->resultType.element == NumericType::UInt64 ||
           main->resultType.element == NumericType::UInt8 ||
           main->resultType.element == NumericType::UInt16)
            ? "kalandra_uint64_entry"
        : (main->resultType.element == NumericType::Int64 ||
           main->resultType.element == NumericType::Int8 ||
           main->resultType.element == NumericType::Int16)
            ? "kalandra_int64_entry"
        : main->resultType.element == NumericType::UInt32 ? "kalandra_uint32_entry"
        : main->resultType.element == NumericType::Int32  ? "kalandra_int32_entry"
                                                          : "kalandra_entry";
    auto entry = func::FuncOp::create(
        builder_, location(main->declaration), entryName,
        builder_.getFunctionType(
            {}, {isNarrowInteger(main->resultType.element) && !main->resultType.tensor
                     ? Type(builder_.getI64Type())
                     : type(main->declaredResult.value_or(main->resultType))}));
    entry->setAttr("llvm.emit_c_interface", builder_.getUnitAttr());
    builder_.setInsertionPointToStart(entry.addEntryBlock());
    const auto mainId = static_cast<FunctionId>(main - input_.functions.begin());
    auto call = func::CallOp::create(builder_, entry.getLoc(), functions_[mainId], ValueRange{});
    if ((hasRecordBacking(input_) || hasOrdinaryStrings(input_)) && !main->resultType.tensor &&
        main->resultType.builtin != BuiltinValue::String)
      func::CallOp::create(builder_, entry.getLoc(), "kalandra_record_cleanup", TypeRange{},
                           ValueRange{});
    if (!main->resultType.tensor && isNarrowInteger(main->resultType.element)) {
      Value result = call.getResult(0);
      if (isUnsigned(main->resultType.element))
        result = arith::ExtUIOp::create(builder_, entry.getLoc(), builder_.getI64Type(), result);
      else
        result = arith::ExtSIOp::create(builder_, entry.getLoc(), builder_.getI64Type(), result);
      func::ReturnOp::create(builder_, entry.getLoc(), ValueRange{result});
    } else
      func::ReturnOp::create(builder_, entry.getLoc(), call.getResults());
    if (testEntry)
      interfaceWrapper(*testEntry);
    return std::move(module_);
  }

private:
  Location location(SourceSpan span) const {
    auto [line, column] = input_.source->location(span.begin);
    return FileLineColLoc::get(builder_.getContext(),
                               input_.source->origin(span.begin).first->path(), line, column);
  }
  Type type(const ValueType &value) {
    if (value.builtin == BuiltinValue::Function && !value.tensor) {
      auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
      return LLVM::LLVMStructType::getLiteral(builder_.getContext(), {ptr, ptr});
    }
    if (isOwnedArray(value))
      return recordDescriptor(value);
    if (value.builtin == BuiltinValue::Record)
      return recordTypes_.at(*value.record);
    if (value.builtin == BuiltinValue::TaskError)
      return LLVM::LLVMStructType::getLiteral(builder_.getContext(),
                                              {builder_.getI32Type(), builder_.getI64Type(),
                                               builder_.getI64Type(), builder_.getI64Type()});
    if (value.builtin == BuiltinValue::FaultKind)
      return builder_.getI32Type();
    if (value.builtin == BuiltinValue::Bool)
      return builder_.getI1Type();
    if (value.builtin == BuiltinValue::String) {
      const auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
      return LLVM::LLVMStructType::getLiteral(builder_.getContext(),
                                              {ptr, ptr, builder_.getI64Type()});
    }
    const Type element = isInteger(value.element)
                             ? Type(builder_.getIntegerType(describeNumeric(value.element)->bits))
                         : value.element == NumericType::Float64 ? Type(builder_.getF64Type())
                                                                 : Type(builder_.getF32Type());
    if (!value.tensor)
      return element;
    SmallVector<std::int64_t> shape;
    for (const auto &extent : value.shape)
      shape.push_back(extent.constant ? static_cast<std::int64_t>(*extent.constant)
                                      : ShapedType::kDynamic);
    return RankedTensorType::get(shape, element);
  }
  Type recordDescriptor(const ValueType &value) {
    auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
    auto dims = LLVM::LLVMArrayType::get(builder_.getI64Type(), value.shape.size());
    return LLVM::LLVMStructType::getLiteral(builder_.getContext(),
                                            {ptr, ptr, builder_.getI64Type(), dims, dims});
  }
  // Ranked tensors cannot cross a generic indirect call during bufferization.
  // Use the existing checked collection descriptors at this private boundary.
  // Numeric collections are copied into invocation-owned storage; owned arrays
  // already have descriptor representations and preserve their existing owners.
  Type callableABIType(const ValueType &value) {
    return value.tensor ? recordDescriptor(value) : type(value);
  }
  Value callablePack(Location loc, Value value, const ValueType &source) {
    return source.tensor && !isOwnedArray(source) ? recordPack(loc, value, source) : value;
  }
  Value callableUnpack(Location loc, Value value, const ValueType &target) {
    return target.tensor && !isOwnedArray(target) ? recordUnpack(loc, value, target) : value;
  }
  Type closureEnvironment(FunctionId id) {
    const auto &source = input_.functions[id];
    SmallVector<Type> fields;
    for (std::size_t i = source.parameters.size() - source.captureCount;
         i < source.parameters.size(); ++i)
      fields.push_back(callableABIType(input_.symbols[source.parameters[i]].type));
    return LLVM::LLVMStructType::getLiteral(builder_.getContext(), fields);
  }
  Value callableCode(Location loc, func::FuncOp function) {
    auto reference = func::ConstantOp::create(
        builder_, loc, function.getFunctionType(),
        FlatSymbolRefAttr::get(builder_.getContext(), function.getSymName()));
    return UnrealizedConversionCastOp::create(
               builder_, loc, TypeRange{LLVM::LLVMPointerType::get(builder_.getContext())},
               ValueRange{reference})
        .getResult(0);
  }
  func::FuncOp callableEntry(FunctionId id) {
    const auto &source = input_.functions[id];
    const auto result = source.declaredResult.value_or(source.resultType);
    if (auto found = callableEntries_.find(id); found != callableEntries_.end())
      return found->second;
    OpBuilder::InsertionGuard insertion(builder_);
    builder_.setInsertionPointToEnd(module_->getBody());
    auto loc = location(source.declaration);
    auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
    const auto explicitCount = source.parameters.size() - source.captureCount;
    SmallVector<Type> parameters{ptr};
    for (std::size_t i = 0; i < explicitCount; ++i)
      parameters.push_back(callableABIType(input_.symbols[source.parameters[i]].type));
    auto entry =
        func::FuncOp::create(builder_, loc, "kalandra_callable_" + std::to_string(id),
                             builder_.getFunctionType(parameters, {callableABIType(result)}));
    entry.setPrivate();
    callableEntries_.emplace(id, entry);
    builder_.setInsertionPointToStart(entry.addEntryBlock());
    SmallVector<Value> arguments;
    for (std::size_t i = 0; i < explicitCount; ++i)
      arguments.push_back(
          callableUnpack(loc, entry.getArgument(i + 1), input_.symbols[source.parameters[i]].type));
    if (source.captureCount) {
      Value address = entry.getArgument(0);
      if (recursiveClosures_) {
        llvm::LLVMContext llvmContext;
        LLVM::TypeToLLVMIRTranslator translator(llvmContext);
        llvm::DataLayout layout(nativeCpuDataLayout);
        auto bytes = layout.getTypeAllocSize(translator.translateType(closureEnvironment(id)))
                         .getFixedValue();
        address = func::CallOp::create(
                      builder_, loc, "kalandra_recursive_environment_read", TypeRange{ptr},
                      ValueRange{address, integer(loc, bytes), callableCode(loc, entry)})
                      .getResult(0);
      }
      Value environment = LLVM::LoadOp::create(builder_, loc, closureEnvironment(id), address);
      for (std::size_t i = 0; i < source.captureCount; ++i) {
        Value capture = LLVM::ExtractValueOp::create(builder_, loc, environment,
                                                     ArrayRef<int64_t>{static_cast<int64_t>(i)});
        arguments.push_back(callableUnpack(
            loc, capture, input_.symbols[source.parameters[explicitCount + i]].type));
      }
    }
    auto value = func::CallOp::create(builder_, loc, functions_[id], arguments).getResult(0);
    func::ReturnOp::create(builder_, loc, ValueRange{callablePack(loc, value, result)});
    return entry;
  }
  SmallVector<Value> recordIndices(Location loc, Value flat, ArrayRef<Value> shape) {
    SmallVector<Value> indices(shape.size());
    for (std::size_t axis = shape.size(); axis > 0; --axis) {
      auto safe = arith::SelectOp::create(
          builder_, loc, compare(loc, arith::CmpIPredicate::eq, shape[axis - 1], index(loc, 0)),
          index(loc, 1), shape[axis - 1]);
      indices[axis - 1] = arith::RemUIOp::create(builder_, loc, flat, safe);
      flat = arith::DivUIOp::create(builder_, loc, flat, safe);
    }
    return indices;
  }
  Value recordPack(Location loc, Value input, const ValueType &field) {
    auto shape = dimensions(loc, input);
    auto element = cast<RankedTensorType>(input.getType()).getElementType();
    auto count = checkedCount(loc, shape, describeNumeric(field.element)->bytes());
    auto bytes = arith::MulIOp::create(builder_, loc, count,
                                       index(loc, describeNumeric(field.element)->bytes()));
    auto bytes64 = arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), bytes);
    auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
    Value storage = func::CallOp::create(builder_, loc, "kalandra_record_alloc", TypeRange{ptr},
                                         ValueRange{bytes64})
                        .getResult(0);
    scf::ForOp::create(
        builder_, loc, index(loc, 0), count, index(loc, 1), ValueRange{},
        [&](OpBuilder &, Location, Value i, ValueRange) {
          auto cell = tensor::ExtractOp::create(builder_, loc, input, recordIndices(loc, i, shape));
          auto offset = arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), i);
          auto address =
              LLVM::GEPOp::create(builder_, loc, ptr, element, storage, ValueRange{offset});
          LLVM::StoreOp::create(builder_, loc, cell, address);
          scf::YieldOp::create(builder_, loc);
        });
    Value result = LLVM::UndefOp::create(builder_, loc, recordDescriptor(field));
    const auto insert = [&](Value value, ArrayRef<int64_t> path) {
      result = LLVM::InsertValueOp::create(builder_, loc, result, value, path);
    };
    insert(storage, {0});
    insert(storage, {1});
    insert(integer(loc, 0), {2});
    Value stride = index(loc, 1);
    auto empty = compare(loc, arith::CmpIPredicate::eq, count, index(loc, 0));
    for (std::size_t axis = shape.size(); axis > 0; --axis) {
      auto canonical = arith::SelectOp::create(builder_, loc, empty, index(loc, 0), stride);
      insert(arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), shape[axis - 1]),
             {3, static_cast<int64_t>(axis - 1)});
      insert(arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), canonical),
             {4, static_cast<int64_t>(axis - 1)});
      // Zero-count shapes need no representable suffix product.
      stride = arith::MulIOp::create(builder_, loc, canonical, shape[axis - 1]);
    }
    return result;
  }
  Value descriptorSlot(Location loc, Type type) {
    OpBuilder::InsertionGuard guard(builder_);
    auto *parent = builder_.getInsertionBlock()->getParentOp();
    while (!isa<func::FuncOp>(parent))
      parent = parent->getParentOp();
    builder_.setInsertionPointToStart(&cast<func::FuncOp>(parent).getBody().front());
    return LLVM::AllocaOp::create(builder_, loc, LLVM::LLVMPointerType::get(builder_.getContext()),
                                  type, integer(loc, 1));
  }
  Value stringSlot(Location loc, Value value) {
    auto slot = descriptorSlot(loc, value.getType());
    LLVM::StoreOp::create(builder_, loc, value, slot);
    return slot;
  }
  void validateString(Location loc, Value value) {
    auto slot = stringSlot(loc, value);
    func::CallOp::create(builder_, loc, "kalandra_string_validate", TypeRange{}, ValueRange{slot});
  }
  Value narrowError(Location loc, Value input, const ValueType &source, const ValueType &target) {
    const auto &from = input_.records[*source.record];
    auto tag = LLVM::ExtractValueOp::create(builder_, loc, input, ArrayRef<int64_t>{0});
    Value result = LLVM::ZeroOp::create(builder_, loc, type(target));
    Value found = arith::ConstantIntOp::create(builder_, loc, 0, 1);
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> targetVariants;
    if (target.record)
      for (std::size_t i = 0; i < input_.records[*target.record].variants.size(); ++i)
        targetVariants.emplace(errorOrigin(input_, *target.record, i), i);
    for (std::size_t i = 0; i < from.variants.size(); ++i) {
      const auto origin = errorOrigin(input_, *source.record, i);
      std::optional<std::size_t> selected;
      if (target.builtin == BuiltinValue::TaskError) {
        if (input_.records[origin.first].taskErrorFamily)
          selected = origin.second;
      } else if (auto found = targetVariants.find(origin); found != targetVariants.end())
        selected = found->second;
      if (!selected)
        continue;
      auto condition = compare(loc, arith::CmpIPredicate::eq, tag, integer(loc, i + 1));
      auto choice = scf::IfOp::create(builder_, loc, TypeRange{type(target)}, condition, true);
      {
        OpBuilder::InsertionGuard insertion(builder_);
        builder_.setInsertionPointToStart(&choice.getThenRegion().front());
        Value value;
        if (target.builtin == BuiltinValue::TaskError)
          value = LLVM::ExtractValueOp::create(
              builder_, loc, input,
              ArrayRef<int64_t>{static_cast<int64_t>(from.variants[i].fields.front())});
        else {
          value = LLVM::ZeroOp::create(builder_, loc, type(target));
          value = LLVM::InsertValueOp::create(builder_, loc, value, integer(loc, *selected + 1),
                                              ArrayRef<int64_t>{0});
          const auto &to = input_.records[*target.record];
          for (std::size_t field = 0; field < from.variants[i].fields.size(); ++field) {
            auto member = LLVM::ExtractValueOp::create(
                builder_, loc, input,
                ArrayRef<int64_t>{static_cast<int64_t>(from.variants[i].fields[field])});
            value = LLVM::InsertValueOp::create(
                builder_, loc, value, member,
                ArrayRef<int64_t>{static_cast<int64_t>(to.variants[*selected].fields[field])});
          }
        }
        scf::YieldOp::create(builder_, loc, value);
        builder_.setInsertionPointToStart(&choice.getElseRegion().front());
        scf::YieldOp::create(builder_, loc, result);
      }
      result = choice.getResult(0);
      found = arith::OrIOp::create(builder_, loc, found, condition);
    }
    guard(loc, found, 3);
    return result;
  }
  void validateStrings(Location loc, Value value, const ValueType &source) {
    if (source.builtin == BuiltinValue::TaskError && !source.tensor) {
      auto kind = LLVM::ExtractValueOp::create(builder_, loc, value, ArrayRef<int64_t>{0});
      auto lower = arith::ConstantIntOp::create(builder_, loc, 1, 32);
      auto upper = arith::ConstantIntOp::create(builder_, loc, 4, 32);
      guard(loc, compare(loc, arith::CmpIPredicate::uge, kind, lower), 3);
      guard(loc, compare(loc, arith::CmpIPredicate::ule, kind, upper), 3);
    } else if (isRecordArray(source) || isFunctionArray(source)) {
      recordArrayLength(loc, value, source);
    } else if (isStringArray(source)) {
      auto address = stringSlot(loc, value);
      func::CallOp::create(builder_, loc, "kalandra_string_array_validate", TypeRange{},
                           ValueRange{address});
    } else if (source.builtin == BuiltinValue::String)
      validateString(loc, value);
    else if (source.builtin == BuiltinValue::Record && !source.tensor) {
      const auto &record = input_.records[*source.record];
      const auto validateField = [&](std::size_t i, std::optional<std::size_t> taskKind = {}) {
        const auto &field = record.fields[i].type;
        if (field.builtin == BuiltinValue::String || field.builtin == BuiltinValue::Record ||
            field.builtin == BuiltinValue::TaskError) {
          auto member = LLVM::ExtractValueOp::create(builder_, loc, value,
                                                     ArrayRef<int64_t>{static_cast<int64_t>(i)});
          if (recordStorage_.fields[*source.record][i] == RecordFieldStorage::ManagedReference) {
            // Validate the immediate reference without recursively generating a
            // walk of the cyclic nominal graph. Payload reads remain checked.
            auto loaded = recursive_->read(loc, member, *field.record);
            if (!loaded)
              throw LoweringFailure{loaded.error()};
          } else
            validateStrings(loc, member, field);
          if (field.builtin == BuiltinValue::TaskError && taskKind) {
            auto kind = LLVM::ExtractValueOp::create(builder_, loc, member, ArrayRef<int64_t>{0});
            auto expected = arith::ConstantIntOp::create(builder_, loc, *taskKind, 32);
            guard(loc, compare(loc, arith::CmpIPredicate::eq, kind, expected), 3);
          }
        }
      };
      if (record.variants.empty()) {
        for (std::size_t i = 0; i < record.fields.size(); ++i)
          validateField(i);
      } else {
        auto tag = LLVM::ExtractValueOp::create(builder_, loc, value, ArrayRef<int64_t>{0});
        guard(loc, compare(loc, arith::CmpIPredicate::uge, tag, integer(loc, 1)), 3);
        guard(loc,
              compare(loc, arith::CmpIPredicate::ule, tag, integer(loc, record.variants.size())),
              3);
        for (std::size_t i = 0; i < record.variants.size(); ++i) {
          auto choice = scf::IfOp::create(
              builder_, loc, TypeRange{},
              compare(loc, arith::CmpIPredicate::eq, tag, integer(loc, i + 1)), false);
          OpBuilder::InsertionGuard insertion(builder_);
          auto &block = choice.getThenRegion().front();
          builder_.setInsertionPointToStart(&block);
          const auto [original, variant] = errorOrigin(input_, *source.record, i);
          for (auto slot : record.variants[i].fields)
            validateField(slot, input_.records[original].taskErrorFamily
                                    ? std::optional<std::size_t>{variant + 1}
                                    : std::nullopt);
          if (block.empty() || !block.back().hasTrait<OpTrait::IsTerminator>())
            scf::YieldOp::create(builder_, loc);
        }
      }
    }
  }
  Type arrayCellType(const ValueType &array) {
    return isBoolArray(array) ? builder_.getI8Type() : type(recordArrayElement(array));
  }
  Value arrayCellRead(Location loc, Value address, const ValueType &array) {
    Value cell = LLVM::LoadOp::create(builder_, loc, arrayCellType(array), address);
    if (isBoolArray(array)) {
      auto one = arith::ConstantIntOp::create(builder_, loc, 1, 8);
      guard(loc, compare(loc, arith::CmpIPredicate::ule, cell, one), 3);
      cell = arith::TruncIOp::create(builder_, loc, builder_.getI1Type(), cell);
    }
    return cell;
  }
  void arrayCellStore(Location loc, Value cell, Value address, const ValueType &array) {
    if (isBoolArray(array))
      cell = arith::ExtUIOp::create(builder_, loc, builder_.getI8Type(), cell);
    LLVM::StoreOp::create(builder_, loc, cell, address);
  }
  std::pair<std::uint64_t, std::uint64_t> recordCellLayout(const ValueType &array) {
    if (isBoolArray(array))
      return {1, 1};
    if (isStringArray(array))
      return {24, 8};
    if (isFunctionArray(array))
      return {16, 8};
    if (auto found = recordLayouts_.find(*array.record); found != recordLayouts_.end())
      return found->second;
    llvm::LLVMContext context;
    LLVM::TypeToLLVMIRTranslator translator(context);
    llvm::DataLayout layout(nativeCpuDataLayout);
    auto *cell = translator.translateType(type(recordArrayElement(array)));
    return recordLayouts_[*array.record] = {layout.getTypeAllocSize(cell).getFixedValue(),
                                            layout.getABITypeAlign(cell).value()};
  }
  Value recordArrayLength(Location loc, Value input, const ValueType &array) {
    const auto [cellBytes, alignment] = recordCellLayout(array);
    auto address = descriptorSlot(loc, input.getType());
    LLVM::StoreOp::create(builder_, loc, input, address);
    if (isStringArray(array))
      func::CallOp::create(builder_, loc, "kalandra_string_array_validate", TypeRange{},
                           ValueRange{address});
    else
      func::CallOp::create(builder_, loc, "kalandra_record_array_validate", TypeRange{},
                           ValueRange{address, integer(loc, cellBytes), integer(loc, alignment)});
    return LLVM::ExtractValueOp::create(builder_, loc, input, ArrayRef<int64_t>{3, 0});
  }
  Value recordArrayStorage(Location loc, const ValueType &array, Value count) {
    const auto cellBytes = recordCellLayout(array).first;
    auto checked = checkedCount(loc, {count}, cellBytes);
    auto bytes = arith::MulIOp::create(builder_, loc, checked, index(loc, cellBytes));
    auto bytes64 = arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), bytes);
    auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
    Value storage = func::CallOp::create(builder_, loc, "kalandra_record_alloc", TypeRange{ptr},
                                         ValueRange{bytes64})
                        .getResult(0);
    Value result = LLVM::UndefOp::create(builder_, loc, recordDescriptor(array));
    const auto insert = [&](Value value, ArrayRef<int64_t> path) {
      result = LLVM::InsertValueOp::create(builder_, loc, result, value, path);
    };
    insert(storage, {0});
    insert(storage, {1});
    insert(integer(loc, 0), {2});
    insert(arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), count), {3, 0});
    auto empty = compare(loc, arith::CmpIPredicate::eq, count, index(loc, 0));
    insert(arith::SelectOp::create(builder_, loc, empty, integer(loc, 0), integer(loc, 1)), {4, 0});
    return result;
  }
  Value recordArrayLiteral(Location loc, const ValueType &array, ArrayRef<Value> elements) {
    auto result = recordArrayStorage(loc, array, index(loc, elements.size()));
    auto storage = LLVM::ExtractValueOp::create(builder_, loc, result, ArrayRef<int64_t>{1});
    auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
    auto cell = arrayCellType(array);
    for (std::size_t i = 0; i < elements.size(); ++i) {
      auto address =
          LLVM::GEPOp::create(builder_, loc, ptr, cell, storage, ValueRange{integer(loc, i)});
      arrayCellStore(loc, elements[i], address, array);
    }
    return result;
  }
  Value recordUnpack(Location loc, Value input, const ValueType &field) {
    auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
    Value address;
    {
      // A read inside a map/loop reuses a fixed entry-block descriptor slot.
      // Repeated dynamic stack allocations would grow until the helper returns.
      OpBuilder::InsertionGuard guard(builder_);
      auto *parent = builder_.getInsertionBlock()->getParentOp();
      while (!isa<func::FuncOp>(parent))
        parent = parent->getParentOp();
      builder_.setInsertionPointToStart(&cast<func::FuncOp>(parent).getBody().front());
      address =
          LLVM::AllocaOp::create(builder_, loc, ptr, recordDescriptor(field), integer(loc, 1));
    }
    LLVM::StoreOp::create(builder_, loc, input, address);
    func::CallOp::create(builder_, loc, "kalandra_record_validate", TypeRange{},
                         ValueRange{address, integer(loc, field.shape.size()),
                                    integer(loc, static_cast<unsigned>(field.element))});
    SmallVector<Value> shape, dynamic;
    auto output = cast<RankedTensorType>(type(field));
    for (std::size_t axis = 0; axis < field.shape.size(); ++axis) {
      auto d = LLVM::ExtractValueOp::create(builder_, loc, input,
                                            ArrayRef<int64_t>{3, static_cast<int64_t>(axis)});
      Value extent = arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(), d);
      shape.push_back(extent);
      if (output.isDynamicDim(axis))
        dynamic.push_back(extent);
      else
        guard(loc,
              compare(loc, arith::CmpIPredicate::eq, extent, index(loc, output.getDimSize(axis))),
              1);
    }
    auto count = checkedCount(loc, shape, describeNumeric(field.element)->bytes());
    auto empty =
        tensor::EmptyOp::create(builder_, loc, output.getShape(), output.getElementType(), dynamic);
    auto storage = LLVM::ExtractValueOp::create(builder_, loc, input, ArrayRef<int64_t>{1});
    auto loop = scf::ForOp::create(
        builder_, loc, index(loc, 0), count, index(loc, 1), ValueRange{empty},
        [&](OpBuilder &, Location, Value i, ValueRange args) {
          auto offset = arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), i);
          auto address = LLVM::GEPOp::create(builder_, loc, ptr, output.getElementType(), storage,
                                             ValueRange{offset});
          auto cell = LLVM::LoadOp::create(builder_, loc, output.getElementType(), address);
          auto inserted =
              tensor::InsertOp::create(builder_, loc, cell, args[0], recordIndices(loc, i, shape));
          scf::YieldOp::create(builder_, loc, ValueRange{inserted});
        });
    return loop.getResult(0);
  }
  Value index(Location loc, std::int64_t value) {
    return arith::ConstantIndexOp::create(builder_, loc, value);
  }
  Value integer(Location loc, std::int64_t value) {
    return arith::ConstantIntOp::create(builder_, loc, value, 64);
  }
  Value zero(Location loc) {
    return arith::ConstantFloatOp::create(builder_, loc, builder_.getF32Type(),
                                          llvm::APFloat(0.0f));
  }
  Value binary(Location loc, StringRef name, Value left, Value right) {
    OperationState state(loc, name);
    state.addOperands({left, right});
    state.addTypes(left.getType());
    return builder_.create(state)->getResult(0);
  }
  Value compare(Location loc, arith::CmpIPredicate predicate, Value left, Value right) {
    return arith::CmpIOp::create(builder_, loc, predicate, left, right);
  }
  void guard(Location loc, Value condition, int code) {
    auto bad = arith::XOrIOp::create(builder_, loc, condition,
                                     arith::ConstantIntOp::create(builder_, loc, 1, 1));
    scf::IfOp::create(builder_, loc, bad, [&](OpBuilder &, Location) {
      auto file = dyn_cast<FileLineColLoc>(loc);
      func::CallOp::create(builder_, loc, "kalandra_fault", TypeRange{},
                           ValueRange{integer(loc, code), integer(loc, file ? file.getLine() : 0),
                                      integer(loc, file ? file.getColumn() : 0)});
      scf::YieldOp::create(builder_, loc);
    });
  }
  Value checkedSigned(Location loc, Value wide, unsigned bits) {
    auto constant = [&](std::int64_t value) -> Value {
      return arith::ConstantOp::create(
          builder_, loc, wide.getType(),
          builder_.getIntegerAttr(wide.getType(),
                                  llvm::APInt(bits * 2, static_cast<std::uint64_t>(value), true)));
    };
    auto low = compare(loc, arith::CmpIPredicate::sge, wide,
                       constant(bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1))));
    auto high = compare(loc, arith::CmpIPredicate::sle, wide,
                        constant(bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1));
    guard(loc, arith::AndIOp::create(builder_, loc, low, high), 4);
    return arith::TruncIOp::create(builder_, loc, builder_.getIntegerType(bits), wide);
  }
  Value scalarComparison(Location loc, Value a, Value b, const ValueType &source,
                         ComparisonKind op) {
    using P = arith::CmpIPredicate;
    if (source.builtin == BuiltinValue::Bool || isInteger(source.element)) {
      const bool u = source.builtin == BuiltinValue::Bool || isUnsigned(source.element);
      const auto predicate = op == ComparisonKind::Equal       ? P::eq
                             : op == ComparisonKind::NotEqual  ? P::ne
                             : op == ComparisonKind::Less      ? (u ? P::ult : P::slt)
                             : op == ComparisonKind::LessEqual ? (u ? P::ule : P::sle)
                             : op == ComparisonKind::Greater   ? (u ? P::ugt : P::sgt)
                                                               : (u ? P::uge : P::sge);
      return compare(loc, predicate, a, b);
    }
    if (arithmetic_ != ArithmeticMode::Software) {
      using F = arith::CmpFPredicate;
      const auto predicate = op == ComparisonKind::Equal       ? F::OEQ
                             : op == ComparisonKind::NotEqual  ? F::UNE
                             : op == ComparisonKind::Less      ? F::OLT
                             : op == ComparisonKind::LessEqual ? F::OLE
                             : op == ComparisonKind::Greater   ? F::OGT
                                                               : F::OGE;
      return arith::CmpFOp::create(builder_, loc, predicate, a, b);
    }
    const unsigned width = describeNumeric(source.element)->bits;
    const auto integerType = builder_.getIntegerType(width);
    a = arith::BitcastOp::create(builder_, loc, integerType, a);
    b = arith::BitcastOp::create(builder_, loc, integerType, b);
    const auto constant = [&](std::uint64_t bits) -> Value {
      return arith::ConstantIntOp::create(builder_, loc, static_cast<std::int64_t>(bits), width);
    };
    const auto both = [&](Value x, Value y) -> Value {
      return arith::AndIOp::create(builder_, loc, x, y);
    };
    const auto either = [&](Value x, Value y) -> Value {
      return arith::OrIOp::create(builder_, loc, x, y);
    };
    const auto negate = [&](Value x) -> Value {
      return arith::XOrIOp::create(builder_, loc, x,
                                   arith::ConstantIntOp::create(builder_, loc, 1, 1));
    };
    auto magnitude = constant(width == 32 ? UINT64_C(0x7fffffff) : UINT64_C(0x7fffffffffffffff));
    auto infinity = constant(width == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000));
    auto zero = constant(0), sign = constant(UINT64_C(1) << (width - 1));
    Value ma = arith::AndIOp::create(builder_, loc, a, magnitude),
          mb = arith::AndIOp::create(builder_, loc, b, magnitude);
    auto ordered = both(compare(loc, P::ule, ma, infinity), compare(loc, P::ule, mb, infinity));
    auto equal = either(compare(loc, P::eq, a, b),
                        both(compare(loc, P::eq, ma, zero), compare(loc, P::eq, mb, zero)));
    Value sa = arith::AndIOp::create(builder_, loc, a, sign),
          sb = arith::AndIOp::create(builder_, loc, b, sign);
    auto negative = compare(loc, P::ne, sa, zero);
    Value sameSignLess = arith::SelectOp::create(
        builder_, loc, negative, compare(loc, P::ugt, a, b), compare(loc, P::ult, a, b));
    Value less =
        arith::SelectOp::create(builder_, loc, compare(loc, P::ne, sa, sb), negative, sameSignLess);
    less = both(negate(equal), less);
    if (op == ComparisonKind::NotEqual)
      return negate(both(ordered, equal));
    Value result = op == ComparisonKind::Equal       ? equal
                   : op == ComparisonKind::Less      ? less
                   : op == ComparisonKind::LessEqual ? either(less, equal)
                   : op == ComparisonKind::Greater   ? negate(either(less, equal))
                                                     : negate(less);
    return both(ordered, result);
  }
  Value relu(Location loc, Value input, NumericType element) {
    const auto bits = describeNumeric(element)->bits;
    if (isUnsigned(element))
      return input;
    auto integerType = builder_.getIntegerType(bits);
    Value value = isInteger(element)
                      ? input
                      : Value(arith::BitcastOp::create(builder_, loc, integerType, input));
    auto zero = arith::ConstantIntOp::create(builder_, loc, 0, bits);
    Value clear = compare(loc, arith::CmpIPredicate::slt, value, zero);
    if (!isInteger(element)) {
      auto magnitude = arith::AndIOp::create(
          builder_, loc, value,
          arith::ConstantIntOp::create(builder_, loc,
                                       bits == 32 ? 0x7fffffffLL : 0x7fffffffffffffffLL, bits));
      auto infinity = arith::ConstantIntOp::create(
          builder_, loc, bits == 32 ? 0x7f800000LL : 0x7ff0000000000000LL, bits);
      auto notNaN = compare(loc, arith::CmpIPredicate::ule, magnitude, infinity);
      clear = arith::AndIOp::create(builder_, loc, clear, notNaN);
    }
    auto result = arith::SelectOp::create(builder_, loc, clear, zero, value);
    return isInteger(element)
               ? Value(result)
               : Value(arith::BitcastOp::create(builder_, loc, input.getType(), result));
  }
  Value integerUnary(Location loc, Value operand, TokenKind op, bool unsignedValue = false) {
    if (op == TokenKind::Plus)
      return operand;
    if (unsignedValue) {
      guard(loc,
            compare(loc, arith::CmpIPredicate::eq, operand,
                    arith::ConstantIntOp::create(builder_, loc, 0,
                                                 cast<IntegerType>(operand.getType()).getWidth())),
            4);
      return operand;
    }
    const auto bits = cast<IntegerType>(operand.getType()).getWidth();
    auto wide = arith::ExtSIOp::create(builder_, loc, builder_.getIntegerType(bits * 2), operand);
    auto z = arith::ConstantIntOp::create(builder_, loc, 0, bits * 2);
    return checkedSigned(loc, arith::SubIOp::create(builder_, loc, z, wide), bits);
  }
  Value integerBinary(Location loc, Value a, Value b, TokenKind op, bool unsignedValue = false) {
    if (unsignedValue) {
      const auto bits = cast<IntegerType>(a.getType()).getWidth();
      if (bits == 64 && (op == TokenKind::Slash || op == TokenKind::Percent)) {
        guard(loc, compare(loc, arith::CmpIPredicate::ne, b, integer(loc, 0)), 4);
        return binary(loc, op == TokenKind::Slash ? "arith.divui" : "arith.remui", a, b);
      }
      auto wideType = builder_.getIntegerType(bits * 2);
      a = arith::ExtUIOp::create(builder_, loc, wideType, a);
      b = arith::ExtUIOp::create(builder_, loc, wideType, b);
      auto z = arith::ConstantIntOp::create(builder_, loc, 0, bits * 2);
      if (op == TokenKind::Slash || op == TokenKind::Percent)
        guard(loc, compare(loc, arith::CmpIPredicate::ne, b, z), 4);
      if (op == TokenKind::Minus)
        guard(loc, compare(loc, arith::CmpIPredicate::uge, a, b), 4);
      const char *name = op == TokenKind::Plus      ? "arith.addi"
                         : op == TokenKind::Minus   ? "arith.subi"
                         : op == TokenKind::Star    ? "arith.muli"
                         : op == TokenKind::Percent ? "arith.remui"
                                                    : "arith.divui";
      auto wide = binary(loc, name, a, b);
      auto maximum = arith::ConstantOp::create(
          builder_, loc, wideType,
          builder_.getIntegerAttr(wideType, llvm::APInt::getLowBitsSet(bits * 2, bits)));
      guard(loc, compare(loc, arith::CmpIPredicate::ule, wide, maximum), 4);
      return arith::TruncIOp::create(builder_, loc, builder_.getIntegerType(bits), wide);
    }
    const auto bits = cast<IntegerType>(a.getType()).getWidth();
    if (bits == 64 && (op == TokenKind::Slash || op == TokenKind::Percent)) {
      guard(loc, compare(loc, arith::CmpIPredicate::ne, b, integer(loc, 0)), 4);
      auto exceptional = arith::AndIOp::create(
          builder_, loc, compare(loc, arith::CmpIPredicate::eq, a, integer(loc, INT64_MIN)),
          compare(loc, arith::CmpIPredicate::eq, b, integer(loc, -1)));
      if (op == TokenKind::Slash) {
        guard(loc,
              compare(loc, arith::CmpIPredicate::eq, exceptional,
                      arith::ConstantIntOp::create(builder_, loc, 0, 1)),
              4);
        return arith::DivSIOp::create(builder_, loc, a, b);
      }
      // x % -1 is zero for every x, including INT64_MIN. Normalizing every
      // -1 divisor to 1 avoids LLVM's exceptional poison pair with one compare,
      // without changing division overflow or the zero-divisor fault above.
      auto minusOne = compare(loc, arith::CmpIPredicate::eq, b, integer(loc, -1));
      auto safe = arith::SelectOp::create(builder_, loc, minusOne, integer(loc, 1), b);
      return arith::RemSIOp::create(builder_, loc, a, safe);
    }
    auto wideType = builder_.getIntegerType(bits * 2);
    a = arith::ExtSIOp::create(builder_, loc, wideType, a);
    b = arith::ExtSIOp::create(builder_, loc, wideType, b);
    if (op == TokenKind::Slash || op == TokenKind::Percent)
      guard(loc,
            compare(loc, arith::CmpIPredicate::ne, b,
                    arith::ConstantIntOp::create(builder_, loc, 0, bits * 2)),
            4);
    const char *name = op == TokenKind::Plus      ? "arith.addi"
                       : op == TokenKind::Minus   ? "arith.subi"
                       : op == TokenKind::Star    ? "arith.muli"
                       : op == TokenKind::Percent ? "arith.remsi"
                                                  : "arith.divsi";
    return checkedSigned(loc, binary(loc, name, a, b), bits);
  }
  SmallVector<Value> dimensions(Location loc, Value tensor) {
    SmallVector<Value> result;
    const auto rank = cast<RankedTensorType>(tensor.getType()).getRank();
    for (std::int64_t axis = 0; axis < rank; ++axis)
      result.push_back(tensor::DimOp::create(builder_, loc, tensor, index(loc, axis)));
    return result;
  }
  Value checkedCount(Location loc, ArrayRef<Value> dimensions, std::uint64_t elementBytes = 4) {
    auto z = index(loc, 0), one = index(loc, 1);
    Value hasZero = arith::ConstantIntOp::create(builder_, loc, 0, 1);
    for (auto d : dimensions) {
      guard(loc, compare(loc, arith::CmpIPredicate::sge, d, z), 1);
      hasZero = arith::OrIOp::create(builder_, loc, hasZero,
                                     compare(loc, arith::CmpIPredicate::eq, d, z));
    }
    Value count = arith::SelectOp::create(builder_, loc, hasZero, z, one);
    auto maximum = index(loc, std::numeric_limits<std::int64_t>::max() / elementBytes);
    for (auto d : dimensions) {
      auto safe = arith::SelectOp::create(builder_, loc,
                                          compare(loc, arith::CmpIPredicate::eq, d, z), one, d);
      auto limit = arith::DivUIOp::create(builder_, loc, maximum, safe);
      guard(loc, compare(loc, arith::CmpIPredicate::ule, count, limit), 2);
      count = arith::MulIOp::create(builder_, loc, count, d);
    }
    return count;
  }
  Value castTensor(Location loc, Value value, Type target) {
    if (value.getType() == target)
      return value;
    return tensor::CastOp::create(builder_, loc, target, value);
  }
  Value reductionScratch(Location loc, Value count, Type element = {}) {
    checkedCount(loc, {count}, element ? element.getIntOrFloatBitWidth() / 8 : 4);
    auto scratchType =
        MemRefType::get({ShapedType::kDynamic}, element ? element : builder_.getF32Type());
    return memref::AllocOp::create(builder_, loc, scratchType, ValueRange{count});
  }
  Value scratchLoad(Location loc, Value scratch, Value i, unsigned width, Value base = {}) {
    Value offset = width == 1 ? i : arith::MulIOp::create(builder_, loc, i, index(loc, width));
    if (base)
      offset = arith::AddIOp::create(builder_, loc, base, offset);
    if (width == 1)
      return memref::LoadOp::create(builder_, loc, scratch, ValueRange{offset});
    return vector::LoadOp::create(
        builder_, loc,
        VectorType::get({width}, cast<MemRefType>(scratch.getType()).getElementType()), scratch,
        ValueRange{offset});
  }
  void scratchStore(Location loc, Value value, Value scratch, Value i, unsigned width,
                    Value base = {}) {
    Value offset = width == 1 ? i : arith::MulIOp::create(builder_, loc, i, index(loc, width));
    if (base)
      offset = arith::AddIOp::create(builder_, loc, base, offset);
    if (width == 1)
      memref::StoreOp::create(builder_, loc, value, scratch, ValueRange{offset});
    else
      vector::StoreOp::create(builder_, loc, value, scratch, ValueRange{offset});
  }
  Value scratchReduction(Location loc, Value count, Value scratch, unsigned width = 1,
                         Value base = {}, bool unsignedValue = false) {
    const auto z = index(loc, 0), one = index(loc, 1), two = index(loc, 2);
    const auto elementType = cast<MemRefType>(scratch.getType()).getElementType();
    const auto vectorType = VectorType::get({width}, elementType);
    const Type resultType = width == 1 ? elementType : Type(vectorType);
    const auto load = [&](Value i) { return scratchLoad(loc, scratch, i, width, base); };
    const auto store = [&](Value value, Value i) {
      scratchStore(loc, value, scratch, i, width, base);
    };
    scf::WhileOp::create(
        builder_, loc, TypeRange{builder_.getIndexType()}, ValueRange{count},
        [&](OpBuilder &, Location, ValueRange args) {
          auto condition = compare(loc, arith::CmpIPredicate::ugt, args[0], one);
          scf::ConditionOp::create(builder_, loc, condition, args);
        },
        [&](OpBuilder &, Location, ValueRange args) {
          auto length = args[0];
          auto pairs = arith::DivUIOp::create(builder_, loc, length, two);
          auto odd = arith::RemUIOp::create(builder_, loc, length, two);
          auto next = arith::AddIOp::create(builder_, loc, pairs, odd);
          scf::ForOp::create(builder_, loc, z, pairs, one, ValueRange{},
                             [&](OpBuilder &, Location, Value i, ValueRange) {
                               auto first = arith::MulIOp::create(builder_, loc, i, two);
                               auto second = arith::AddIOp::create(builder_, loc, first, one);
                               auto a = load(first);
                               auto b = load(second);
                               Value sum =
                                   isa<IntegerType>(elementType)
                                       ? integerBinary(loc, a, b, TokenKind::Plus, unsignedValue)
                                       : Value(arith::AddFOp::create(builder_, loc, a, b));
                               store(sum, i);
                               scf::YieldOp::create(builder_, loc);
                             });
          scf::IfOp::create(builder_, loc, compare(loc, arith::CmpIPredicate::ne, odd, z),
                            [&](OpBuilder &, Location) {
                              auto last = arith::SubIOp::create(builder_, loc, length, one);
                              auto tail = load(last);
                              store(tail, pairs);
                              scf::YieldOp::create(builder_, loc);
                            });
          scf::YieldOp::create(builder_, loc, ValueRange{next});
        });
    auto result = scf::IfOp::create(builder_, loc, TypeRange{resultType},
                                    compare(loc, arith::CmpIPredicate::eq, count, z), true, true);
    {
      OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(&result.getThenRegion().front());
      Value empty = isa<IntegerType>(elementType)
                        ? Value(arith::ConstantIntOp::create(
                              builder_, loc, 0, cast<IntegerType>(elementType).getWidth()))
                    : elementType.isF64()
                        ? Value(arith::ConstantFloatOp::create(builder_, loc, builder_.getF64Type(),
                                                               llvm::APFloat(0.0)))
                        : zero(loc);
      if (width != 1)
        empty = vector::BroadcastOp::create(builder_, loc, vectorType, empty);
      scf::YieldOp::create(builder_, loc, ValueRange{empty});
      builder_.setInsertionPointToStart(&result.getElseRegion().front());
      auto value = load(z);
      scf::YieldOp::create(builder_, loc, ValueRange{value});
    }
    return result.getResult(0);
  }
  template <class Callback>
  Value reduction(Location loc, Value count, Value scratch, Callback element, unsigned width = 1,
                  bool unsignedValue = false) {
    const auto z = index(loc, 0), one = index(loc, 1);
    scf::ForOp::create(builder_, loc, z, count, one, ValueRange{},
                       [&](OpBuilder &, Location, Value i, ValueRange) {
                         scratchStore(loc, element(i), scratch, i, width);
                         scf::YieldOp::create(builder_, loc);
                       });
    return scratchReduction(loc, count, scratch, width, {}, unsignedValue);
  }
  // A power-of-two block preserves the exact adjacent-pair reduction tree.
  // Keep the allocation bound and register subtree size coupled.
  static unsigned matmulReductionBlock(Type element) { return element.isF64() ? 16 : 32; }
  template <class Callback>
  Value registerBlockReduction(Location loc, Value count, Value scratch, Callback element,
                               unsigned width) {
    const auto blockSize =
        matmulReductionBlock(cast<MemRefType>(scratch.getType()).getElementType());
    const auto z = index(loc, 0), one = index(loc, 1), block = index(loc, blockSize);
    auto groups = arith::DivUIOp::create(builder_, loc, count, block);
    auto tail = arith::RemUIOp::create(builder_, loc, count, block);
    const auto base = index(loc, blockSize * width);
    // Consecutive power-of-two leaves form one exact subtree in SSA/registers,
    // writing only the root rather than every product. Float64 retains sixteen
    // leaves: larger subtrees regress its measured kernel throughput.
    const auto tree = [&](auto &&self, Value first, unsigned start, unsigned length) -> Value {
      if (length == 1)
        return element(arith::AddIOp::create(builder_, loc, first, index(loc, start)));
      auto left = self(self, first, start, length / 2);
      auto right = self(self, first, start + length / 2, length / 2);
      return arith::AddFOp::create(builder_, loc, left, right);
    };
    scf::ForOp::create(builder_, loc, z, groups, one, ValueRange{},
                       [&](OpBuilder &, Location, Value i, ValueRange) {
                         auto first = arith::MulIOp::create(builder_, loc, i, block);
                         scratchStore(loc, tree(tree, first, 0, blockSize), scratch, i, width,
                                      base);
                         scf::YieldOp::create(builder_, loc);
                       });
    auto hasTail = compare(loc, arith::CmpIPredicate::ne, tail, z);
    auto partial = scf::IfOp::create(builder_, loc, hasTail, false);
    {
      OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(&partial.getThenRegion().front());
      auto first = arith::MulIOp::create(builder_, loc, groups, block);
      // Never pad missing leaves with zeros: signed zero and odd carry order
      // remain identical to the specified adjacent-pair tree.
      auto value = reduction(loc, tail, scratch,
                             [&](Value i) { return element(arith::AddIOp::create(builder_, loc, first, i)); },
                             width);
      scratchStore(loc, value, scratch, groups, width, base);
      // Result-free scf.if already owns its implicit yield.
    }
    auto roots = arith::AddIOp::create(
        builder_, loc, groups, arith::SelectOp::create(builder_, loc, hasTail, one, z));
    return scratchReduction(loc, roots, scratch, width, base);
  }
  Value matmulKernel(Location loc, Value a, Value b, RankedTensorType output,
                     const Matmul &operation, bool wide, bool denseRight = false) {
    if (operation.order != ReductionOrder::BalancedAdjacent || !operation.separatelyRoundedProducts)
      throw LoweringFailure{"unsupported matmul numerical policy"};
    auto left = dimensions(loc, a), right = dimensions(loc, b);
    Value denseBuffer, packedRight;
    if (denseRight) {
      auto cell = output.getElementType();
      auto dynamicLayout = StridedLayoutAttr::get(builder_.getContext(), ShapedType::kDynamic,
                                                  {ShapedType::kDynamic, ShapedType::kDynamic});
      auto dynamicType =
          MemRefType::get(cast<RankedTensorType>(b.getType()).getShape(), cell, dynamicLayout);
      auto buffer = bufferization::ToBufferOp::create(builder_, loc, dynamicType, b, true);
      auto metadata = memref::ExtractStridedMetadataOp::create(builder_, loc, buffer);
      auto layout = StridedLayoutAttr::get(builder_.getContext(), ShapedType::kDynamic,
                                           {ShapedType::kDynamic, 1});
      auto type = MemRefType::get({ShapedType::kDynamic, ShapedType::kDynamic}, cell, layout);
      denseBuffer = memref::ReinterpretCastOp::create(
          builder_, loc, type, metadata.getSource(), metadata.getOffset(),
          ArrayRef<OpFoldResult>{right[0], right[1]},
          ArrayRef<OpFoldResult>{metadata.getStrides()[0], builder_.getIndexAttr(1)});
    }
    if (operation.checkInnerExtent)
      guard(loc, compare(loc, arith::CmpIPredicate::eq, left[1], right[0]), 1);
    auto cells =
        checkedCount(loc, {left[0], right[1]}, output.getElementType().getIntOrFloatBitWidth() / 8);
    const auto z = index(loc, 0), one = index(loc, 1), two = index(loc, 2), four = index(loc, 4);
    const unsigned tileRows = output.getElementType().isF64() ? 1 : 2;
    const auto rowStep = index(loc, tileRows), eight = index(loc, 8);
    auto noCells = compare(loc, arith::CmpIPredicate::eq, cells, z);
    auto rowCount = arith::SelectOp::create(builder_, loc, noCells, z, left[0]);
    auto hasPair = compare(loc, arith::CmpIPredicate::uge, right[1], two);
    auto narrowWidth = arith::SelectOp::create(builder_, loc, hasPair, two, one);
    auto hasTile = compare(loc, arith::CmpIPredicate::uge, right[1], four);
    auto smallWidth = arith::SelectOp::create(builder_, loc, hasTile, four, narrowWidth);
    Value width = smallWidth, count = left[1];
    if (wide) {
      width = index(loc, tileRows * 8);
      const auto blockSize = matmulReductionBlock(output.getElementType());
      auto groups = arith::DivUIOp::create(builder_, loc, left[1], index(loc, blockSize));
      count = arith::AddIOp::create(builder_, loc, groups, index(loc, blockSize + 1));
    }
    // Empty output must never size scratch from an unused, potentially enormous
    // contraction dimension. All paths/edge kernels reuse this one allocation.
    auto scratchCount = arith::SelectOp::create(builder_, loc, noCells, z, count);
    auto scratch =
        reductionScratch(loc,
                         checkedCount(loc, {scratchCount, width},
                                      output.getElementType().getIntOrFloatBitWidth() / 8),
                         output.getElementType());
    if (denseRight) {
      // Pack full eight-column panels once. A contraction then streams a small
      // contiguous panel instead of walking the RHS at its full row pitch.
      auto panels = arith::DivUIOp::create(builder_, loc, right[1], eight);
      packedRight = reductionScratch(
          loc, checkedCount(loc, {panels, left[1], eight},
                            output.getElementType().getIntOrFloatBitWidth() / 8),
          output.getElementType());
      scf::ForOp::create(builder_, loc, z, panels, one, ValueRange{},
                        [&](OpBuilder &, Location, Value panel, ValueRange) {
                          scf::ForOp::create(builder_, loc, z, left[1], one, ValueRange{},
                                            [&](OpBuilder &, Location, Value k, ValueRange) {
                                              auto first = arith::MulIOp::create(builder_, loc, panel, eight);
                                              auto row = arith::AddIOp::create(
                                                  builder_, loc, arith::MulIOp::create(builder_, loc, panel, left[1]), k);
                                              auto base = arith::MulIOp::create(builder_, loc, row, eight);
                                              for (unsigned c = 0; c < 8; ++c) {
                                                auto column = arith::AddIOp::create(builder_, loc, first, index(loc, c));
                                                auto value = memref::LoadOp::create(builder_, loc, denseBuffer, ValueRange{k, column});
                                                memref::StoreOp::create(builder_, loc, value, packedRight,
                                                    ValueRange{arith::AddIOp::create(builder_, loc, base, index(loc, c))});
                                              }
                                              scf::YieldOp::create(builder_, loc);
                                            });
                          scf::YieldOp::create(builder_, loc);
                        });
    }
    auto remainder = arith::RemUIOp::create(builder_, loc, right[1], four);
    auto tiledColumns = arith::SubIOp::create(builder_, loc, right[1], remainder);
    SmallVector<Value> dynamic;
    if (output.isDynamicDim(0))
      dynamic.push_back(left[0]);
    if (output.isDynamicDim(1))
      dynamic.push_back(right[1]);
    auto empty =
        tensor::EmptyOp::create(builder_, loc, output.getShape(), output.getElementType(), dynamic);
    // Explicit serial loops bufferize in place. Scratch is invocation-local;
    // tensor.generate would not guarantee exclusive use of mutable scratch.
    const auto tile = [&](Value row, Value column, Value input, unsigned rows, unsigned columns,
                          bool blocked) -> Value {
      const unsigned lanes = rows * columns;
      const auto vectorType = VectorType::get({lanes}, output.getElementType());
      const auto product = [&](Value k) -> Value {
        Value singleX;
        if (rows == 1)
          singleX = tensor::ExtractOp::create(builder_, loc, a, ValueRange{row, k});
        SmallVector<Value> values;
        for (unsigned c = 0; c < columns; ++c) {
          auto col = arith::AddIOp::create(builder_, loc, column, index(loc, c));
          if (denseRight && columns == 8) {
            auto panel = arith::DivUIOp::create(builder_, loc, column, eight);
            auto packedRow = arith::AddIOp::create(builder_, loc,
                arith::MulIOp::create(builder_, loc, panel, left[1]), k);
            auto at = arith::AddIOp::create(builder_, loc,
                arith::MulIOp::create(builder_, loc, packedRow, eight), index(loc, c));
            values.push_back(memref::LoadOp::create(builder_, loc, packedRight, ValueRange{at}));
          } else
            values.push_back(denseRight
                                 ? Value(memref::LoadOp::create(builder_, loc, denseBuffer, ValueRange{k, col}))
                                 : Value(tensor::ExtractOp::create(builder_, loc, b, ValueRange{k, col})));
        }
        if (lanes == 1) {
          return arith::MulFOp::create(builder_, loc, singleX, values[0]);
        }
        Value lhs, rhs;
        if (rows == 1) {
          lhs = vector::BroadcastOp::create(builder_, loc, vectorType, singleX);
          rhs = vector::FromElementsOp::create(builder_, loc, vectorType, values);
        } else {
          SmallVector<Value> xs, ys;
          for (unsigned r = 0; r < rows; ++r) {
            auto at = arith::AddIOp::create(builder_, loc, row, index(loc, r));
            auto x = tensor::ExtractOp::create(builder_, loc, a, ValueRange{at, k});
            for (unsigned c = 0; c < columns; ++c) {
              xs.push_back(x);
              ys.push_back(values[c]); // Reuse RHS loads across all four rows.
            }
          }
          lhs = vector::FromElementsOp::create(builder_, loc, vectorType, xs);
          rhs = vector::FromElementsOp::create(builder_, loc, vectorType, ys);
        }
        return arith::MulFOp::create(builder_, loc, lhs, rhs);
      };
      auto sum = blocked ? registerBlockReduction(loc, left[1], scratch, product, lanes)
                         : reduction(loc, left[1], scratch, product, lanes);
      Value result = input;
      for (unsigned r = 0; r < rows; ++r)
        for (unsigned c = 0; c < columns; ++c) {
          Value value = sum;
          if (lanes != 1)
            value = vector::ExtractOp::create(builder_, loc, sum, std::int64_t(r * columns + c));
          auto at = arith::AddIOp::create(builder_, loc, row, index(loc, r));
          auto col = arith::AddIOp::create(builder_, loc, column, index(loc, c));
          result = tensor::InsertOp::create(builder_, loc, value, result, ValueRange{at, col});
        }
      return result;
    };
    const auto rowKernel = [&](unsigned lanes, Value input, Value firstColumn,
                               bool blocked) -> Value {
      auto rows = scf::ForOp::create(
          builder_, loc, z, rowCount, one, ValueRange{input},
          [&](OpBuilder &, Location, Value row, ValueRange rowValues) {
            Value result = rowValues[0];
            if (lanes == 4) {
              auto tiles = scf::ForOp::create(
                  builder_, loc, firstColumn, tiledColumns, four, ValueRange{result},
                  [&](OpBuilder &, Location, Value column, ValueRange values) {
                    auto value = tile(row, column, values[0], 1, 4, blocked);
                    scf::YieldOp::create(builder_, loc, ValueRange{value});
                  });
              auto pair = scf::IfOp::create(builder_, loc, TypeRange{empty.getType()},
                                            compare(loc, arith::CmpIPredicate::uge, remainder, two),
                                            true, true);
              {
                OpBuilder::InsertionGuard guard(builder_);
                builder_.setInsertionPointToStart(&pair.getThenRegion().front());
                auto value = tile(row, tiledColumns, tiles.getResult(0), 1, 2, blocked);
                scf::YieldOp::create(builder_, loc, ValueRange{value});
                builder_.setInsertionPointToStart(&pair.getElseRegion().front());
                scf::YieldOp::create(builder_, loc, tiles.getResults());
              }
              result = pair.getResult(0);
            } else if (lanes == 2)
              result = tile(row, z, result, 1, 2, blocked);
            if (lanes == 1)
              result = tile(row, z, result, 1, 1, blocked);
            else {
              auto odd = arith::RemUIOp::create(builder_, loc, right[1], two);
              auto tail =
                  scf::IfOp::create(builder_, loc, TypeRange{empty.getType()},
                                    compare(loc, arith::CmpIPredicate::ne, odd, z), true, true);
              {
                OpBuilder::InsertionGuard guard(builder_);
                builder_.setInsertionPointToStart(&tail.getThenRegion().front());
                auto col = arith::SubIOp::create(builder_, loc, right[1], one);
                auto value = tile(row, col, result, 1, 1, blocked);
                scf::YieldOp::create(builder_, loc, ValueRange{value});
                builder_.setInsertionPointToStart(&tail.getElseRegion().front());
                scf::YieldOp::create(builder_, loc, ValueRange{result});
              }
              result = tail.getResult(0);
            }
            scf::YieldOp::create(builder_, loc, ValueRange{result});
          });
      return rows.getResult(0);
    };
    if (wide) {
      auto fullRows = arith::SubIOp::create(builder_, loc, rowCount,
                                            arith::RemUIOp::create(builder_, loc, rowCount, rowStep));
      auto fullColumns = arith::SubIOp::create(
          builder_, loc, right[1], arith::RemUIOp::create(builder_, loc, right[1], eight));
      auto main =
          scf::ForOp::create(builder_, loc, z, fullRows, rowStep, ValueRange{empty},
                             [&](OpBuilder &, Location, Value row, ValueRange values) {
                               auto cols = scf::ForOp::create(
                                   builder_, loc, z, fullColumns, eight, values,
                                   [&](OpBuilder &, Location, Value col, ValueRange outputs) {
                                     auto result = tile(row, col, outputs[0], tileRows, 8, true);
                                     scf::YieldOp::create(builder_, loc, ValueRange{result});
                                   });
                               scf::YieldOp::create(builder_, loc, cols.getResults());
                             });
      // Remaining rows still process eight columns together. Column edges
      // use 4/2/1 lanes, but MUST use blocked reductions with this scratch
      // bound.
      auto bottom =
          scf::ForOp::create(builder_, loc, fullRows, rowCount, one, main.getResults(),
                             [&](OpBuilder &, Location, Value row, ValueRange values) {
                               auto cols = scf::ForOp::create(
                                   builder_, loc, z, fullColumns, eight, values,
                                   [&](OpBuilder &, Location, Value col, ValueRange outputs) {
                                     auto result = tile(row, col, outputs[0], 1, 8, true);
                                     scf::YieldOp::create(builder_, loc, ValueRange{result});
                                   });
                               scf::YieldOp::create(builder_, loc, cols.getResults());
                             });
      auto edge = scf::IfOp::create(builder_, loc, TypeRange{empty.getType()},
                                    compare(loc, arith::CmpIPredicate::ne, fullColumns, right[1]),
                                    true, true);
      {
        OpBuilder::InsertionGuard guard(builder_);
        builder_.setInsertionPointToStart(&edge.getThenRegion().front());
        auto value = rowKernel(4, bottom.getResult(0), fullColumns, true);
        scf::YieldOp::create(builder_, loc, ValueRange{value});
        builder_.setInsertionPointToStart(&edge.getElseRegion().front());
        scf::YieldOp::create(builder_, loc, bottom.getResults());
      }
      return edge.getResult(0);
    }
    auto small = scf::IfOp::create(builder_, loc, TypeRange{empty.getType()}, hasTile, true, true);
    {
      OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(&small.getThenRegion().front());
      auto value = rowKernel(4, empty, z, false);
      scf::YieldOp::create(builder_, loc, ValueRange{value});
      builder_.setInsertionPointToStart(&small.getElseRegion().front());
      auto narrow =
          scf::IfOp::create(builder_, loc, TypeRange{empty.getType()}, hasPair, true, true);
      {
        OpBuilder::InsertionGuard guard(builder_);
        builder_.setInsertionPointToStart(&narrow.getThenRegion().front());
        auto pair = rowKernel(2, empty, z, false);
        scf::YieldOp::create(builder_, loc, ValueRange{pair});
        builder_.setInsertionPointToStart(&narrow.getElseRegion().front());
        auto single = rowKernel(1, empty, z, false);
        scf::YieldOp::create(builder_, loc, ValueRange{single});
      }
      scf::YieldOp::create(builder_, loc, narrow.getResults());
    }
    return small.getResult(0);
  }
  Value scalarMatmul(Location loc, Value a, Value b, RankedTensorType output,
                     const Matmul &operation, bool unsignedValue) {
    if (operation.order != ReductionOrder::BalancedAdjacent || !operation.separatelyRoundedProducts)
      throw LoweringFailure{"unsupported matmul numerical policy"};
    auto left = dimensions(loc, a), right = dimensions(loc, b);
    const auto z = index(loc, 0), one = index(loc, 1);
    for (auto extent : {left[0], left[1], right[0], right[1]})
      guard(loc, compare(loc, arith::CmpIPredicate::sge, extent, z), 1);
    if (operation.checkInnerExtent)
      guard(loc, compare(loc, arith::CmpIPredicate::eq, left[1], right[0]), 1);
    const auto cellType = output.getElementType();
    auto cells = checkedCount(loc, {left[0], right[1]}, cellType.getIntOrFloatBitWidth() / 8);
    auto noCells = compare(loc, arith::CmpIPredicate::eq, cells, z);
    auto rows = arith::SelectOp::create(builder_, loc, noCells, z, left[0]);
    // No output cells means no contraction work and no contraction-sized scratch.
    auto scratchCount = arith::SelectOp::create(builder_, loc, noCells, z, left[1]);
    auto scratch = reductionScratch(loc, scratchCount, cellType);
    SmallVector<Value> dynamic;
    if (output.isDynamicDim(0))
      dynamic.push_back(left[0]);
    if (output.isDynamicDim(1))
      dynamic.push_back(right[1]);
    auto empty = tensor::EmptyOp::create(builder_, loc, output.getShape(), cellType, dynamic);
    auto result = scf::ForOp::create(
        builder_, loc, z, rows, one, ValueRange{empty},
        [&](OpBuilder &, Location, Value row, ValueRange rowValues) {
          auto columns = scf::ForOp::create(
              builder_, loc, z, right[1], one, rowValues,
              [&](OpBuilder &, Location, Value column, ValueRange columnValues) {
                auto sum = reduction(
                    loc, left[1], scratch,
                    [&](Value k) -> Value {
                      auto x = tensor::ExtractOp::create(builder_, loc, a, ValueRange{row, k});
                      auto y = tensor::ExtractOp::create(builder_, loc, b, ValueRange{k, column});
                      return isa<IntegerType>(cellType)
                                 ? integerBinary(loc, x, y, TokenKind::Star, unsignedValue)
                                 : binary(loc, "arith.mulf", x, y);
                    },
                    1, unsignedValue);
                auto cell = tensor::InsertOp::create(builder_, loc, sum, columnValues[0],
                                                     ValueRange{row, column});
                scf::YieldOp::create(builder_, loc, ValueRange{cell});
              });
          scf::YieldOp::create(builder_, loc, columns.getResults());
        });
    return result.getResult(0);
  }
  Value matmul(Location loc, Value a, Value b, RankedTensorType output, const Matmul &operation,
               NumericType element) {
    if (isInteger(element) || arithmetic_ == ArithmeticMode::Software)
      return scalarMatmul(loc, a, b, output, operation, isUnsigned(element));
    // Outlining prevents a large SIMD kernel's register/stack requirements from
    // penalizing narrow shapes in the surrounding source function.
    func::FuncOp helper;
    {
      OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToEnd(module_->getBody());
      helper = func::FuncOp::create(
          builder_, loc, "kalandra_fn_matmul_wide_" + std::to_string(nextMatmulHelper_++),
          builder_.getFunctionType({a.getType(), b.getType()}, {output}));
      helper.setPrivate();
      builder_.setInsertionPointToStart(helper.addEntryBlock());
      // Specialize only a proven unit column stride. Row stride, offsets and
      // general/non-contiguous layouts retain their original descriptor meaning.
      auto layout = StridedLayoutAttr::get(builder_.getContext(), ShapedType::kDynamic,
                                          {ShapedType::kDynamic, ShapedType::kDynamic});
      auto bufferType =
          MemRefType::get(cast<RankedTensorType>(helper.getArgument(1).getType()).getShape(),
                          output.getElementType(), layout);
      auto buffer =
          bufferization::ToBufferOp::create(builder_, loc, bufferType, helper.getArgument(1), true);
      auto metadata = memref::ExtractStridedMetadataOp::create(builder_, loc, buffer);
      Value dense = compare(loc, arith::CmpIPredicate::eq, metadata.getStrides()[1], index(loc, 1));
      auto rows = tensor::DimOp::create(builder_, loc, helper.getArgument(0), index(loc, 0));
      for (auto condition : {
             compare(loc, arith::CmpIPredicate::uge, rows, index(loc, 32)),
             compare(loc, arith::CmpIPredicate::uge, metadata.getSizes()[0], index(loc, 128)),
             compare(loc, arith::CmpIPredicate::uge, metadata.getSizes()[1], index(loc, 32))})
        dense = arith::AndIOp::create(builder_, loc, dense, condition);
      auto choice = scf::IfOp::create(builder_, loc, TypeRange{output}, dense, true, true);
      {
        OpBuilder::InsertionGuard guard(builder_);
        builder_.setInsertionPointToStart(&choice.getThenRegion().front());
        auto result = matmulKernel(loc, helper.getArgument(0), helper.getArgument(1), output,
                                   operation, true, true);
        scf::YieldOp::create(builder_, loc, ValueRange{result});
        builder_.setInsertionPointToStart(&choice.getElseRegion().front());
        result = matmulKernel(loc, helper.getArgument(0), helper.getArgument(1), output,
                              operation, true, false);
        scf::YieldOp::create(builder_, loc, ValueRange{result});
      }
      func::ReturnOp::create(builder_, loc, choice.getResults());
    }
    auto left = dimensions(loc, a), right = dimensions(loc, b);
    auto useWide = arith::AndIOp::create(
        builder_, loc, compare(loc, arith::CmpIPredicate::uge, left[0], index(loc, 4)),
        compare(loc, arith::CmpIPredicate::uge, right[1], index(loc, 16)));
    auto dispatch = scf::IfOp::create(builder_, loc, TypeRange{output}, useWide, true, true);
    {
      OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(&dispatch.getThenRegion().front());
      auto call = func::CallOp::create(builder_, loc, helper, ValueRange{a, b});
      scf::YieldOp::create(builder_, loc, call.getResults());
      builder_.setInsertionPointToStart(&dispatch.getElseRegion().front());
      auto small = matmulKernel(loc, a, b, output, operation, false);
      scf::YieldOp::create(builder_, loc, ValueRange{small});
    }
    return dispatch.getResult(0);
  }
  Value reduce(Location loc, Value tensor, const ReduceSum &operation, NumericType element) {
    if (operation.order != ReductionOrder::BalancedAdjacent || !operation.rowMajor)
      throw LoweringFailure{"unsupported reduction numerical policy"};
    auto shape = dimensions(loc, tensor);
    auto count = checkedCount(loc, shape, describeNumeric(element)->bytes());
    auto scratch =
        reductionScratch(loc, count, cast<RankedTensorType>(tensor.getType()).getElementType());
    return reduction(
        loc, count, scratch,
        [&](Value linear) -> Value {
          SmallVector<Value> indices(shape.size());
          for (std::size_t axis = shape.size(); axis > 0; --axis) {
            if (axis == 1)
              indices[0] = linear;
            else {
              indices[axis - 1] = arith::RemUIOp::create(builder_, loc, linear, shape[axis - 1]);
              linear = arith::DivUIOp::create(builder_, loc, linear, shape[axis - 1]);
            }
          }
          return tensor::ExtractOp::create(builder_, loc, tensor, indices);
        },
        1, isUnsigned(element));
  }
  Value convertTensor(Location loc, Value input, RankedTensorType output, NumericType source,
                      NumericType destination) {
    auto shape = dimensions(loc, input);
    checkedCount(loc, shape, describeNumeric(source)->bytes());
    checkedCount(loc, shape, describeNumeric(destination)->bytes());
    SmallVector<Value> dynamic;
    for (std::size_t axis = 0; axis < shape.size(); ++axis)
      if (output.isDynamicDim(axis))
        dynamic.push_back(shape[axis]);
    Value empty =
        tensor::EmptyOp::create(builder_, loc, output.getShape(), output.getElementType(), dynamic);
    SmallVector<Value> indices;
    auto zero = index(loc, 0), one = index(loc, 1);
    std::function<Value(std::size_t, Value)> loop = [&](std::size_t axis, Value tensor) -> Value {
      if (axis == shape.size()) {
        Value lane = tensor::ExtractOp::create(builder_, loc, input, indices);
        auto [converted, valid] =
            numericConversion(builder_, loc, lane, source, destination, arithmetic_);
        guard(loc, valid, 4);
        return tensor::InsertOp::create(builder_, loc, converted, tensor, indices);
      }
      auto iteration = scf::ForOp::create(builder_, loc, zero, shape[axis], one, ValueRange{tensor},
                                          [&](OpBuilder &, Location, Value i, ValueRange args) {
                                            indices.push_back(i);
                                            auto result = loop(axis + 1, args[0]);
                                            indices.pop_back();
                                            scf::YieldOp::create(builder_, loc, ValueRange{result});
                                          });
      return iteration.getResult(0);
    };
    return loop(0, empty);
  }
  Value elementwise(Location loc, Value a, Value b, RankedTensorType output, TokenKind op,
                    NumericType element) {
    const bool tensorA = isa<RankedTensorType>(a.getType());
    const bool tensorB = b && isa<RankedTensorType>(b.getType());
    auto shape = dimensions(loc, tensorA ? a : b);
    if (tensorA && tensorB) {
      auto other = dimensions(loc, b);
      for (std::size_t axis = 0; axis < shape.size(); ++axis)
        guard(loc, compare(loc, arith::CmpIPredicate::eq, shape[axis], other[axis]), 1);
    }
    checkedCount(loc, shape, describeNumeric(element)->bytes());
    SmallVector<Value> dynamic;
    for (std::size_t axis = 0; axis < shape.size(); ++axis)
      if (output.isDynamicDim(axis))
        dynamic.push_back(shape[axis]);
    Value empty =
        tensor::EmptyOp::create(builder_, loc, output.getShape(), output.getElementType(), dynamic);
    SmallVector<Value> indices;
    const auto zero = index(loc, 0), one = index(loc, 1);
    std::function<Value(std::size_t, Value)> loop = [&](std::size_t axis, Value tensor) -> Value {
      if (axis == shape.size()) {
        Value left = tensorA ? tensor::ExtractOp::create(builder_, loc, a, indices).getResult() : a;
        Value result;
        if (!b) {
          if (op == TokenKind::Relu)
            result = relu(loc, left, element);
          else
            result = isInteger(element) ? integerUnary(loc, left, op, isUnsigned(element))
                                        : arith::NegFOp::create(builder_, loc, left).getResult();
        } else {
          Value right =
              tensorB ? tensor::ExtractOp::create(builder_, loc, b, indices).getResult() : b;
          const char *arithmetic = op == TokenKind::Plus    ? "arith.addf"
                                   : op == TokenKind::Minus ? "arith.subf"
                                   : op == TokenKind::Slash ? "arith.divf"
                                                            : "arith.mulf";
          result = isInteger(element) ? integerBinary(loc, left, right, op, isUnsigned(element))
                                      : binary(loc, arithmetic, left, right);
        }
        return tensor::InsertOp::create(builder_, loc, result, tensor, indices);
      }
      auto iteration = scf::ForOp::create(builder_, loc, zero, shape[axis], one, ValueRange{tensor},
                                          [&](OpBuilder &, Location, Value i, ValueRange args) {
                                            indices.push_back(i);
                                            auto result = loop(axis + 1, args[0]);
                                            indices.pop_back();
                                            scf::YieldOp::create(builder_, loc, ValueRange{result});
                                          });
      return iteration.getResult(0);
    };
    return loop(0, empty);
  }
  Value outlinedElementwise(Location loc, Value a, Value b, RankedTensorType output, TokenKind op,
                            NumericType element) {
    func::FuncOp helper;
    SmallVector<Value> operands{a};
    if (b)
      operands.push_back(b);
    {
      OpBuilder::InsertionGuard restore(builder_);
      builder_.setInsertionPointToEnd(module_->getBody());
      SmallVector<Type> parameters;
      for (auto operand : operands)
        parameters.push_back(operand.getType());
      helper = func::FuncOp::create(
          builder_, loc, "kalandra_fn_elementwise_" + std::to_string(nextElementwiseHelper_++),
          builder_.getFunctionType(parameters, {output}));
      builder_.setInsertionPointToStart(helper.addEntryBlock());
      auto result = elementwise(loc, helper.getArgument(0), b ? helper.getArgument(1) : Value{},
                                output, op, element);
      func::ReturnOp::create(builder_, loc, ValueRange{result});
    }
    return func::CallOp::create(builder_, loc, helper, operands).getResult(0);
  }
  struct Frame {
    bool outlineElementwise = false;
    std::unordered_map<ValueId, Value> values;
    std::unordered_map<SymbolId, Value> symbols;
  };
  // Fuse a pure floating map into the first level of its adjacent-pair sum.
  // Inputs stay immutable; only the half-sized private reduction scratch mutates.
  Value fusedMapReduction(Frame &frame, ValueId root, Location loc) {
    const auto &output = input_.expressions[root].type;
    if (arithmetic_ == ArithmeticMode::Software || isInteger(output.element) ||
        frame.values.contains(root) ||
        (!std::holds_alternative<TensorBinary>(input_.expressions[root].operation) &&
         !std::holds_alternative<TensorUnary>(input_.expressions[root].operation)))
      return {};
    std::vector<ValueId> leaves;
    std::function<bool(ValueId)> admit = [&](ValueId id) {
      const auto &node = input_.expressions[id];
      if (node.type.element != output.element || node.type.array)
        return false;
      if (!frame.values.contains(id)) {
        if (auto *op = std::get_if<TensorBinary>(&node.operation))
          return admit(op->left) && admit(op->right);
        if (auto *op = std::get_if<TensorUnary>(&node.operation))
          return admit(op->operand);
      }
      if (!frame.values.contains(id) && !std::holds_alternative<SymbolRead>(node.operation) &&
          !std::holds_alternative<Float32Literal>(node.operation) &&
          !std::holds_alternative<Float64Literal>(node.operation))
        return false;
      if (node.type.tensor && node.type.shape.size() != output.shape.size())
        return false;
      if (std::ranges::find(leaves, id) == leaves.end())
        leaves.push_back(id);
      return true;
    };
    if (!admit(root))
      return {};
    std::unordered_map<ValueId, Value> values;
    SmallVector<Value> shape;
    bool shaped = false;
    for (auto id : leaves) {
      auto value = expression(frame, id);
      values.emplace(id, value);
      if (!input_.expressions[id].type.tensor)
        continue;
      auto current = dimensions(loc, value);
      if (!shaped) {
        shape = current;
        shaped = true;
      } else
        for (std::size_t axis = 0; axis < shape.size(); ++axis)
          guard(loc, compare(loc, arith::CmpIPredicate::eq, shape[axis], current[axis]), 1);
    }
    if (!shaped)
      throw LoweringFailure{"tensor map reduction has no tensor input"};
    auto count = checkedCount(loc, shape, describeNumeric(output.element)->bytes());
    const auto z = index(loc, 0), one = index(loc, 1), two = index(loc, 2);
    auto pairs = arith::DivUIOp::create(builder_, loc, count, two);
    auto odd = arith::RemUIOp::create(builder_, loc, count, two);
    auto next = arith::AddIOp::create(builder_, loc, pairs, odd);
    auto cell = output.element == NumericType::Float64 ? Type(builder_.getF64Type())
                                                       : Type(builder_.getF32Type());
    auto scratch = reductionScratch(loc, next, cell);
    std::map<ValueId, memref::ExtractStridedMetadataOp> buffers;
    Value contiguous = arith::ConstantIntOp::create(builder_, loc, 1, 1);
    if (shape.size() == 1)
      for (auto id : leaves) {
        if (!input_.expressions[id].type.tensor)
          continue;
        auto layout = StridedLayoutAttr::get(builder_.getContext(), ShapedType::kDynamic,
                                             {ShapedType::kDynamic});
        auto bufferType = MemRefType::get(
            cast<RankedTensorType>(values.at(id).getType()).getShape(), cell, layout);
        auto buffer =
            bufferization::ToBufferOp::create(builder_, loc, bufferType, values.at(id), true);
        auto metadata = memref::ExtractStridedMetadataOp::create(builder_, loc, buffer);
        buffers.emplace(id, metadata);
        contiguous = arith::AndIOp::create(
            builder_, loc, contiguous,
            compare(loc, arith::CmpIPredicate::eq, metadata.getStrides()[0], one));
      }
    const auto run = [&](bool dense) -> Value {
      std::unordered_map<ValueId, Value> reads;
      if (dense)
        for (auto [id, metadata] : buffers) {
          auto layout = StridedLayoutAttr::get(builder_.getContext(), ShapedType::kDynamic, {1});
          auto bufferType = MemRefType::get({ShapedType::kDynamic}, cell, layout);
          reads[id] = memref::ReinterpretCastOp::create(
              builder_, loc, bufferType, metadata.getSource(), metadata.getOffset(),
              ArrayRef<OpFoldResult>{shape[0]}, ArrayRef<OpFoldResult>{builder_.getIndexAttr(1)});
        }
      std::function<Value(ValueId, ArrayRef<Value>)> lane = [&](ValueId id,
                                                                ArrayRef<Value> indices) -> Value {
        if (auto found = reads.find(id); found != reads.end())
          return memref::LoadOp::create(builder_, loc, found->second, indices);
        if (auto found = values.find(id); found != values.end())
          return input_.expressions[id].type.tensor
                     ? Value(tensor::ExtractOp::create(builder_, loc, found->second, indices))
                     : found->second;
        const auto &node = input_.expressions[id];
        if (auto *op = std::get_if<TensorUnary>(&node.operation)) {
          auto value = lane(op->operand, indices);
          if (op->operation == TokenKind::Relu)
            return relu(loc, value, node.type.element);
          return op->operation == TokenKind::Plus
                     ? value
                     : Value(arith::NegFOp::create(builder_, loc, value));
        }
        auto &op = std::get<TensorBinary>(node.operation);
        auto a = lane(op.left, indices), b = lane(op.right, indices);
        const char *name = op.operation == TokenKind::Plus    ? "arith.addf"
                           : op.operation == TokenKind::Minus ? "arith.subf"
                           : op.operation == TokenKind::Slash ? "arith.divf"
                                                              : "arith.mulf";
        return binary(loc, name, a, b);
      };
      const auto at = [&](Value linear) {
        SmallVector<Value> indices(shape.size());
        for (std::size_t axis = shape.size(); axis > 0; --axis) {
          if (axis == 1)
            indices[0] = linear;
          else {
            indices[axis - 1] = arith::RemUIOp::create(builder_, loc, linear, shape[axis - 1]);
            linear = arith::DivUIOp::create(builder_, loc, linear, shape[axis - 1]);
          }
        }
        return lane(root, indices);
      };
      scf::ForOp::create(builder_, loc, z, pairs, one, ValueRange{},
                         [&](OpBuilder &, Location, Value i, ValueRange) {
                           auto first = arith::MulIOp::create(builder_, loc, i, two);
                           auto second = arith::AddIOp::create(builder_, loc, first, one);
                           auto a = at(first), b = at(second);
                           auto sum = arith::AddFOp::create(builder_, loc, a, b);
                           scratchStore(loc, sum, scratch, i, 1);
                           scf::YieldOp::create(builder_, loc);
                         });
      scf::IfOp::create(builder_, loc, compare(loc, arith::CmpIPredicate::ne, odd, z),
                        [&](OpBuilder &, Location) {
                          scratchStore(loc, at(arith::SubIOp::create(builder_, loc, count, one)),
                                       scratch, pairs, 1);
                          scf::YieldOp::create(builder_, loc);
                        });
      return scratchReduction(loc, next, scratch);
    };
    if (shape.size() != 1)
      return run(false);
    auto result = scf::IfOp::create(builder_, loc, TypeRange{cell}, contiguous, true, true);
    {
      OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(&result.getThenRegion().front());
      scf::YieldOp::create(builder_, loc, ValueRange{run(true)});
      builder_.setInsertionPointToStart(&result.getElseRegion().front());
      scf::YieldOp::create(builder_, loc, ValueRange{run(false)});
    }
    return result.getResult(0);
  }

  Value expression(Frame &frame, ValueId id) {
    if (auto found = frame.values.find(id); found != frame.values.end())
      return found->second;
    const auto &node = input_.expressions[id];
    auto loc = location(node.span);
    auto value = std::visit(
        [&](const auto &operation) -> Value {
          using T = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<T, Float64Literal>) {
            llvm::APFloat number(llvm::APFloat::IEEEdouble(), llvm::APInt(64, operation.bits));
            return arith::ConstantFloatOp::create(builder_, loc, builder_.getF64Type(), number);
          } else if constexpr (std::is_same_v<T, Float32Literal>) {
            llvm::APFloat number(llvm::APFloat::IEEEsingle(), llvm::APInt(32, operation.bits));
            return arith::ConstantFloatOp::create(builder_, loc, builder_.getF32Type(), number);
          } else if constexpr (std::is_same_v<T, UInt64Literal>) {
            return arith::ConstantOp::create(
                builder_, loc, builder_.getI64Type(),
                builder_.getIntegerAttr(builder_.getI64Type(), llvm::APInt(64, operation.value)));
          } else if constexpr (std::is_same_v<T, Int32Literal> ||
                               std::is_same_v<T, UInt32Literal> ||
                               std::is_same_v<T, Int64Literal>) {
            return arith::ConstantIntOp::create(builder_, loc, operation.value,
                                                describeNumeric(node.type.element)->bits);
          } else if constexpr (std::is_same_v<T, SymbolRead>)
            return frame.symbols.at(operation.symbol);
          else if constexpr (std::is_same_v<T, ScalarUnary>) {
            auto operand = expression(frame, operation.operand);
            if (isInteger(node.type.element))
              return integerUnary(loc, operand, operation.operation, isUnsigned(node.type.element));
            return operation.operation == TokenKind::Minus
                       ? arith::NegFOp::create(builder_, loc, operand).getResult()
                       : operand;
          } else if constexpr (std::is_same_v<T, StringSlice>) {
            auto input = expression(frame, operation.input);
            Value start = expression(frame, operation.start);
            Value end = expression(frame, operation.end);
            const auto extend = [&](Value value, ValueId id) -> Value {
              const auto element = input_.expressions[id].type.element;
              if (describeNumeric(element)->bits >= 64)
                return value;
              if (isUnsigned(element))
                return arith::ExtUIOp::create(builder_, loc, builder_.getI64Type(), value);
              return arith::ExtSIOp::create(builder_, loc, builder_.getI64Type(), value);
            };
            start = extend(start, operation.start);
            end = extend(end, operation.end);
            auto source = stringSlot(loc, input), output = descriptorSlot(loc, type(node.type));
            func::CallOp::create(builder_, loc, "kalandra_string_slice", TypeRange{},
                                 ValueRange{output, source, start, end});
            return LLVM::LoadOp::create(builder_, loc, type(node.type), output);
          } else if constexpr (std::is_same_v<T, StringIndex>) {
            auto input = expression(frame, operation.input);
            Value index = expression(frame, operation.index);
            const auto element = input_.expressions[operation.index].type.element;
            if (describeNumeric(element)->bits < 64) {
              if (isUnsigned(element))
                index = arith::ExtUIOp::create(builder_, loc, builder_.getI64Type(), index);
              else
                index = arith::ExtSIOp::create(builder_, loc, builder_.getI64Type(), index);
            }
            auto source = stringSlot(loc, input), output = descriptorSlot(loc, type(node.type));
            func::CallOp::create(builder_, loc, "kalandra_string_index", TypeRange{},
                                 ValueRange{output, source, index});
            return LLVM::LoadOp::create(builder_, loc, type(node.type), output);
          } else if constexpr (std::is_same_v<T, StringQuery>) {
            auto value = expression(frame, operation.input);
            auto slot = stringSlot(loc, value);
            return func::CallOp::create(builder_, loc,
                                        operation.kind == StringQueryKind::Length
                                            ? "kalandra_string_length"
                                            : "kalandra_string_byte_length",
                                        TypeRange{builder_.getI64Type()}, ValueRange{slot})
                .getResult(0);
          } else if constexpr (std::is_same_v<T, StringConcat>) {
            auto a = expression(frame, operation.left), b = expression(frame, operation.right);
            auto left = stringSlot(loc, a), right = stringSlot(loc, b);
            auto output = descriptorSlot(loc, type(node.type));
            func::CallOp::create(builder_, loc, "kalandra_string_concat", TypeRange{},
                                 ValueRange{output, left, right});
            return LLVM::LoadOp::create(builder_, loc, type(node.type), output);
          } else if constexpr (std::is_same_v<T, StringLiteral>) {
            LLVM::GlobalOp global;
            {
              OpBuilder::InsertionGuard guard(builder_);
              builder_.setInsertionPointToStart(module_->getBody());
              const auto bytes = operation.value.empty() ? std::string(1, '\0') : operation.value;
              auto storage = LLVM::LLVMArrayType::get(builder_.getI8Type(), bytes.size());
              global =
                  LLVM::GlobalOp::create(builder_, loc, storage, true, LLVM::Linkage::Internal,
                                         "kalandra_string_" + std::to_string(nextStringLiteral_++),
                                         builder_.getStringAttr(bytes));
            }
            auto address = LLVM::AddressOfOp::create(builder_, loc, global);
            auto slot = descriptorSlot(loc, type(node.type));
            func::CallOp::create(builder_, loc, "kalandra_string_literal", TypeRange{},
                                 ValueRange{slot, address, integer(loc, operation.value.size())});
            return LLVM::LoadOp::create(builder_, loc, type(node.type), slot);
          } else if constexpr (std::is_same_v<T, BoolLiteral>) {
            return arith::ConstantIntOp::create(builder_, loc, operation.value, 1);
          } else if constexpr (std::is_same_v<T, RecordValue>) {
            Value value = operation.base
                              ? expression(frame, *operation.base)
                              : Value(LLVM::ZeroOp::create(builder_, loc, type(node.type)));
            if (operation.variant)
              value = LLVM::InsertValueOp::create(
                  builder_, loc, value, integer(loc, *operation.variant + 1), ArrayRef<int64_t>{0});
            for (const auto &field : operation.fields) {
              auto member = expression(frame, field.value);
              const auto &fieldType = input_.records[*node.type.record].fields[field.field].type;
              if (recordStorage_.fields[*node.type.record][field.field] ==
                  RecordFieldStorage::ManagedReference) {
                auto boxed = recursive_->box(loc, member, *fieldType.record);
                if (!boxed)
                  throw LoweringFailure{boxed.error()};
                member = *boxed;
              } else if (fieldType.tensor && !isOwnedArray(fieldType))
                member = recordPack(loc, member, fieldType);
              value =
                  LLVM::InsertValueOp::create(builder_, loc, value, member,
                                              ArrayRef<int64_t>{static_cast<int64_t>(field.field)});
            }
            return value;
          } else if constexpr (std::is_same_v<T, ErrorNarrow>) {
            return narrowError(loc, expression(frame, operation.input),
                               input_.expressions[operation.input].type, node.type);
          } else if constexpr (std::is_same_v<T, RecordFieldRead>) {
            Value member = LLVM::ExtractValueOp::create(
                builder_, loc, expression(frame, operation.input),
                ArrayRef<int64_t>{static_cast<int64_t>(operation.field)});
            const auto &container = input_.expressions[operation.input].type;
            if (recordStorage_.fields[*container.record][operation.field] ==
                RecordFieldStorage::ManagedReference) {
              auto loaded = recursive_->read(loc, member, *node.type.record);
              if (!loaded)
                throw LoweringFailure{loaded.error()};
              return *loaded;
            }
            if (isOwnedArray(node.type)) {
              recordArrayLength(loc, member, node.type);
              return member;
            }
            return node.type.tensor ? recordUnpack(loc, member, node.type) : member;
          } else if constexpr (std::is_same_v<T, ScalarCompare>) {
            auto left = expression(frame, operation.left),
                 right = expression(frame, operation.right);
            if (input_.expressions[operation.left].type.builtin == BuiltinValue::String) {
              auto a = stringSlot(loc, left), b = stringSlot(loc, right);
              auto equal = func::CallOp::create(builder_, loc, "kalandra_string_equal",
                                                TypeRange{builder_.getI64Type()}, ValueRange{a, b})
                               .getResult(0);
              return compare(loc,
                             operation.operation == ComparisonKind::Equal
                                 ? arith::CmpIPredicate::ne
                                 : arith::CmpIPredicate::eq,
                             equal, integer(loc, 0));
            }
            return scalarComparison(loc, left, right, input_.expressions[operation.left].type,
                                    operation.operation);
          } else if constexpr (std::is_same_v<T, ScalarBinary>) {
            auto a = expression(frame, operation.left), b = expression(frame, operation.right);
            if (isInteger(node.type.element))
              return integerBinary(loc, a, b, operation.operation, isUnsigned(node.type.element));
            const char *name = operation.operation == TokenKind::Plus    ? "arith.addf"
                               : operation.operation == TokenKind::Minus ? "arith.subf"
                               : operation.operation == TokenKind::Star  ? "arith.mulf"
                                                                         : "arith.divf";
            return binary(loc, name, a, b);
          } else if constexpr (std::is_same_v<T, NumericConvert>) {
            auto input = expression(frame, operation.input);
            const auto source = input_.expressions[operation.input].type.element;
            if (node.type.tensor)
              return convertTensor(loc, input, cast<RankedTensorType>(type(node.type)), source,
                                   node.type.element);
            auto [converted, valid] =
                numericConversion(builder_, loc, input, source, node.type.element, arithmetic_);
            guard(loc, valid, 4);
            return converted;
          } else if constexpr (std::is_same_v<T, TensorUnary>) {
            auto input = expression(frame, operation.operand);
            if (frame.outlineElementwise && node.type.element == NumericType::Float32)
              return outlinedElementwise(loc, input, {}, cast<RankedTensorType>(type(node.type)),
                                         operation.operation, node.type.element);
            return elementwise(loc, input, {}, cast<RankedTensorType>(type(node.type)),
                               operation.operation, node.type.element);
          } else if constexpr (std::is_same_v<T, TensorBinary>) {
            auto a = expression(frame, operation.left), b = expression(frame, operation.right);
            if (frame.outlineElementwise && node.type.element == NumericType::Float32)
              return outlinedElementwise(loc, a, b, cast<RankedTensorType>(type(node.type)),
                                         operation.operation, node.type.element);
            return elementwise(loc, a, b, cast<RankedTensorType>(type(node.type)),
                               operation.operation, node.type.element);
          } else if constexpr (std::is_same_v<T, ArrayIndex> || std::is_same_v<T, TensorIndex>) {
            if constexpr (std::is_same_v<T, ArrayIndex>) {
              const auto &array = input_.expressions[operation.input].type;
              if (isOwnedArray(array)) {
                auto input = expression(frame, operation.input);
                Value position = expression(frame, operation.index);
                const auto element = input_.expressions[operation.index].type.element;
                if (describeNumeric(element)->bits < 64) {
                  if (isUnsigned(element))
                    position =
                        arith::ExtUIOp::create(builder_, loc, builder_.getI64Type(), position);
                  else
                    position =
                        arith::ExtSIOp::create(builder_, loc, builder_.getI64Type(), position);
                }
                auto count = recordArrayLength(loc, input, array);
                guard(loc, compare(loc, arith::CmpIPredicate::ult, position, count), 5);
                auto storage =
                    LLVM::ExtractValueOp::create(builder_, loc, input, ArrayRef<int64_t>{1});
                auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
                auto cell = arrayCellType(array);
                auto address =
                    LLVM::GEPOp::create(builder_, loc, ptr, cell, storage, ValueRange{position});
                return arrayCellRead(loc, address, array);
              }
            }
            auto input = expression(frame, operation.input);
            std::vector<ValueId> indices;
            if constexpr (std::is_same_v<T, ArrayIndex>)
              indices = {operation.index};
            else
              indices = operation.indices;
            SmallVector<Value> positions, offsets;
            for (auto id : indices)
              positions.push_back(expression(frame, id));
            auto shape = dimensions(loc, input);
            checkedCount(loc, shape, describeNumeric(node.type.element)->bytes());
            for (std::size_t axis = 0; axis < indices.size(); ++axis) {
              auto position = positions[axis];
              const auto element = input_.expressions[indices[axis]].type.element;
              if (describeNumeric(element)->bits < 64) {
                if (isUnsigned(element))
                  position = arith::ExtUIOp::create(builder_, loc, builder_.getI64Type(), position);
                else
                  position = arith::ExtSIOp::create(builder_, loc, builder_.getI64Type(), position);
              }
              auto limit =
                  arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), shape[axis]);
              // Unsigned comparison rejects negative signed indices as well as
              // high uint64 indices, before any storage element is extracted.
              guard(loc, compare(loc, arith::CmpIPredicate::ult, position, limit), 5);
              offsets.push_back(
                  arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(), position));
            }
            return tensor::ExtractOp::create(builder_, loc, input, offsets);
          } else if constexpr (std::is_same_v<T, CollectionQuery>) {
            auto input = expression(frame, operation.input);
            const auto &array = input_.expressions[operation.input].type;
            if (isOwnedArray(array))
              return recordArrayLength(loc, input, array);
            auto shape = dimensions(loc, input);
            checkedCount(
                loc, shape,
                describeNumeric(input_.expressions[operation.input].type.element)->bytes());
            if (operation.kind == CollectionQueryKind::Length)
              return arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), shape[0]);
            const auto output = cast<RankedTensorType>(type(node.type));
            Value result =
                tensor::EmptyOp::create(builder_, loc, output.getShape(), output.getElementType());
            for (std::size_t axis = 0; axis < shape.size(); ++axis) {
              auto extent =
                  arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), shape[axis]);
              result = tensor::InsertOp::create(builder_, loc, extent, result,
                                                ValueRange{index(loc, axis)});
            }
            return result;
          } else if constexpr (std::is_same_v<T, ArrayToTensor> ||
                               std::is_same_v<T, TensorToArray> ||
                               std::is_same_v<T, TensorReshape>) {
            std::optional<std::vector<ValueId>> dimensionOperands;
            if constexpr (std::is_same_v<T, TensorToArray>)
              dimensionOperands.emplace();
            else
              dimensionOperands = operation.dimensions;
            if (dimensionOperands) {
              auto input = expression(frame, operation.input);
              auto inputShape = dimensions(loc, input);
              auto inputCount =
                  checkedCount(loc, inputShape, describeNumeric(node.type.element)->bytes());
              SmallVector<Value> shape, dynamic, evaluated;
              for (auto id : *dimensionOperands)
                evaluated.push_back(expression(frame, id));
              const auto output = cast<RankedTensorType>(type(node.type));
              if constexpr (std::is_same_v<T, TensorToArray>) {
                shape.push_back(inputCount);
                if (output.isDynamicDim(0))
                  dynamic.push_back(inputCount);
                else
                  guard(loc,
                        compare(loc, arith::CmpIPredicate::eq, inputCount,
                                index(loc, output.getDimSize(0))),
                        1);
              }
              for (std::size_t axis = 0; axis < dimensionOperands->size(); ++axis) {
                auto id = (*dimensionOperands)[axis];
                auto dimension = evaluated[axis];
                const auto element = input_.expressions[id].type.element;
                if (describeNumeric(element)->bits < 64) {
                  if (isUnsigned(element))
                    dimension =
                        arith::ExtUIOp::create(builder_, loc, builder_.getI64Type(), dimension);
                  else
                    dimension =
                        arith::ExtSIOp::create(builder_, loc, builder_.getI64Type(), dimension);
                }
                guard(loc,
                      compare(loc,
                              isUnsigned(element) ? arith::CmpIPredicate::ule
                                                  : arith::CmpIPredicate::sge,
                              dimension, integer(loc, isUnsigned(element) ? INT64_MAX : 0)),
                      1);
                auto d =
                    arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(), dimension);
                shape.push_back(d);
                if (output.isDynamicDim(axis))
                  dynamic.push_back(d);
                else
                  guard(loc,
                        compare(loc, arith::CmpIPredicate::eq, d,
                                index(loc, output.getDimSize(axis))),
                        1);
              }
              auto count = checkedCount(loc, shape, describeNumeric(node.type.element)->bytes());
              // Even a zero-element tensor needs representable row-major strides.
              Value stride = index(loc, 1);
              for (auto axis = shape.rbegin(); axis != shape.rend(); ++axis) {
                auto safe = arith::SelectOp::create(
                    builder_, loc, compare(loc, arith::CmpIPredicate::eq, *axis, index(loc, 0)),
                    index(loc, 1), *axis);
                auto limit = arith::DivUIOp::create(builder_, loc, index(loc, INT64_MAX), safe);
                auto zero = compare(loc, arith::CmpIPredicate::eq, *axis, index(loc, 0));
                guard(loc,
                      arith::OrIOp::create(builder_, loc, zero,
                                           compare(loc, arith::CmpIPredicate::ule, stride, limit)),
                      2);
                stride = arith::MulIOp::create(builder_, loc, stride, *axis);
              }
              guard(loc, compare(loc, arith::CmpIPredicate::eq, inputCount, count), 1);
              auto empty = tensor::EmptyOp::create(builder_, loc, output.getShape(),
                                                   output.getElementType(), dynamic);
              auto loop = scf::ForOp::create(
                  builder_, loc, index(loc, 0), count, index(loc, 1), ValueRange{empty},
                  [&](OpBuilder &, Location, Value i, ValueRange args) {
                    SmallVector<Value> sourceIndices;
                    if constexpr (std::is_same_v<T, ArrayToTensor>)
                      sourceIndices.push_back(i);
                    else {
                      sourceIndices.resize(inputShape.size());
                      Value remaining = i;
                      for (std::size_t axis = inputShape.size(); axis > 0; --axis) {
                        auto safe =
                            arith::SelectOp::create(builder_, loc,
                                                    compare(loc, arith::CmpIPredicate::eq,
                                                            inputShape[axis - 1], index(loc, 0)),
                                                    index(loc, 1), inputShape[axis - 1]);
                        sourceIndices[axis - 1] =
                            arith::RemUIOp::create(builder_, loc, remaining, safe);
                        remaining = arith::DivUIOp::create(builder_, loc, remaining, safe);
                      }
                    }
                    auto element = tensor::ExtractOp::create(builder_, loc, input, sourceIndices);
                    SmallVector<Value> indices(shape.size());
                    Value remaining = i;
                    for (std::size_t axis = shape.size(); axis > 0; --axis) {
                      auto safe = arith::SelectOp::create(
                          builder_, loc,
                          compare(loc, arith::CmpIPredicate::eq, shape[axis - 1], index(loc, 0)),
                          index(loc, 1), shape[axis - 1]);
                      indices[axis - 1] = arith::RemUIOp::create(builder_, loc, remaining, safe);
                      remaining = arith::DivUIOp::create(builder_, loc, remaining, safe);
                    }
                    auto inserted =
                        tensor::InsertOp::create(builder_, loc, element, args[0], indices);
                    scf::YieldOp::create(builder_, loc, ValueRange{inserted});
                  });
              return loop.getResult(0);
            }
            auto input = expression(frame, operation.input);
            auto shape = dimensions(loc, input);
            checkedCount(loc, shape, describeNumeric(node.type.element)->bytes());
            const auto output = cast<RankedTensorType>(type(node.type));
            SmallVector<Value> dynamic;
            if (output.isDynamicDim(0))
              dynamic.push_back(shape[0]);
            auto empty = tensor::EmptyOp::create(builder_, loc, output.getShape(),
                                                 output.getElementType(), dynamic);
            auto loop = scf::ForOp::create(
                builder_, loc, index(loc, 0), shape[0], index(loc, 1), ValueRange{empty},
                [&](OpBuilder &, Location, Value i, ValueRange args) {
                  auto element = tensor::ExtractOp::create(builder_, loc, input, ValueRange{i});
                  auto inserted =
                      tensor::InsertOp::create(builder_, loc, element, args[0], ValueRange{i});
                  scf::YieldOp::create(builder_, loc, ValueRange{inserted});
                });
            return loop.getResult(0);
          } else if constexpr (std::is_same_v<T, ArrayConcat>) {
            auto left = expression(frame, operation.left);
            auto right = expression(frame, operation.right);
            const bool owned = isOwnedArray(node.type);
            const auto length = [&](Value input) -> Value {
              if (owned)
                return arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(),
                                                  recordArrayLength(loc, input, node.type));
              auto shape = dimensions(loc, input);
              checkedCount(loc, shape, describeNumeric(node.type.element)->bytes());
              return shape[0];
            };
            auto a = length(left), b = length(right);
            auto remaining = arith::SubIOp::create(builder_, loc, index(loc, INT64_MAX), b);
            guard(loc, compare(loc, arith::CmpIPredicate::ule, a, remaining), 2);
            Value count = arith::AddIOp::create(builder_, loc, a, b);
            if (node.type.shape[0].constant)
              guard(loc,
                    compare(loc, arith::CmpIPredicate::eq, count,
                            index(loc, *node.type.shape[0].constant)),
                    1);
            Value empty;
            if (owned)
              empty = recordArrayStorage(loc, node.type, count);
            else {
              checkedCount(loc, {count}, describeNumeric(node.type.element)->bytes());
              auto output = cast<RankedTensorType>(type(node.type));
              SmallVector<Value> dynamic;
              if (output.isDynamicDim(0))
                dynamic.push_back(count);
              empty = tensor::EmptyOp::create(builder_, loc, output.getShape(),
                                              output.getElementType(), dynamic);
            }
            auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
            const auto append = [&](Value input, Value size, Value offset, Value output) -> Value {
              auto loop = scf::ForOp::create(
                  builder_, loc, index(loc, 0), size, index(loc, 1), ValueRange{output},
                  [&](OpBuilder &, Location, Value i, ValueRange args) {
                    Value targetIndex = arith::AddIOp::create(builder_, loc, offset, i);
                    Value result;
                    if (owned) {
                      auto position =
                          arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), i);
                      auto destinationIndex = arith::IndexCastOp::create(
                          builder_, loc, builder_.getI64Type(), targetIndex);
                      auto storage =
                          LLVM::ExtractValueOp::create(builder_, loc, input, ArrayRef<int64_t>{1});
                      auto cell = arrayCellType(node.type);
                      auto address = LLVM::GEPOp::create(builder_, loc, ptr, cell, storage,
                                                         ValueRange{position});
                      auto value = arrayCellRead(loc, address, node.type);
                      auto destination = LLVM::ExtractValueOp::create(builder_, loc, args[0],
                                                                      ArrayRef<int64_t>{1});
                      auto target = LLVM::GEPOp::create(builder_, loc, ptr, cell, destination,
                                                        ValueRange{destinationIndex});
                      arrayCellStore(loc, value, target, node.type);
                      result = args[0];
                    } else {
                      auto value = tensor::ExtractOp::create(builder_, loc, input, ValueRange{i});
                      result = tensor::InsertOp::create(builder_, loc, value, args[0],
                                                        ValueRange{targetIndex});
                    }
                    scf::YieldOp::create(builder_, loc, ValueRange{result});
                  });
              return loop.getResult(0);
            };
            auto first = append(left, a, index(loc, 0), empty);
            return append(right, b, a, first);
          } else if constexpr (std::is_same_v<T, ArraySlice>) {
            auto input = expression(frame, operation.input);
            Value start = expression(frame, operation.start);
            Value end = expression(frame, operation.end);
            const auto extend = [&](Value value, ValueId id) -> Value {
              const auto element = input_.expressions[id].type.element;
              if (describeNumeric(element)->bits >= 64)
                return value;
              if (isUnsigned(element))
                return arith::ExtUIOp::create(builder_, loc, builder_.getI64Type(), value);
              return arith::ExtSIOp::create(builder_, loc, builder_.getI64Type(), value);
            };
            start = extend(start, operation.start);
            end = extend(end, operation.end);
            const bool owned = isOwnedArray(node.type);
            Value length;
            if (owned)
              length = recordArrayLength(loc, input, node.type);
            else {
              auto shape = dimensions(loc, input);
              checkedCount(loc, shape, describeNumeric(node.type.element)->bytes());
              length = arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), shape[0]);
            }
            // Unsigned comparisons also reject negative signed bounds and high uint64 values.
            guard(loc, compare(loc, arith::CmpIPredicate::ule, start, end), 5);
            guard(loc, compare(loc, arith::CmpIPredicate::ule, end, length), 5);
            auto size = arith::SubIOp::create(builder_, loc, end, start);
            auto count = arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(), size);
            if (node.type.shape[0].constant)
              guard(loc,
                    compare(loc, arith::CmpIPredicate::eq, size,
                            integer(loc, *node.type.shape[0].constant)),
                    1);
            Value empty;
            if (owned)
              empty = recordArrayStorage(loc, node.type, count);
            else {
              checkedCount(loc, {count}, describeNumeric(node.type.element)->bytes());
              auto output = cast<RankedTensorType>(type(node.type));
              SmallVector<Value> dynamic;
              if (output.isDynamicDim(0))
                dynamic.push_back(count);
              empty = tensor::EmptyOp::create(builder_, loc, output.getShape(),
                                              output.getElementType(), dynamic);
            }
            auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
            auto loop = scf::ForOp::create(
                builder_, loc, index(loc, 0), count, index(loc, 1), ValueRange{empty},
                [&](OpBuilder &, Location, Value i, ValueRange args) {
                  auto position =
                      arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), i);
                  Value offset = arith::AddIOp::create(builder_, loc, start, position);
                  Value result;
                  if (owned) {
                    auto storage =
                        LLVM::ExtractValueOp::create(builder_, loc, input, ArrayRef<int64_t>{1});
                    auto cell = arrayCellType(node.type);
                    auto address =
                        LLVM::GEPOp::create(builder_, loc, ptr, cell, storage, ValueRange{offset});
                    auto value = arrayCellRead(loc, address, node.type);
                    auto destination =
                        LLVM::ExtractValueOp::create(builder_, loc, args[0], ArrayRef<int64_t>{1});
                    auto target = LLVM::GEPOp::create(builder_, loc, ptr, cell, destination,
                                                      ValueRange{position});
                    arrayCellStore(loc, value, target, node.type);
                    result = args[0];
                  } else {
                    auto source =
                        arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(), offset);
                    auto value =
                        tensor::ExtractOp::create(builder_, loc, input, ValueRange{source});
                    result = tensor::InsertOp::create(builder_, loc, value, args[0], ValueRange{i});
                  }
                  scf::YieldOp::create(builder_, loc, ValueRange{result});
                });
            return loop.getResult(0);
          } else if constexpr (std::is_same_v<T, ArrayMap>) {
            if (operation.parallel)
              throw std::runtime_error("parallelMap requires CPU continuation lowering");
            auto input = expression(frame, operation.input);
            SmallVector<Value> captures;
            for (auto capture : operation.captures)
              captures.push_back(expression(frame, capture));
            const auto &inputType = input_.expressions[operation.input].type;
            const bool ownedInput = isOwnedArray(inputType);
            const bool ownedOutput = isOwnedArray(node.type);
            Value count =
                ownedInput
                    ? Value(arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(),
                                                       recordArrayLength(loc, input, inputType)))
                    : dimensions(loc, input)[0];
            if (!ownedInput)
              checkedCount(loc, {count}, describeNumeric(inputType.element)->bytes());
            Value empty;
            if (ownedOutput) {
              empty = recordArrayStorage(loc, node.type, count);
            } else {
              checkedCount(loc, {count}, describeNumeric(node.type.element)->bytes());
              auto output = cast<RankedTensorType>(type(node.type));
              SmallVector<Value> dynamic;
              if (output.isDynamicDim(0))
                dynamic.push_back(count);
              empty = tensor::EmptyOp::create(builder_, loc, output.getShape(),
                                              output.getElementType(), dynamic);
            }
            auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
            auto loop = scf::ForOp::create(
                builder_, loc, index(loc, 0), count, index(loc, 1), ValueRange{empty},
                [&](OpBuilder &, Location, Value i, ValueRange args) {
                  Value element;
                  auto position =
                      arith::IndexCastOp::create(builder_, loc, builder_.getI64Type(), i);
                  if (ownedInput) {
                    auto storage =
                        LLVM::ExtractValueOp::create(builder_, loc, input, ArrayRef<int64_t>{1});
                    auto cell = arrayCellType(inputType);
                    auto address = LLVM::GEPOp::create(builder_, loc, ptr, cell, storage,
                                                       ValueRange{position});
                    element = arrayCellRead(loc, address, inputType);
                  } else {
                    element = tensor::ExtractOp::create(builder_, loc, input, ValueRange{i});
                  }
                  SmallVector<Value> arguments{element};
                  arguments.append(captures.begin(), captures.end());
                  auto call = func::CallOp::create(builder_, loc, functions_[operation.function],
                                                   arguments);
                  Value result;
                  if (ownedOutput) {
                    auto storage =
                        LLVM::ExtractValueOp::create(builder_, loc, args[0], ArrayRef<int64_t>{1});
                    auto cell = arrayCellType(node.type);
                    auto address = LLVM::GEPOp::create(builder_, loc, ptr, cell, storage,
                                                       ValueRange{position});
                    arrayCellStore(loc, call.getResult(0), address, node.type);
                    result = args[0];
                  } else {
                    result = tensor::InsertOp::create(builder_, loc, call.getResult(0), args[0],
                                                      ValueRange{i});
                  }
                  scf::YieldOp::create(builder_, loc, ValueRange{result});
                });
            return loop.getResult(0);
          } else if constexpr (std::is_same_v<T, TensorLiteral>) {
            SmallVector<Value> elements;
            for (auto child : operation.elements)
              elements.push_back(expression(frame, child));
            if (isOwnedArray(node.type))
              return recordArrayLiteral(loc, node.type, elements);
            return tensor::FromElementsOp::create(builder_, loc, type(node.type), elements);
          } else if constexpr (std::is_same_v<T, ConditionalOperation>) {
            auto condition = expression(frame, operation.condition);
            auto evaluate = [&](const std::vector<TypedStatement> &statements, ValueId result) {
              auto branch = frame;
              for (const auto &statement : statements) {
                auto value = expression(branch, statement.value);
                if (statement.binding)
                  branch.symbols[*statement.binding] = value;
              }
              auto value = castTensor(loc, expression(branch, result), type(node.type));
              scf::YieldOp::create(builder_, loc, ValueRange{value});
            };
            auto choice =
                scf::IfOp::create(builder_, loc, TypeRange{type(node.type)}, condition, true);
            {
              OpBuilder::InsertionGuard guard(builder_);
              builder_.setInsertionPointToStart(&choice.getThenRegion().front());
              evaluate(operation.whenTrue, operation.trueResult);
              builder_.setInsertionPointToStart(&choice.getElseRegion().front());
              evaluate(operation.whenFalse, operation.falseResult);
            }
            return choice.getResult(0);
          } else if constexpr (std::is_same_v<T, MatchOperation>) {
            auto subject = expression(frame, operation.subject);
            const auto &subjectType = input_.expressions[operation.subject].type;
            const auto count = input_.records[*subjectType.record].variants.size();
            auto tag = LLVM::ExtractValueOp::create(builder_, loc, subject, ArrayRef<int64_t>{0});
            guard(loc, compare(loc, arith::CmpIPredicate::uge, tag, integer(loc, 1)), 3);
            guard(loc, compare(loc, arith::CmpIPredicate::ule, tag, integer(loc, count)), 3);
            SmallVector<std::int64_t> tags;
            // The final checked arm is the default: either the explicit wildcard
            // or the final alternative of an exhaustive match. No nested chain.
            for (std::size_t i = 0; i + 1 < operation.cases.size(); ++i)
              tags.push_back(*operation.cases[i].tag);
            auto selector = arith::IndexCastOp::create(builder_, loc, builder_.getIndexType(), tag);
            auto choice =
                scf::IndexSwitchOp::create(builder_, loc, TypeRange{type(node.type)}, selector,
                                           ArrayRef<std::int64_t>(tags), tags.size());
            OpBuilder::InsertionGuard insertion(builder_);
            const auto evaluate = [&](const MatchCase &arm, Region &region) {
              builder_.createBlock(&region);
              auto branch = frame;
              for (const auto &statement : arm.body) {
                auto value = expression(branch, statement.value);
                if (statement.binding)
                  branch.symbols[*statement.binding] = value;
              }
              auto value = castTensor(loc, expression(branch, arm.result), type(node.type));
              scf::YieldOp::create(builder_, loc, ValueRange{value});
            };
            for (std::size_t i = 0; i < tags.size(); ++i)
              evaluate(operation.cases[i], choice.getCaseRegions()[i]);
            evaluate(operation.cases.back(), choice.getDefaultRegion());
            return choice.getResult(0);
          } else if constexpr (std::is_same_v<T, FunctionReference>) {
            auto function = callableEntry(operation.function);
            auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
            Value code = callableCode(loc, function);
            Value environment = LLVM::ZeroOp::create(builder_, loc, ptr);
            if (!operation.captures.empty()) {
              auto layout = closureEnvironment(operation.function);
              Value contents = LLVM::ZeroOp::create(builder_, loc, layout);
              for (std::size_t i = 0; i < operation.captures.size(); ++i) {
                auto capture = operation.captures[i];
                auto value =
                    callablePack(loc, expression(frame, capture), input_.expressions[capture].type);
                contents = LLVM::InsertValueOp::create(builder_, loc, contents, value,
                                                       ArrayRef<int64_t>{static_cast<int64_t>(i)});
              }
              llvm::LLVMContext llvmContext;
              LLVM::TypeToLLVMIRTranslator translator(llvmContext);
              llvm::DataLayout dataLayout(nativeCpuDataLayout);
              Value bytes = integer(
                  loc,
                  dataLayout.getTypeAllocSize(translator.translateType(layout)).getFixedValue());
              if (recursiveClosures_) {
                auto boxed =
                    recursive_->box(loc, contents, *environmentRecords_[operation.function]);
                if (!boxed)
                  throw LoweringFailure{boxed.error()};
                environment =
                    func::CallOp::create(builder_, loc, "kalandra_recursive_environment_bind",
                                         TypeRange{ptr}, ValueRange{*boxed, code})
                        .getResult(0);
              } else {
                environment = func::CallOp::create(builder_, loc, "kalandra_environment_alloc",
                                                   TypeRange{ptr}, ValueRange{bytes, code})
                                  .getResult(0);
                LLVM::StoreOp::create(builder_, loc, contents, environment);
              }
            }
            Value result = LLVM::ZeroOp::create(builder_, loc, type(node.type));
            result = LLVM::InsertValueOp::create(builder_, loc, result, code, ArrayRef<int64_t>{0});
            return LLVM::InsertValueOp::create(builder_, loc, result, environment,
                                               ArrayRef<int64_t>{1});
          } else if constexpr (std::is_same_v<T, FunctionInvoke>) {
            const auto &signature = *input_.expressions[operation.callee].type.callable;
            auto ptr = LLVM::LLVMPointerType::get(builder_.getContext());
            auto closure = expression(frame, operation.callee);
            Value pointer =
                LLVM::ExtractValueOp::create(builder_, loc, closure, ArrayRef<int64_t>{0});
            Value environment =
                LLVM::ExtractValueOp::create(builder_, loc, closure, ArrayRef<int64_t>{1});
            SmallVector<Type> parameters{ptr};
            SmallVector<Value> arguments{environment};
            for (std::size_t i = 0; i < operation.arguments.size(); ++i) {
              parameters.push_back(callableABIType(signature.parameters[i]));
              arguments.push_back(callablePack(loc, expression(frame, operation.arguments[i]),
                                               signature.parameters[i]));
            }
            auto signatureType =
                builder_.getFunctionType(parameters, {callableABIType(signature.result)});
            auto reference = UnrealizedConversionCastOp::create(
                                 builder_, loc, TypeRange{signatureType}, ValueRange{pointer})
                                 .getResult(0);
            auto result =
                func::CallIndirectOp::create(builder_, loc, reference, arguments).getResult(0);
            return callableUnpack(loc, result, node.type);
          } else if constexpr (std::is_same_v<T, FunctionCall>) {
            SmallVector<Value> arguments;
            for (std::size_t i = 0; i < operation.arguments.size(); ++i) {
              auto argument = expression(frame, operation.arguments[i]);
              arguments.push_back(
                  castTensor(loc, argument, functions_[operation.function].getArgumentTypes()[i]));
            }
            auto call =
                func::CallOp::create(builder_, loc, functions_[operation.function], arguments);
            return castTensor(loc, call.getResult(0), type(node.type));
          } else if constexpr (std::is_same_v<T, Matmul>) {
            auto a = expression(frame, operation.left), b = expression(frame, operation.right);
            return matmul(loc, a, b, cast<RankedTensorType>(type(node.type)), operation,
                          node.type.element);
          } else if constexpr (std::is_same_v<T, ReduceSum>) {
            if (operation.order == ReductionOrder::BalancedAdjacent && operation.rowMajor)
              if (auto fused = fusedMapReduction(frame, operation.input, loc))
                return fused;
            return reduce(loc, expression(frame, operation.input), operation, node.type.element);
          } else if constexpr (std::is_same_v<T, DeviceSelection> || std::is_same_v<T, Transfer> ||
                               std::is_same_v<T, TaskOperation> ||
                               std::is_same_v<T, ParallelOperation> ||
                               std::is_same_v<T, CatchOperation> ||
                               std::is_same_v<T, ErrorFieldRead> ||
                               std::is_same_v<T, FailOperation> ||
                               std::is_same_v<T, GuardOperation>) {
            throw LoweringFailure{"task operation requires continuation lowering"};
          } else if constexpr (std::is_same_v<T, ShapeCheck>) {
            auto input = expression(frame, operation.input);
            if (isOwnedArray(node.type)) {
              auto count = recordArrayLength(loc, input, node.type);
              for (const auto &requirement : operation.requirements)
                guard(
                    loc,
                    compare(loc, arith::CmpIPredicate::eq, count, integer(loc, requirement.extent)),
                    1);
              return input;
            }
            for (const auto &requirement : operation.requirements) {
              auto actual =
                  tensor::DimOp::create(builder_, loc, input, index(loc, requirement.axis));
              guard(loc,
                    compare(loc, arith::CmpIPredicate::eq, actual, index(loc, requirement.extent)),
                    1);
            }
            return castTensor(loc, input, type(node.type));
          }
        },
        node.operation);
    frame.values.emplace(id, value);
    return value;
  }
  void body(FunctionId id) {
    auto function = functions_[id];
    auto *entry = function.addEntryBlock();
    builder_.setInsertionPointToStart(entry);
    Frame frame;
    // Long float32 tensor expressions otherwise inline many allocation/shape/
    // loop bodies into one native frame. Outline operations into ordinary
    // checked, non-inlined helpers; every frame retains the same stack budget.
    frame.outlineElementwise =
        std::ranges::count_if(input_.functions[id].expressions, [&](ValueId value) {
          const auto &node = input_.expressions[value];
          return node.type.element == NumericType::Float32 &&
                 (std::holds_alternative<TensorBinary>(node.operation) ||
                  std::holds_alternative<TensorUnary>(node.operation));
        }) >= 16;
    for (std::size_t i = 0; i < input_.functions[id].parameters.size(); ++i) {
      const auto parameter = input_.functions[id].parameters[i];
      frame.symbols.emplace(parameter, entry->getArgument(i));
      validateStrings(function.getLoc(), entry->getArgument(i), input_.symbols[parameter].type);
    }
    const auto &source = input_.functions[id];
    const auto plainNumeric = [](const ValueType &t) {
      return !t.fallible && !t.task &&
             (t.builtin == BuiltinValue::Numeric || (t.builtin == BuiltinValue::Bool && !t.tensor));
    };
    // Keep the terminal branch before the loop condition and the recursive
    // branch after it. Each condition forwards the current arrays unchanged;
    // only a continuing iteration produces replacement buffers. Mixing a base
    // result and new allocations in one loop yield forces needless copies.
    const auto *choice = source.body.size() == 1
        ? std::get_if<ConditionalOperation>(&input_.expressions[source.body[0].value].operation)
        : nullptr;
    const auto *trueCall = choice
        ? std::get_if<FunctionCall>(&input_.expressions[choice->trueResult].operation) : nullptr;
    const auto *falseCall = choice
        ? std::get_if<FunctionCall>(&input_.expressions[choice->falseResult].operation) : nullptr;
    const bool trueTail = trueCall && trueCall->function == id &&
        !choice->whenTrue.empty() && choice->whenTrue.back().value == choice->trueResult;
    const bool falseTail = falseCall && falseCall->function == id &&
        !choice->whenFalse.empty() && choice->whenFalse.back().value == choice->falseResult;
    const bool arrayTail = arrayTailLoops_ && (trueTail != falseTail) &&
        !source.resultType.tensor && plainNumeric(source.resultType) &&
        std::ranges::all_of(source.parameters, [&](auto p) {
          const auto &t = input_.symbols[p].type;
          return plainNumeric(t) && (!t.tensor || t.shape.size() == 1);
        }) && std::ranges::any_of(source.parameters, [&](auto p) {
          return input_.symbols[p].type.tensor;
        });
    if (arrayTail) {
      // Preserve source immutability in function-boundary bufferization. A
      // writable default otherwise forces callers to clone even a base case.
      for (std::size_t n = 0; n < source.parameters.size(); ++n)
        if (input_.symbols[source.parameters[n]].type.tensor)
          function.setArgAttr(n, "bufferization.writable", builder_.getBoolAttr(false));
      const auto loc = function.getLoc();
      const auto &terminal = trueTail ? choice->whenFalse : choice->whenTrue;
      const auto terminalResult = trueTail ? choice->falseResult : choice->trueResult;
      const auto &continuation = trueTail ? choice->whenTrue : choice->whenFalse;
      const auto &call = *(trueTail ? trueCall : falseCall);
      auto resultType = function.getResultTypes()[0];
      SmallVector<Value> initial(entry->getArguments());
      SmallVector<Type> stateTypes(function.getArgumentTypes());
      stateTypes.push_back(resultType);
      auto iteration = [&](ValueRange args) {
        auto current = frame;
        current.values.clear();
        for (std::size_t n = 0; n < source.parameters.size(); ++n)
          current.symbols[source.parameters[n]] = args[n];
        return current;
      };
      auto emitStep = [&](ValueRange args) {
        auto current = iteration(args);
        for (std::size_t n = 0; n + 1 < continuation.size(); ++n) {
          const auto &statement = continuation[n];
          auto value = expression(current, statement.value);
          if (statement.binding) current.symbols[*statement.binding] = value;
        }
        SmallVector<Value> next;
        for (std::size_t n = 0; n < call.arguments.size(); ++n)
          next.push_back(castTensor(loc, expression(current, call.arguments[n]),
                                     function.getArgumentTypes()[n]));
        return next;
      };
      auto emitWhile = [&]() -> Value {
        Value zero = arith::ConstantOp::create(builder_, loc, builder_.getZeroAttr(resultType));
        auto loop = scf::WhileOp::create(builder_, loc, stateTypes, initial,
          [&](OpBuilder &, Location, ValueRange args) {
            auto current = iteration(args);
            Value active = expression(current, choice->condition);
            if (!trueTail)
              active = arith::XOrIOp::create(builder_, loc, active,
                  arith::ConstantIntOp::create(builder_, loc, 1, 1));
            auto completed = scf::IfOp::create(builder_, loc, TypeRange{resultType}, active, true);
            {
              OpBuilder::InsertionGuard guard(builder_);
              builder_.setInsertionPointToStart(&completed.getThenRegion().front());
              scf::YieldOp::create(builder_, loc, zero);
              builder_.setInsertionPointToStart(&completed.getElseRegion().front());
              for (const auto &statement : terminal) {
                auto value = expression(current, statement.value);
                if (statement.binding) current.symbols[*statement.binding] = value;
              }
              auto result = castTensor(loc, expression(current, terminalResult), resultType);
              scf::YieldOp::create(builder_, loc, result);
            }
            SmallVector<Value> next(args);
            next.push_back(completed.getResult(0));
            scf::ConditionOp::create(builder_, loc, active, next);
          },
          [&](OpBuilder &, Location, ValueRange args) {
            scf::YieldOp::create(builder_, loc, emitStep(args));
          });
        return loop.getResults().back();
      };
      const auto simpleOperand = [&](ValueId value) {
        const auto &node = input_.expressions[value];
        return !node.type.tensor && plainNumeric(node.type) && std::visit([](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          return std::is_same_v<T, SymbolRead> || std::is_same_v<T, Int32Literal> ||
                 std::is_same_v<T, Int64Literal> || std::is_same_v<T, UInt32Literal> ||
                 std::is_same_v<T, UInt64Literal> || std::is_same_v<T, Float32Literal> ||
                 std::is_same_v<T, Float64Literal> || std::is_same_v<T, BoolLiteral>;
        }, node.operation);
      };
      const auto *comparison =
          std::get_if<ScalarCompare>(&input_.expressions[choice->condition].operation);
      // A structural int64 countdown has a finite nonnegative iteration count.
      // SCF's for ownership interface carries replacement arrays without the
      // condition/body clones required by its general while interface. Negative
      // counts retain the original while, including eventual checked underflow.
      std::optional<std::size_t> countdown;
      if (comparison && ((comparison->operation == ComparisonKind::Equal && !trueTail) ||
                         (comparison->operation == ComparisonKind::NotEqual && trueTail))) {
        auto isInteger = [&](ValueId value, std::int64_t expected) {
          const auto *literal = std::get_if<Int64Literal>(&input_.expressions[value].operation);
          return literal && literal->value == expected;
        };
        auto candidate = isInteger(comparison->right, 0) ? comparison->left
            : isInteger(comparison->left, 0) ? comparison->right : choice->condition;
        const auto *read = std::get_if<SymbolRead>(&input_.expressions[candidate].operation);
        if (read && input_.symbols[read->symbol].type.element == NumericType::Int64)
          for (std::size_t n = 0; n < source.parameters.size(); ++n) {
            const auto *subtract =
                std::get_if<ScalarBinary>(&input_.expressions[call.arguments[n]].operation);
            const auto *from = subtract
                ? std::get_if<SymbolRead>(&input_.expressions[subtract->left].operation) : nullptr;
            if (source.parameters[n] == read->symbol && subtract && from &&
                subtract->operation == TokenKind::Minus && from->symbol == read->symbol &&
                isInteger(subtract->right, 1)) countdown = n;
          }
      }
      auto emitLoop = [&]() -> Value {
        if (!countdown) return emitWhile();
        auto zero = arith::ConstantIntOp::create(builder_, loc, 0, 64);
        auto one = arith::ConstantIntOp::create(builder_, loc, 1, 64);
        auto negative = arith::CmpIOp::create(builder_, loc, arith::CmpIPredicate::slt,
                                             initial[*countdown], zero);
        auto selected = scf::IfOp::create(builder_, loc, TypeRange{resultType}, negative, true);
        {
          OpBuilder::InsertionGuard guard(builder_);
          builder_.setInsertionPointToStart(&selected.getThenRegion().front());
          scf::YieldOp::create(builder_, loc, emitWhile());
          builder_.setInsertionPointToStart(&selected.getElseRegion().front());
          // The enclosing zero-case bypass and negative branch prove at least
          // one iteration. Peel it so freshly produced arrays enter the counted
          // loop as writable owned buffers, rather than cloning borrowed input.
          auto first = emitStep(initial);
          auto loop = scf::ForOp::create(builder_, loc, one, initial[*countdown], one, first,
              [&](OpBuilder &, Location, Value, ValueRange args) {
                scf::YieldOp::create(builder_, loc, emitStep(args));
              });
          auto completed = iteration(loop.getResults());
          for (const auto &statement : terminal) {
            auto value = expression(completed, statement.value);
            if (statement.binding) completed.symbols[*statement.binding] = value;
          }
          scf::YieldOp::create(builder_, loc,
              castTensor(loc, expression(completed, terminalResult), resultType));
        }
        return selected.getResult(0);
      };
      if (comparison && simpleOperand(comparison->left) && simpleOperand(comparison->right)) {
        // A parameter/literal comparison has no allocation or other source
        // effects. Keep its terminal case outside loop ownership conversion,
        // which otherwise clones an unused tensor result even at zero steps.
        Value active = expression(frame, choice->condition);
        if (!trueTail)
          active = arith::XOrIOp::create(builder_, loc, active,
              arith::ConstantIntOp::create(builder_, loc, 1, 1));
        auto selected = scf::IfOp::create(builder_, loc, TypeRange{resultType}, active, true);
        {
          OpBuilder::InsertionGuard guard(builder_);
          builder_.setInsertionPointToStart(&selected.getThenRegion().front());
          scf::YieldOp::create(builder_, loc, emitLoop());
          builder_.setInsertionPointToStart(&selected.getElseRegion().front());
          auto stopped = frame;
          for (const auto &statement : terminal) {
            auto value = expression(stopped, statement.value);
            if (statement.binding) stopped.symbols[*statement.binding] = value;
          }
          scf::YieldOp::create(builder_, loc,
              castTensor(loc, expression(stopped, terminalResult), resultType));
        }
        func::ReturnOp::create(builder_, loc, selected.getResult(0));
      } else
        func::ReturnOp::create(builder_, loc, emitLoop());
      return;
    }
    Value result;
    for (const auto &statement : input_.functions[id].body) {
      result = expression(frame, statement.value);
      if (statement.binding)
        frame.symbols.emplace(*statement.binding, result);
    }
    auto value = castTensor(function.getLoc(), result, function.getResultTypes()[0]);
    validateStrings(function.getLoc(), value, input_.functions[id].resultType);
    func::ReturnOp::create(builder_, function.getLoc(), value);
  }
  void interfaceWrapper(const std::string &name,
                        const std::string &symbol = "kalandra_test_entry") {
    auto found =
        std::ranges::find_if(input_.functions, [&](const auto &f) { return f.name == name; });
    if (found == input_.functions.end())
      throw LoweringFailure{"interface entry must name a function"};
    const auto id = static_cast<FunctionId>(found - input_.functions.begin());
    SmallVector<Type> arguments;
    for (auto parameter : found->parameters) {
      const auto &t = input_.symbols[parameter].type;
      if (!t.tensor || isOwnedArray(t))
        arguments.push_back(type(t));
      else
        arguments.push_back(
            MemRefType::get(SmallVector<std::int64_t>(t.shape.size(), ShapedType::kDynamic),
                            cast<RankedTensorType>(type(t)).getElementType()));
    }
    builder_.setInsertionPointToEnd(module_->getBody());
    auto function =
        func::FuncOp::create(builder_, location(found->declaration), symbol,
                             builder_.getFunctionType(arguments, functions_[id].getResultTypes()));
    function->setAttr("llvm.emit_c_interface", builder_.getUnitAttr());
    builder_.setInsertionPointToStart(function.addEntryBlock());
    SmallVector<Value> operands;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      Value argument = function.getArgument(i);
      const auto &t = input_.symbols[found->parameters[i]].type;
      if (t.tensor && !isOwnedArray(t)) {
        SmallVector<Value> shape;
        for (std::size_t axis = 0; axis < t.shape.size(); ++axis) {
          auto d = memref::DimOp::create(builder_, function.getLoc(), argument,
                                         index(function.getLoc(), axis));
          shape.push_back(d);
          if (t.shape[axis].constant)
            guard(function.getLoc(),
                  compare(function.getLoc(), arith::CmpIPredicate::eq, d,
                          index(function.getLoc(), *t.shape[axis].constant)),
                  1);
        }
        checkedCount(function.getLoc(), shape, describeNumeric(t.element)->bytes());
        auto tensorType =
            RankedTensorType::get(SmallVector<std::int64_t>(t.shape.size(), ShapedType::kDynamic),
                                  cast<RankedTensorType>(type(t)).getElementType());
        argument = bufferization::ToTensorOp::create(builder_, function.getLoc(), tensorType,
                                                     argument, true, false);
        argument = castTensor(function.getLoc(), argument, type(t));
      }
      operands.push_back(argument);
    }
    auto call = func::CallOp::create(builder_, function.getLoc(), functions_[id], operands);
    func::ReturnOp::create(builder_, function.getLoc(), call.getResults());
  }
  const TypedModule &input_;
  OpBuilder builder_;
  OwningOpRef<ModuleOp> module_;
  std::vector<func::FuncOp> functions_;
  std::unordered_map<FunctionId, func::FuncOp> callableEntries_;
  bool recursiveClosures_ = false;
  std::optional<TypedModule> ownershipInput_;
  std::vector<std::optional<std::size_t>> environmentRecords_;
  std::unique_ptr<RecursiveRecordOps> recursive_;
  RecordStoragePlan recordStorage_;
  std::vector<LLVM::LLVMStructType> recursiveTypes_;
  std::unordered_map<std::size_t, Type> recordTypes_;
  std::unordered_map<std::size_t, std::pair<std::uint64_t, std::uint64_t>> recordLayouts_;
  unsigned nextMatmulHelper_ = 0, nextStringLiteral_ = 0, nextElementwiseHelper_ = 0;
};
} // namespace
static std::expected<BackendOutput, std::string> lowerNativeImpl(const TypedModule &input,
                                                      std::optional<std::string> testEntry,
                                                      const std::vector<std::string> &exports,
                                                      ArithmeticMode arithmetic,
                                                      bool deferCallFinalization,
                                                      bool arrayTailLoops) {
  if (auto error = numericCapabilityError(input, NumericTarget::Cpu))
    return std::unexpected(*error);
  const auto storage = planRecordStorage(input);
  const bool recursive = storage && hasRecursiveStorage(*storage);
  if (recursive && !exports.empty())
    return std::unexpected("recursive programs do not yet support CPU bridge exports");
  if (hasPlacement(input))
    return std::unexpected("placement-aware IR requires a placement-capable execution backend");
  if (std::ranges::any_of(input.functions, [](const auto &f) { return f.resultType.fallible; }) ||
      std::ranges::any_of(input.expressions, [recursive](const auto &e) {
        return std::holds_alternative<TaskOperation>(e.operation) ||
               std::holds_alternative<ParallelOperation>(e.operation) ||
               std::holds_alternative<CatchOperation>(e.operation) ||
               (!recursive && std::holds_alternative<MatchOperation>(e.operation));
      })) {
    if (!exports.empty())
      return std::unexpected("CPU bridge exports require ordinary synchronous functions");
    return lowerTaskNative(input, testEntry, arithmetic);
  }
  DialectRegistry registry;
  registerAllDialects(registry);
  registerAllExtensions(registry);
  registerAllToLLVMIRTranslations(registry);
  MLIRContext context(registry);
  context.loadAllAvailableDialects();
  static std::once_flag passRegistration;
  std::call_once(passRegistration, [] { registerAllPasses(); });
  try {
    auto module = Lowerer(input, context, arithmetic, arrayTailLoops).run(testEntry, exports);
    if (failed(verify(*module)))
      return std::unexpected("generated tensor MLIR failed verification");
    BackendOutput result;
    {
      llvm::raw_string_ostream source(result.mlir);
      OpPrintingFlags flags;
      if (!input.source->parts().empty())
        flags.enableDebugInfo();
      module->print(source, flags);
    }
    // Hardware arithmetic is the default; strict integer IEEE arithmetic is opt-in.
    SmallVector<Operation *> doubles;
    module->walk([&](Operation *op) {
      if (isa<arith::AddFOp, arith::SubFOp, arith::MulFOp, arith::DivFOp, arith::NegFOp>(op) &&
          (op->getResult(0).getType().isF64() || op->getResult(0).getType().isF32()) &&
          arithmetic == ArithmeticMode::Software)
        doubles.push_back(op);
    });
    for (auto *op : doubles)
      if (failed(lowerOutlinedStrictCpuFloat(op)))
        return std::unexpected("strict CPU float64 lowering failed");
    PassManager pipeline(&context);
    pipeline.enableVerifier(true);
    const char *passes = "one-shot-bufferize{bufferize-function-boundaries "
                         "allow-return-allocs-from-loops},convert-linalg-to-"
                         "loops,buffer-deallocation-pipeline,convert-bufferization-to-memref";
    llvm::SmallPtrSet<Operation *, 32> originalFunctions;
    for (auto fn : module->getOps<func::FuncOp>()) originalFunctions.insert(fn);
    if (failed(parsePassPipeline(passes, pipeline)))
      return std::unexpected("cannot construct pinned lowering pipeline");
    if (failed(pipeline.run(*module)))
      return std::unexpected("native MLIR lowering failed");
    // Deallocation's alias-check helper creates small index/Bool workspaces.
    // They are compiler control storage, not source allocations. Keep bounded,
    // non-escaping workspaces in the native frame, reusable across iterations.
    llvm::SmallPtrSet<Operation *, 8> deallocationHelpers;
    for (auto fn : module->getOps<func::FuncOp>())
      if (arrayTailLoops && !originalFunctions.contains(fn) && fn.isPrivate() &&
          fn.getName().starts_with("dealloc_helper")) deallocationHelpers.insert(fn);
    SmallVector<memref::AllocOp> scratch;
    module->walk([&](memref::AllocOp op) {
      auto t = op.getType();
      if (arrayTailLoops && op.getSymbolOperands().empty() &&
          t.hasStaticShape() && t.getRank() == 1 && t.getNumElements() <= 32 &&
          (t.getElementType().isIndex() || t.getElementType().isInteger(1)))
        scratch.push_back(op);
    });
    for (auto allocation : scratch) {
      llvm::SmallPtrSet<Operation *, 16> seen;
      SmallVector<Value> pending{allocation.getResult()};
      SmallVector<memref::DeallocOp> releases;
      bool safe = true, usedByHelper = false;
      while (!pending.empty() && safe) {
        auto value = pending.pop_back_val();
        for (auto *user : value.getUsers()) {
          if (!seen.insert(user).second) continue;
          if (auto cast = dyn_cast<memref::CastOp>(user)) pending.push_back(cast.getResult());
          else if (auto release = dyn_cast<memref::DeallocOp>(user)) releases.push_back(release);
          else if (auto load = dyn_cast<memref::LoadOp>(user)) safe &= load.getMemRef() == value;
          else if (auto store = dyn_cast<memref::StoreOp>(user)) safe &= store.getMemRef() == value;
          else if (auto call = dyn_cast<func::CallOp>(user)) {
            auto target = module->lookupSymbol<func::FuncOp>(call.getCallee());
            safe &= target && deallocationHelpers.contains(target);
            usedByHelper = true;
          } else safe = false;
        }
      }
      if (!safe || !usedByHelper) continue;
      auto fn = allocation->getParentOfType<func::FuncOp>();
      if (!fn || fn.empty()) continue;
      OpBuilder entry(&fn.front(), fn.front().begin());
      auto local = memref::AllocaOp::create(entry, allocation.getLoc(), allocation.getType());
      allocation.replaceAllUsesWith(local.getResult());
      for (auto release : releases) release.erase();
      allocation.erase();
    }
    SmallVector<memref::CopyOp> copies;
    module->walk([&](memref::CopyOp op) { copies.push_back(op); });
    for (auto copy : copies) {
      OpBuilder builder(copy);
      auto loc = copy.getLoc();
      const auto rank = cast<MemRefType>(copy.getSource().getType()).getRank();
      SmallVector<Value> indices;
      std::function<void(std::int64_t)> emitCopy = [&](std::int64_t axis) {
        if (axis == rank) {
          auto value = memref::LoadOp::create(builder, loc, copy.getSource(), indices);
          memref::StoreOp::create(builder, loc, value, copy.getTarget(), indices);
          return;
        }
        auto zero = arith::ConstantIndexOp::create(builder, loc, 0);
        auto one = arith::ConstantIndexOp::create(builder, loc, 1);
        auto count = memref::DimOp::create(builder, loc, copy.getSource(), axis);
        scf::ForOp::create(builder, loc, zero, count, one, ValueRange{},
                           [&](OpBuilder &, Location, Value index, ValueRange) {
                             indices.push_back(index);
                             emitCopy(axis + 1);
                             indices.pop_back();
                             scf::YieldOp::create(builder, loc);
                           });
      };
      emitCopy(0);
      copy.erase();
    }
    PassManager llvmPipeline(&context);
    llvmPipeline.enableVerifier(true);
    if (failed(parsePassPipeline("convert-vector-to-llvm,convert-scf-to-cf,finalize-memref-to-llvm{"
                                 "use-generic-functions},"
                                 "convert-to-llvm,reconcile-unrealized-casts",
                                 llvmPipeline)) ||
        failed(llvmPipeline.run(*module)))
      return std::unexpected("LLVM dialect lowering failed");
    llvm::LLVMContext llvmContext;
    auto native = translateModuleToLLVMIR(*module, llvmContext);
    if (!native)
      return std::unexpected("LLVM translation failed");
    // LLVM's signless i8/i16 type does not describe the platform C return
    // extension. Packet loads naturally zero-extend, so preserve signedness
    // explicitly at checked/foreign entries instead of relying on arithmetic
    // accidentally leaving the upper register bits in the expected state.
    auto resultABI = [&](const std::string &symbol, const ValueType &value) {
      if (value.tensor) return;
      const bool boolean = value.builtin == BuiltinValue::Bool;
      if (!boolean && (value.builtin != BuiltinValue::Numeric ||
                       !isNarrowInteger(value.element))) return;
      const unsigned width = boolean ? 1 : describeNumeric(value.element)->bits;
      for (const auto &name : {symbol, "_mlir_ciface_" + symbol})
        if (auto *fn = native->getFunction(name);
            fn && fn->getReturnType()->isIntegerTy(width))
          fn->addRetAttr(boolean || isUnsigned(value.element) ? llvm::Attribute::ZExt :
                                                               llvm::Attribute::SExt);
    };
    for (FunctionId id = 0; id != input.functions.size(); ++id) {
      const auto &fn = input.functions[id];
      const auto value = fn.declaredResult.value_or(fn.resultType);
      resultABI("kalandra_fn_" + std::to_string(id), value);
      resultABI("kalandra_callable_" + std::to_string(id), value);
      if (testEntry && fn.name == *testEntry)
        resultABI("kalandra_test_entry", value);
      if (fn.name == "main") resultABI("kalandra_bool_entry", value);
      for (std::size_t n = 0; n != exports.size(); ++n)
        if (exports[n] == fn.name) resultABI("kalandra_cpu_" + std::to_string(n), value);
    }
    const bool functionValues = std::ranges::any_of(input.expressions, [](const auto &value) {
      return std::holds_alternative<FunctionReference>(value.operation) ||
             std::holds_alternative<FunctionInvoke>(value.operation);
    });
    if (auto error = addCheckedInvocationABI(*native, functionValues))
      return std::unexpected(*error);
    if (exports.empty())
      for (const auto &function : input.functions)
        if (function.name == "main" && function.resultType.tensor)
          collectionMetadata(*native, function.resultType);
    native->setTargetTriple(llvm::Triple(KALANDRA_NATIVE_CPU_TARGET_TRIPLE));
    if (!deferCallFinalization)
      if (auto error = optimizeNativeCalls(native)) return std::unexpected(*error);
    std::string errors;
    llvm::raw_string_ostream errorStream(errors);
    if (llvm::verifyModule(*native, &errorStream))
      return std::unexpected("generated LLVM IR failed verification: " + errors);
    {
      llvm::raw_string_ostream llvmOutput(result.llvm);
      native->print(llvmOutput, nullptr);
    }
    return result;
  } catch (const LoweringFailure &failure) {
    return std::unexpected(failure.message);
  }
}
std::expected<BackendOutput, std::string> lowerNative(const TypedModule &input,
                                                      std::optional<std::string> testEntry,
                                                      const std::vector<std::string> &exports,
                                                      ArithmeticMode arithmetic,
                                                      bool deferCallFinalization) {
  auto result = lowerNativeImpl(input, testEntry, exports, arithmetic, deferCallFinalization, true);
  if (!result && result.error().find("native stack budget") != std::string::npos)
    return lowerNativeImpl(input, testEntry, exports, arithmetic, deferCallFinalization, false);
  return result;
}
} // namespace kalandra
