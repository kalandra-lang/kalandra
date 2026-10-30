#include "CollectionMetadata.h"
#include "ErrorFamilies.h"
#include "NativeLayout.h"
#include "NativeStack.h"
#include "NativeCallFoundation.h"
#include "RecordLayout.h"
#include "RecursiveAdmission.h"
#include "TaskCallableOps.h"
#include "TaskEntries.h"
#include "TaskEnvironment.h"
#include "TaskRecordOps.h"
#include "TaskTraversal.h"
#include "TaskValueCopy.h"
#include "TaskValueTypes.h"
#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace kalandra {
namespace {
using namespace continuation;
class Emitter {
  using Type = llvm::Type;
  using Value = llvm::Value;
  using Function = llvm::Function;
  using Block = llvm::BasicBlock;
  ModulePlan &planner;
  llvm::Module &module;
  llvm::LLVMContext &context;
  llvm::IRBuilder<> builder;
  Type *pointer, *i32, *i64, *f32, *boolean, *voidType;
  llvm::StructType *failureType, *handleType, *packetType, *layoutType, *entryType;
  std::map<std::string, llvm::StructType *> values;
  std::optional<TaskValueTypes> taskTypes;
  std::map<std::string, llvm::GlobalVariable *> layouts;
  std::map<std::string, Function *> copyCallbacks;
  std::map<std::size_t, Function *> recordRetainers;
  std::vector<std::size_t> pendingRecordRetainers;
  std::vector<llvm::StructType *> frames, packets;
  std::vector<std::map<std::size_t, std::size_t>> observationFields, recursiveResultFields;
  std::vector<llvm::GlobalVariable *> entries;
  Function *destroy;
  std::vector<Function *> synchronousEntries;
  std::vector<ValueType> functionSignatures;
  std::vector<bool> coordinating;
  bool recursiveClosures = false;
  std::map<std::size_t, Function *> environmentBoxes;

public:
  Emitter(ModulePlan &planner, llvm::Module &module)
      : planner(planner), module(module), context(module.getContext()), builder(context),
        coordinating(coordinatingFunctions(planner.input)) {
    module.setDataLayout(nativeCpuDataLayout);
    pointer = builder.getPtrTy();
    i32 = builder.getInt32Ty();
    i64 = builder.getInt64Ty();
    f32 = builder.getFloatTy();
    boolean = builder.getInt1Ty();
    voidType = builder.getVoidTy();
    failureType = llvm::StructType::get(context, {i32, i64, i64, i64});
    handleType = llvm::StructType::get(context, {i32, i64});
    packetType = llvm::StructType::get(context, {i64, pointer, i64, pointer});
    layoutType = llvm::StructType::get(context, {i64, i64, i64, pointer, pointer});
    entryType = llvm::StructType::get(
        context, {i32, i64, i64, i64, i64, i64, pointer, pointer, pointer, pointer});
    auto types = TaskValueTypes::create(planner.input, context);
    if (!types)
      throw std::runtime_error(types.error().message);
    taskTypes = std::move(*types);
    recursiveClosures = hasRecursiveStorage(*planRecordStorage(planner.input));
    destroy = function("kalandra_frame_destroy", voidType, {pointer});
    builder.SetInsertPoint(Block::Create(context, "entry", destroy));
    builder.CreateRetVoid();
    for (std::size_t id = 0; id < planner.plans.size(); ++id) {
      const auto &plan = *planner.plans[id];
      std::vector<Type *> fields{i32}, parameters;
      for (const auto &slot : plan.slots)
        fields.push_back(valueType(slot));
      for (std::size_t g = 0; g < plan.groups; ++g)
        fields.push_back(handleType);
      // Widening observes a child using its own layout. Keep that temporary
      // in the budgeted frame rather than growing the native worker stack.
      observationFields.emplace_back();
      for (const auto &instruction : plan.code)
        if (instruction.code == Code::Await &&
            !observationFields.back().contains(instruction.target)) {
          const auto &child = *planner.plans[instruction.target];
          if (key(child.slots[child.result]) != key(plan.slots[instruction.output])) {
            observationFields.back()[instruction.target] = fields.size();
            fields.push_back(valueType(child.slots[child.result]));
          }
        }
      recursiveResultFields.emplace_back();
      for (std::size_t n = 0; n < plan.code.size(); ++n)
        if (plan.code[n].code == Code::SynchronousCall) {
          const auto &target = planner.input.functions.at(plan.code[n].target);
          if (target.resultType.fallible) {
            recursiveResultFields.back()[n] = fields.size();
            fields.push_back(valueType(target.resultType));
          }
        }
      for (auto slot : plan.parameters)
        parameters.push_back(valueType(plan.slots[slot]));
      frames.push_back(
          llvm::StructType::create(context, fields, "task.frame." + std::to_string(id)));
      packets.push_back(llvm::StructType::get(context, parameters));
      entries.push_back(new llvm::GlobalVariable(module, entryType, true,
                                                 llvm::GlobalValue::InternalLinkage, nullptr,
                                                 "task.entry." + std::to_string(id)));
    }
  }
  void emitEntries() {
    synchronousEntries.resize(planner.plans.size());
    for (const auto &plan : planner.plans)
      for (const auto &instruction : plan->code)
        if ((instruction.code == Code::FunctionReference ||
             instruction.code == Code::SynchronousCall) &&
            !synchronousEntries[instruction.target]) {
          const auto id = instruction.target;
          if (instruction.code == Code::FunctionReference && coordinating.at(id))
            continue;
          const auto &source = planner.input.functions.at(id);
          if (planner.plans[id]->groups ||
              std::ranges::any_of(planner.plans[id]->code, [](const auto &op) {
                return op.code == Code::Open || op.code == Code::Spawn || op.code == Code::Await ||
                       op.code == Code::Join || op.code == Code::Device ||
                       op.code == Code::Transfer;
              }))
            throw std::runtime_error("synchronous callable plan contains task/device coordination");
          const auto result = source.declaredResult.value_or(source.resultType);
          std::vector<Type *> parameters{pointer}; // Immutable capture environment.
          if (result.fallible)
            parameters.push_back(pointer);
          for (std::size_t i = 0; i < source.parameters.size() - source.captureCount; ++i)
            parameters.push_back(scalarType(planner.input.symbols[source.parameters[i]].type));
          parameters.push_back(pointer);
          synchronousEntries[id] =
              function("kalandra_callable_plan_" + std::to_string(id),
                       result.fallible ? voidType : scalarType(result), parameters);
        }
    if (recursiveClosures) {
      std::set<std::size_t> targets;
      for (const auto &plan : planner.plans)
        for (const auto &op : plan->code)
          if (op.code == Code::FunctionReference) {
            const auto &target = planner.input.functions.at(op.target);
            if (op.args.size() != target.captureCount)
              throw std::runtime_error("task closure capture count mismatch");
            for (std::size_t i = 0; i < op.args.size(); ++i)
              if (!sameFunctionValueType(plan->slots.at(op.args[i]),
                                         planner.input.symbols
                                             .at(target.parameters.at(target.parameters.size() -
                                                                      target.captureCount + i))
                                             .type))
                throw std::runtime_error("task closure capture type mismatch");
            if (!op.args.empty())
              targets.insert(op.target);
          }
      const std::vector<std::size_t> ids(targets.begin(), targets.end());
      environmentBoxes = emitTaskEnvironmentBoxes(planner.input, ids, module);
    }
    // All synchronous entry declarations exist before emitting bodies, so
    // self/mutual references and recursive direct calls need no expansion.
    for (std::size_t id = 0; id < planner.plans.size(); ++id)
      emitPlan(id);
    for (std::size_t id = 0; id < synchronousEntries.size(); ++id)
      if (synchronousEntries[id]) {
        emitPlan(id, true);
        if (auto error = nativeStackBudgetError(*synchronousEntries[id], module.getDataLayout()))
          throw std::runtime_error(*error);
      }
    finishRecordRetainers();
  }
  std::vector<llvm::GlobalVariable *> taskEntries() const { return entries; }
  void run(const std::optional<std::string> &testEntry) {
    emitEntries();
    if (testEntry) {
      const auto found = std::ranges::find_if(planner.input.functions,
                                              [&](const auto &f) { return f.name == *testEntry; });
      const bool callableProbe = found != planner.input.functions.end() && !found->captureCount &&
                                 synchronousEntries[found - planner.input.functions.begin()];
      if (found == planner.input.functions.end() ||
          (!callableProbe && !found->parameters.empty() &&
           found->resultType.builtin != BuiltinValue::Record && !isBoolArray(found->resultType) &&
           found->resultType.builtin != BuiltinValue::String) ||
          (found->resultType.builtin != BuiltinValue::Numeric &&
           found->resultType.builtin != BuiltinValue::Bool &&
           found->resultType.builtin != BuiltinValue::Record &&
           found->resultType.builtin != BuiltinValue::String))
        throw std::runtime_error("private task test entry requires a parameterless numeric/Bool "
                                 "function or a record/String function with typed parameters");
      const auto id = std::size_t(found - planner.input.functions.begin());
      // Trusted native test export, not a dynamic/public task or wire ABI.
      new llvm::GlobalVariable(module, pointer, true, llvm::GlobalValue::ExternalLinkage,
                               entries[id], "kalandra_task_test_entry");
      if (synchronousEntries[id] && !found->captureCount) {
        auto *entry = synchronousEntries[id];
        std::vector<Type *> parameters(entry->getFunctionType()->param_begin() + 1,
                                       entry->getFunctionType()->param_end());
        auto *probe = function("kalandra_callable_probe", entry->getReturnType(), parameters);
        builder.SetInsertPoint(Block::Create(context, "entry", probe));
        std::vector<Value *> args{llvm::ConstantPointerNull::get(builder.getPtrTy())};
        for (auto &arg : probe->args())
          args.push_back(&arg);
        auto *value = builder.CreateCall(entry, args);
        if (entry->getReturnType()->isVoidTy())
          builder.CreateRetVoid();
        else
          builder.CreateRet(value);
        new llvm::GlobalVariable(module, pointer, true, llvm::GlobalValue::ExternalLinkage, probe,
                                 "kalandra_callable_test_entry");
      }
    }
    auto main = std::ranges::find_if(planner.input.functions,
                                     [](const auto &f) { return f.name == "main"; });
    if (main == planner.input.functions.end() || !main->parameters.empty() ||
        (main->resultType.builtin != BuiltinValue::Numeric &&
         main->resultType.builtin != BuiltinValue::Bool &&
         main->resultType.builtin != BuiltinValue::String))
      throw std::runtime_error(
          "native task entry requires parameterless float32, int32, uint32, "
          "int64, uint64, float64, Bool, String or CPU numeric array/tensor main");
    const auto id = std::size_t(main - planner.input.functions.begin());
    auto *entry = module.getFunction("_mlir_ciface_kalandra_entry");
    const bool stringResult =
        main->resultType.builtin == BuiltinValue::String && !main->resultType.tensor;
    const bool indirectResult = main->resultType.tensor || stringResult;
    if (indirectResult) {
      if (!stringResult)
        collectionMetadata(module, main->resultType);
      entry->eraseFromParent();
      entry = Function::Create(llvm::FunctionType::get(voidType, {pointer, pointer}, false),
                               llvm::GlobalValue::ExternalLinkage,
                               stringResult ? "_mlir_ciface_kalandra_string_entry"
                                            : "_mlir_ciface_kalandra_collection_entry",
                               module);
    } else if (main->resultType.builtin == BuiltinValue::Bool ||
               isInteger(main->resultType.element) ||
               main->resultType.element == NumericType::Float64) {
      entry->eraseFromParent();
      entry = Function::Create(
          llvm::FunctionType::get(
              isNarrowInteger(main->resultType.element) ? i64 : scalarType(main->resultType),
              {pointer}, false),
          llvm::GlobalValue::ExternalLinkage,
          main->resultType.builtin == BuiltinValue::Bool     ? "_mlir_ciface_kalandra_bool_entry"
          : main->resultType.element == NumericType::Float64 ? "_mlir_ciface_kalandra_float64_entry"
          : (main->resultType.element == NumericType::UInt64 ||
             main->resultType.element == NumericType::UInt8 ||
             main->resultType.element == NumericType::UInt16)
              ? "_mlir_ciface_kalandra_uint64_entry"
          : (main->resultType.element == NumericType::Int64 ||
             main->resultType.element == NumericType::Int8 ||
             main->resultType.element == NumericType::Int16)
              ? "_mlir_ciface_kalandra_int64_entry"
          : main->resultType.element == NumericType::UInt32 ? "_mlir_ciface_kalandra_uint32_entry"
                                                            : "_mlir_ciface_kalandra_int32_entry",
          module);
    } else
      entry->deleteBody();
    builder.SetInsertPoint(Block::Create(context, "entry", entry));
    auto *ctx = entry->getArg(indirectResult ? 1 : 0);
    const auto returnFailure = [&] {
      if (indirectResult)
        builder.CreateRetVoid();
      else
        builder.CreateRet(llvm::Constant::getNullValue(entry->getReturnType()));
    };
    if (stringResult)
      builder.CreateStore(llvm::Constant::getNullValue(scalarType(main->resultType)),
                          entry->getArg(0));
    else if (main->resultType.tensor)
      builder.CreateMemSet(entry->getArg(0), builder.getInt8(0), size(descriptor(main->resultType)),
                           llvm::MaybeAlign(alignment(descriptor(main->resultType))));
    auto *packet = builder.CreateAlloca(packetType);
    auto *result = builder.CreateAlloca(valueType(main->resultType));
    auto *failure = builder.CreateAlloca(failureType);
    builder.CreateStore(llvm::Constant::getNullValue(valueType(main->resultType)), result);
    fillPacket(packet, id, llvm::ConstantPointerNull::get(builder.getPtrTy()), ctx);
    call("kalandra_run_task_v1", voidType, {entries[id], packet, result, ctx, failure});
    auto *bad = Block::Create(context, "root.fault", entry);
    auto *ok = Block::Create(context, "root.result", entry);
    builder.CreateCondBr(hasFailure(failure), bad, ok);
    builder.SetInsertPoint(bad);
    auto *checkedFault = Block::Create(context, "root.checked_fault", entry);
    auto *executionError = Block::Create(context, "root.execution_error", entry);
    auto *rootKind = builder.CreateLoad(i32, builder.CreateStructGEP(failureType, failure, 0));
    builder.CreateCondBr(builder.CreateICmpEQ(rootKind, c32(1)), checkedFault, executionError);
    builder.SetInsertPoint(executionError);
    call("kalandra_expected_task_error_v1", voidType,
         {ctx, builder.CreateZExt(rootKind, i64), loadFailure(failure, 1), loadFailure(failure, 2),
          loadFailure(failure, 3)});
    returnFailure();
    builder.SetInsertPoint(checkedFault);
    Value *code = builder.CreateLoad(i64, builder.CreateStructGEP(failureType, failure, 1));
    code = builder.CreateSelect(builder.CreateICmpEQ(code, c64(0)), c64(3), code);
    call("kalandra_fault_v1", voidType,
         {ctx, code, loadFailure(failure, 2), loadFailure(failure, 3)});
    returnFailure();
    builder.SetInsertPoint(ok);
    auto *expected = Block::Create(context, "root.expected_error", entry);
    auto *success = Block::Create(context, "root.success", entry);
    auto *header = builder.CreateStructGEP(valueType(main->resultType), result, 0);
    builder.CreateCondBr(hasFailure(header), expected, success);
    builder.SetInsertPoint(expected);
    auto *kind = builder.CreateZExt(
        builder.CreateLoad(i32, builder.CreateStructGEP(failureType, header, 0)), i64);
    call("kalandra_expected_task_error_v1", voidType,
         {ctx, kind, loadFailure(header, 1), loadFailure(header, 2), loadFailure(header, 3)});
    returnFailure();
    builder.SetInsertPoint(success);
    if (main->resultType.errorRecord) {
      const auto family = *main->resultType.errorRecord;
      const auto &record = planner.input.records[family];
      auto *data = application(main->resultType, result);
      auto *tag = builder.CreateLoad(
          i64, builder.CreateStructGEP(scalarType(errorRecordType(family)), data, 0));
      auto *valid = Block::Create(context, "root.application.success", entry);
      auto *invalid = Block::Create(context, "root.application.invalid", entry);
      auto *dispatch = builder.CreateSwitch(tag, invalid, record.variants.size() + 1);
      dispatch->addCase(c64(0), valid);
      for (std::size_t i = 0; i < record.variants.size(); ++i) {
        auto *report = Block::Create(context, "root.application.error", entry);
        dispatch->addCase(c64(i + 1), report);
        builder.SetInsertPoint(report);
        const auto [original, variant] = errorOrigin(planner.input, family, i);
        const auto &source = planner.input.records[original];
        if (source.taskErrorFamily) {
          const auto slot = record.variants[i].fields.front();
          auto *failure = builder.CreateStructGEP(scalarType(errorRecordType(family)), data, slot);
          call("kalandra_expected_task_error_v1", voidType,
               {ctx,
                builder.CreateZExt(
                    builder.CreateLoad(i32, builder.CreateStructGEP(failureType, failure, 0)), i64),
                loadFailure(failure, 1), loadFailure(failure, 2), loadFailure(failure, 3)});
        } else
          call("kalandra_expected_application_error_v1", voidType,
               {ctx, builder.CreateGlobalString(source.name),
                builder.CreateGlobalString(source.variants[variant].name)});
        returnFailure();
      }
      builder.SetInsertPoint(invalid);
      call("kalandra_fault_v1", voidType, {ctx, c64(3), c64(0), c64(0)});
      returnFailure();
      builder.SetInsertPoint(valid);
    }
    auto *payload = builder.CreateStructGEP(valueType(main->resultType), result, 1);
    if (indirectResult) {
      copy(entry->getArg(0), payload, scalarType(main->resultType));
      builder.CreateRetVoid();
    } else {
      Value *value = builder.CreateLoad(scalarType(main->resultType), payload);
      if (isNarrowInteger(main->resultType.element))
        value = isUnsigned(main->resultType.element) ? builder.CreateZExt(value, i64)
                                                     : builder.CreateSExt(value, i64);
      if (hasRecordBacking(planner.input) ||
          std::ranges::any_of(planner.input.expressions,
                              [](const auto &e) { return e.type.builtin == BuiltinValue::String; }))
        call("kalandra_record_cleanup_v1", voidType, {ctx});
      builder.CreateRet(value);
    }
  }

  llvm::GlobalVariable *emitValueLayout(const ValueType &type) {
    auto *result = layout(type);
    finishRecordRetainers();
    return result;
  }

private:
  Function *recordRetainer(std::size_t id) {
    if (const auto found = recordRetainers.find(id); found != recordRetainers.end())
      return found->second;
    auto *result = function("task.record.retain." + std::to_string(id), boolean,
                            {pointer, pointer, pointer, pointer});
    recordRetainers.emplace(id, result);
    pendingRecordRetainers.push_back(id);
    return result;
  }
  void finishRecordRetainers() {
    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    // Declare on first use, then emit through a growing worklist. Array-mediated
    // cycles become runtime calls rather than recursive compiler expansion.
    for (std::size_t next = 0; next < pendingRecordRetainers.size(); ++next) {
      const auto id = pendingRecordRetainers[next];
      auto *fn = recordRetainers.at(id);
      if (!fn->empty())
        continue;
      builder.SetInsertPoint(Block::Create(context, "entry", fn));
      auto *start = Block::Create(context, "retain.start", fn);
      auto *denied = Block::Create(context, "retain.denied", fn);
      auto *failed = Block::Create(context, "retain.failed", fn);
      auto *entered = call("kalandra_call_enter_v1", boolean, {fn->getArg(0), c64(0), c64(0)});
      builder.CreateCondBr(entered, start, denied);
      builder.SetInsertPoint(denied);
      builder.CreateRet(builder.getFalse());
      builder.SetInsertPoint(start);
      auto *ready = call("kalandra_invocation_ready_v1", boolean, {fn->getArg(0)});
      auto *active = Block::Create(context, "retain.active", fn);
      builder.CreateCondBr(ready, active, failed);
      builder.SetInsertPoint(active);
      TaskTraversal traversal(builder, *fn, fn->getArg(3), failed);
      ValueType type;
      type.builtin = BuiltinValue::Record;
      type.record = id;
      retainRecord(type, fn->getArg(3), fn, failed);
      call("kalandra_call_leave_v1", voidType, {fn->getArg(0)});
      builder.CreateRet(builder.getTrue());
      builder.SetInsertPoint(failed);
      call("kalandra_call_leave_v1", voidType, {fn->getArg(0)});
      builder.CreateRet(builder.getFalse());
      traversal.finish();
      if (auto error = nativeStackBudgetError(*fn, module.getDataLayout()))
        throw std::runtime_error(*error);
    }
  }
  llvm::ConstantInt *c64(std::uint64_t n) { return builder.getInt64(n); }
  llvm::ConstantInt *c32(std::uint32_t n) { return builder.getInt32(n); }
  Function *function(const std::string &name, Type *result, const std::vector<Type *> &args) {
    return Function::Create(llvm::FunctionType::get(result, args, false),
                            llvm::GlobalValue::InternalLinkage, name, module);
  }
  Value *call(const char *name, Type *result, std::initializer_list<Value *> args) {
    std::vector<Type *> types;
    for (auto *arg : args)
      types.push_back(arg->getType());
    return builder.CreateCall(
        module.getOrInsertFunction(name, llvm::FunctionType::get(result, types, false)), args);
  }
  std::uint64_t size(Type *type) const { return module.getDataLayout().getTypeAllocSize(type); }
  std::uint64_t alignment(Type *type) const {
    return module.getDataLayout().getABITypeAlign(type).value();
  }
  llvm::StructType *descriptor(const ValueType &type) {
    auto *dims = llvm::ArrayType::get(i64, type.shape.size());
    return llvm::StructType::get(context, {pointer, pointer, i64, dims, dims});
  }
  Type *scalarType(const ValueType &type) { return taskTypes->scalar(type); }
  std::size_t functionSignature(ValueType type) {
    type.fallible = false;
    type.errorRecord.reset();
    for (std::size_t i = 0; i < functionSignatures.size(); ++i)
      if (sameFunctionValueType(type, functionSignatures[i]))
        return i;
    functionSignatures.push_back(std::move(type));
    return functionSignatures.size() - 1;
  }
  std::string payloadKey(const ValueType &type) {
    if (type.task)
      return "handle";
    if (type.builtin == BuiltinValue::Function)
      return type.array
                 ? "array.Function." + std::to_string(functionSignature(recordArrayElement(type)))
                 : "function.reference." + std::to_string(functionSignature(type));
    if (isStringArray(type) && !type.task)
      return "array.String";
    if (isBoolArray(type) && !type.task)
      return "array.Bool";
    if (isRecordArray(type) && !type.task)
      return "record.array." + std::to_string(*type.record);
    if (type.builtin == BuiltinValue::Record && !type.task)
      return "record." + std::to_string(*type.record);
    if (type.array && !type.task)
      return "array." + std::string(describeNumeric(type.element)->name);
    if (type.builtin == BuiltinValue::Numeric && !type.task && type.tensor &&
        (isInteger(type.element) || type.element == NumericType::Float64))
      return "tensor." + std::string(describeNumeric(type.element)->name) + "." +
             std::to_string(type.shape.size());
    return type.builtin == BuiltinValue::TaskError   ? "TaskError"
           : type.builtin == BuiltinValue::FaultKind ? "FaultKind"
           : type.builtin == BuiltinValue::String    ? "String"
           : type.task                               ? "handle"
           : type.builtin == BuiltinValue::Bool      ? "Bool"
           : type.tensor                             ? "tensor" + std::to_string(type.shape.size())
           : type.element == NumericType::Float64    ? "scalar.float64"
           : type.element == NumericType::UInt64     ? "scalar.uint64"
           : type.element == NumericType::Int64      ? "scalar.int64"
           : type.element == NumericType::UInt32     ? "scalar.uint32"
           : type.element == NumericType::Int32      ? "scalar.int32"
           : type.element == NumericType::Int16      ? "scalar.int16"
           : type.element == NumericType::UInt16     ? "scalar.uint16"
           : type.element == NumericType::Int8       ? "scalar.int8"
           : type.element == NumericType::UInt8      ? "scalar.uint8"
                                                     : "scalar";
  }

  std::string key(const ValueType &type) {
    return payloadKey(type) +
           (type.errorRecord ? ".error." + std::to_string(*type.errorRecord) : "");
  }
  ValueType errorRecordType(std::size_t id) {
    ValueType type;
    type.builtin = BuiltinValue::Record;
    type.record = id;
    return type;
  }
  llvm::StructType *valueType(const ValueType &type) {
    const auto name = key(type);
    if (auto found = values.find(name); found != values.end())
      return found->second;
    return values[name] = taskTypes->value(type, "result." + name);
  }

  void copy(Value *output, Value *input, Type *type) {
    builder.CreateMemCpy(output, llvm::MaybeAlign(alignment(type)), input,
                         llvm::MaybeAlign(alignment(type)), size(type));
  }
  Value *application(const ValueType &type, Value *slot) {
    return builder.CreateStructGEP(valueType(type), slot, 2);
  }
  Value *hasResultError(const ValueType &type, Value *slot) {
    auto *failure = hasFailure(builder.CreateStructGEP(valueType(type), slot, 0));
    if (!type.errorRecord)
      return failure;
    auto *record = application(type, slot);
    auto *tag = builder.CreateLoad(
        i64, builder.CreateStructGEP(scalarType(errorRecordType(*type.errorRecord)), record, 0));
    return builder.CreateOr(failure, builder.CreateICmpNE(tag, c64(0)));
  }
  void copyErrorValue(std::size_t target, Value *output, std::size_t source, Value *input) {
    auto *outType = llvm::cast<llvm::StructType>(scalarType(errorRecordType(target)));
    auto *inType = llvm::cast<llvm::StructType>(scalarType(errorRecordType(source)));
    if (source == target) {
      copy(output, input, outType);
      return;
    }
    if (!errorSubset(planner.input, source, target))
      throw std::runtime_error("error conversion loses a declared family");
    const auto &from = planner.input.records[source], &to = planner.input.records[target];
    builder.CreateStore(llvm::Constant::getNullValue(outType), output);
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *done = Block::Create(context, "error.converted", fn);
    auto *invalid = Block::Create(context, "error.invalid", fn);
    auto *tag = builder.CreateLoad(i64, builder.CreateStructGEP(inType, input, 0));
    auto *dispatch = builder.CreateSwitch(tag, invalid, from.variants.size() + 1);
    dispatch->addCase(c64(0), done);
    for (std::size_t i = 0; i < from.variants.size(); ++i) {
      auto origin = errorOrigin(planner.input, source, i);
      std::size_t j = 0;
      while (j < to.variants.size() && errorOrigin(planner.input, target, j) != origin)
        ++j;
      if (j == to.variants.size())
        throw std::runtime_error("missing error-union variant");
      auto *active = Block::Create(context, "error.inject", fn);
      dispatch->addCase(c64(i + 1), active);
      builder.SetInsertPoint(active);
      builder.CreateStore(c64(j + 1), builder.CreateStructGEP(outType, output, 0));
      for (std::size_t field = 0; field < from.variants[i].fields.size(); ++field) {
        const auto a = from.variants[i].fields[field], b = to.variants[j].fields[field];
        if (outType->getElementType(b) != inType->getElementType(a))
          throw std::runtime_error("error forwarding changed physical field storage");
        copy(builder.CreateStructGEP(outType, output, b), builder.CreateStructGEP(inType, input, a),
             outType->getElementType(b));
      }
      builder.CreateBr(done);
    }
    builder.SetInsertPoint(invalid);
    builder.CreateStore(c64(UINT64_MAX), builder.CreateStructGEP(outType, output, 0));
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
  }
  void narrowErrorValue(const ValueType &target, Value *output, const ValueType &source,
                        Value *input, Block *failed) {
    if (source.builtin != BuiltinValue::Record || !source.record || source.tensor || source.task ||
        source.fallible || source.errorRecord || source.device ||
        !planner.input.records.at(*source.record).error || target.tensor || target.task ||
        target.fallible || target.errorRecord || target.device ||
        (target.builtin != BuiltinValue::TaskError &&
         (target.builtin != BuiltinValue::Record || !target.record ||
          !planner.input.records.at(*target.record).error)) ||
        !errorSubset(planner.input, target.record, source.record))
      throw std::runtime_error("task error narrowing requires a checked completed family subset");
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> targetVariants;
    if (target.record)
      for (std::size_t j = 0; j < planner.input.records[*target.record].variants.size(); ++j)
        targetVariants.emplace(errorOrigin(planner.input, *target.record, j), j);
    const auto &from = planner.input.records[*source.record];
    auto *fromType = llvm::cast<llvm::StructType>(scalarType(source));
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *done = Block::Create(context, "error.narrowed", fn);
    auto *tag = builder.CreateLoad(i64, builder.CreateStructGEP(fromType, input, 0));
    auto *dispatch = builder.CreateSwitch(tag, failed, from.variants.size());
    for (std::size_t i = 0; i < from.variants.size(); ++i) {
      const auto origin = errorOrigin(planner.input, *source.record, i);
      std::optional<std::size_t> selected;
      if (target.builtin == BuiltinValue::TaskError) {
        if (planner.input.records[origin.first].taskErrorFamily)
          selected = origin.second;
      } else if (auto found = targetVariants.find(origin); found != targetVariants.end())
        selected = found->second;
      if (!selected)
        continue;
      auto *active = Block::Create(context, "error.narrow.active", fn);
      dispatch->addCase(c64(i + 1), active);
      builder.SetInsertPoint(active);
      if (target.builtin == BuiltinValue::TaskError) {
        auto *payload = builder.CreateStructGEP(fromType, input, from.variants[i].fields.front());
        auto *valid = Block::Create(context, "error.narrow.task", fn);
        auto *kind = builder.CreateLoad(i32, builder.CreateStructGEP(failureType, payload, 0));
        builder.CreateCondBr(builder.CreateICmpEQ(kind, c32(*selected + 1)), valid, failed);
        builder.SetInsertPoint(valid);
        copy(output, payload, failureType);
      } else {
        auto *toType = llvm::cast<llvm::StructType>(scalarType(target));
        const auto &to = planner.input.records[*target.record];
        builder.CreateStore(llvm::Constant::getNullValue(toType), output);
        builder.CreateStore(c64(*selected + 1), builder.CreateStructGEP(toType, output, 0));
        const auto &fields = to.variants[*selected].fields;
        if (fields.size() != from.variants[i].fields.size())
          throw std::runtime_error("task error narrowing payload metadata mismatch");
        for (std::size_t n = 0; n < fields.size(); ++n) {
          const auto a = from.variants[i].fields[n], b = fields[n];
          if (toType->getElementType(b) != fromType->getElementType(a))
            throw std::runtime_error("error narrowing changed physical field storage");
          copy(builder.CreateStructGEP(toType, output, b),
               builder.CreateStructGEP(fromType, input, a), toType->getElementType(b));
        }
      }
      builder.CreateBr(done);
    }
    builder.SetInsertPoint(done);
  }
  void injectTaskError(std::size_t target, Value *output, Value *failure) {
    if (!errorSubset(planner.input, {}, target))
      throw std::runtime_error("destination does not cover TaskError");
    const auto &record = planner.input.records[target];
    auto *outType = scalarType(errorRecordType(target));
    builder.CreateStore(llvm::Constant::getNullValue(outType), output);
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *done = Block::Create(context, "task.error.injected", fn);
    auto *invalid = Block::Create(context, "task.error.invalid", fn);
    auto *kind = builder.CreateLoad(i32, builder.CreateStructGEP(failureType, failure, 0));
    auto *dispatch = builder.CreateSwitch(kind, invalid, 4);
    for (std::size_t i = 0; i < record.variants.size(); ++i) {
      const auto [original, variant] = errorOrigin(planner.input, target, i);
      if (!planner.input.records[original].taskErrorFamily)
        continue;
      auto *active = Block::Create(context, "task.error.payload", fn);
      dispatch->addCase(c32(variant + 1), active);
      builder.SetInsertPoint(active);
      builder.CreateStore(c64(i + 1), builder.CreateStructGEP(outType, output, 0));
      copy(builder.CreateStructGEP(outType, output, record.variants[i].fields.front()), failure,
           failureType);
      builder.CreateBr(done);
    }
    builder.SetInsertPoint(invalid);
    builder.CreateStore(c64(UINT64_MAX), builder.CreateStructGEP(outType, output, 0));
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
  }
  void copyErrorPayload(std::size_t target, Value *output, const ValueType &source, Value *input) {
    if (source.errorRecord)
      copyErrorValue(target, output, *source.errorRecord, application(source, input));
    else
      builder.CreateStore(llvm::Constant::getNullValue(scalarType(errorRecordType(target))),
                          output);
    if (errorSubset(planner.input, {}, target)) {
      auto *fn = builder.GetInsertBlock()->getParent();
      auto *active = Block::Create(context, "task.error.normalize", fn);
      auto *done = Block::Create(context, "task.error.normalized", fn);
      auto *header = builder.CreateStructGEP(valueType(source), input, 0);
      builder.CreateCondBr(hasFailure(header), active, done);
      builder.SetInsertPoint(active);
      injectTaskError(target, output, header);
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
    }
  }
  void copyErrors(const ValueType &outType, Value *output, const ValueType &inType, Value *input) {
    copy(builder.CreateStructGEP(valueType(outType), output, 0),
         builder.CreateStructGEP(valueType(inType), input, 0), failureType);
    if (outType.errorRecord) {
      copyErrorPayload(*outType.errorRecord, application(outType, output), inType, input);
      if (errorSubset(planner.input, {}, outType.errorRecord))
        builder.CreateStore(llvm::Constant::getNullValue(failureType),
                            builder.CreateStructGEP(valueType(outType), output, 0));
    }
  }
  void copyResult(const ValueType &outType, Value *output, const ValueType &inType, Value *input) {
    copyErrors(outType, output, inType, input);
    copy(builder.CreateStructGEP(valueType(outType), output, 1),
         builder.CreateStructGEP(valueType(inType), input, 1),
         valueType(outType)->getElementType(1));
  }
  Value *loadFailure(Value *pointer, unsigned index) {
    return builder.CreateLoad(i64, builder.CreateStructGEP(failureType, pointer, index));
  }
  Value *hasFailure(Value *pointer) {
    return builder.CreateICmpNE(
        builder.CreateLoad(i32, builder.CreateStructGEP(failureType, pointer, 0)), c32(0));
  }
  void retainRecordArray(const ValueType &type, Value *input, Function *fn, Block *failed) {
    const auto cellType = recordArrayElement(type);
    auto *cell = scalarType(cellType);
    auto *ready = call("kalandra_invocation_ready_v1", boolean, {fn->getArg(0)});
    auto *validate = Block::Create(context, "array.validate", fn);
    builder.CreateCondBr(ready, validate, failed);
    builder.SetInsertPoint(validate);
    auto *valid =
        call("kalandra_record_array_retain_v1", boolean,
             {fn->getArg(0), fn->getArg(2), input, c64(size(cell)), c64(alignment(cell))});
    auto *begin = Block::Create(context, "array.begin", fn);
    builder.CreateCondBr(valid, begin, failed);
    builder.SetInsertPoint(begin);
    auto *value = builder.CreateLoad(descriptor(type), input);
    auto *count = builder.CreateExtractValue(value, {3, 0});
    auto *storage = builder.CreateExtractValue(value, {1});
    auto *head = Block::Create(context, "array.head", fn);
    auto *body = Block::Create(context, "array.body", fn);
    auto *done = Block::Create(context, "array.done", fn);
    builder.CreateBr(head);
    builder.SetInsertPoint(head);
    auto *index = builder.CreatePHI(i64, 2, "element");
    index->addIncoming(c64(0), begin);
    auto *gate = Block::Create(context, "array.poll.gate", fn);
    auto *poll = Block::Create(context, "array.poll", fn);
    builder.CreateCondBr(builder.CreateICmpULT(index, count), gate, done);
    builder.SetInsertPoint(gate);
    builder.CreateCondBr(builder.CreateICmpEQ(builder.CreateAnd(index, c64(255)), c64(0)), poll,
                         body);
    builder.SetInsertPoint(poll);
    auto *status = call("kalandra_poll_v1", i64, {fn->getArg(0)});
    builder.CreateCondBr(builder.CreateICmpEQ(status, c64(0)), body, failed);
    builder.SetInsertPoint(body);
    auto *address = builder.CreateInBoundsGEP(cell, storage, index);
    auto *retained = builder.CreateCall(recordRetainer(*cellType.record),
                                        {fn->getArg(0), fn->getArg(1), fn->getArg(2), address});
    auto *retainedBlock = Block::Create(context, "array.element.retained", fn);
    builder.CreateCondBr(retained, retainedBlock, failed);
    builder.SetInsertPoint(retainedBlock);
    auto *next = builder.CreateAdd(index, c64(1));
    index->addIncoming(next, builder.GetInsertBlock());
    builder.CreateBr(head);
    builder.SetInsertPoint(done);
  }
  void retainValue(const ValueType &field, Value *address, Function *fn, Block *failed,
                   std::optional<std::size_t> taskKind = {}) {
    if (containsFunctionStorage(planner.input, field)) {
      copyFunctionPayload(field, address, fn, failed);
    } else if (field.builtin == BuiltinValue::TaskError) {
      auto *kind = builder.CreateLoad(i32, builder.CreateStructGEP(failureType, address, 0));
      auto *next = Block::Create(context, "task.error.validated", fn);
      builder.CreateCondBr(taskKind ? builder.CreateICmpEQ(kind, c32(*taskKind))
                                    : builder.CreateAnd(builder.CreateICmpUGE(kind, c32(1)),
                                                        builder.CreateICmpULE(kind, c32(4))),
                           next, failed);
      builder.SetInsertPoint(next);
    } else if (isFunctionArray(field)) {
      auto *success = call("kalandra_record_array_retain_v1", boolean,
                           {fn->getArg(0), fn->getArg(2), address, c64(16), c64(8)});
      auto *next = Block::Create(context, "function.array.next", fn);
      builder.CreateCondBr(success, next, failed);
      builder.SetInsertPoint(next);
    } else if (isRecordArray(field))
      retainRecordArray(field, address, fn, failed);
    else if (field.builtin == BuiltinValue::Record)
      retainRecord(field, address, fn, failed);
    else if (isStringArray(field)) {
      auto *success =
          call("kalandra_string_array_retain_v1", boolean, {fn->getArg(0), fn->getArg(2), address});
      auto *next = Block::Create(context, "record.string.array.next", fn);
      builder.CreateCondBr(success, next, failed);
      builder.SetInsertPoint(next);
    } else if (field.builtin == BuiltinValue::String) {
      auto *success =
          call("kalandra_string_retain_v1", boolean, {fn->getArg(0), fn->getArg(2), address});
      auto *next = Block::Create(context, "record.string.next", fn);
      builder.CreateCondBr(success, next, failed);
      builder.SetInsertPoint(next);
    } else if (field.tensor) {
      auto *success = isBoolArray(field)
                          ? call("kalandra_bool_array_retain_v1", boolean,
                                 {fn->getArg(0), fn->getArg(2), address})
                          : call("kalandra_record_retain_v1", boolean,
                                 {fn->getArg(0), fn->getArg(2), address, c64(field.shape.size()),
                                  c64(static_cast<unsigned>(field.element))});
      for (std::size_t axis = 0; axis < field.shape.size(); ++axis)
        if (field.shape[axis].constant) {
          auto *dimensions = builder.CreateStructGEP(descriptor(field), address, 3);
          auto *extent = builder.CreateLoad(
              i64, builder.CreateInBoundsGEP(llvm::ArrayType::get(i64, field.shape.size()),
                                             dimensions, {c64(0), c64(axis)}));
          success = builder.CreateAnd(
              success, builder.CreateICmpEQ(extent, c64(*field.shape[axis].constant)));
        }
      auto *next = Block::Create(context, "record.next", fn);
      builder.CreateCondBr(success, next, failed);
      builder.SetInsertPoint(next);
    }
  }
  void retainRecord(const ValueType &type, Value *input, Function *fn, Block *failed) {
    const auto &record = planner.input.records[*type.record];
    const auto &fields = record.fields;
    const auto retainField = [&](std::size_t i, std::optional<std::size_t> taskKind = {}) {
      if (taskTypes->fieldStorage(*type.record, i) == RecordFieldStorage::ManagedReference) {
        TaskRecordOps(planner.input, *taskTypes, module)
            .copyManagedField(builder, fn->getArg(0), fn->getArg(2), input, *type.record, i,
                              failed);
      } else {
        retainValue(fields[i].type, builder.CreateStructGEP(scalarType(type), input, i), fn, failed,
                    taskKind);
      }
    };
    if (record.variants.empty()) {
      for (std::size_t i = 0; i < fields.size(); ++i)
        retainField(i);
    } else {
      auto *tag = builder.CreateLoad(i64, builder.CreateStructGEP(scalarType(type), input, 0));
      auto *done = Block::Create(context, "enum.retained", fn);
      auto *dispatch = builder.CreateSwitch(tag, failed, record.variants.size());
      for (std::size_t i = 0; i < record.variants.size(); ++i) {
        auto *active = Block::Create(context, "enum.payload", fn);
        dispatch->addCase(c64(i + 1), active);
        builder.SetInsertPoint(active);
        const auto [origin, variant] = errorOrigin(planner.input, *type.record, i);
        for (auto slot : record.variants[i].fields)
          retainField(slot, planner.input.records[origin].taskErrorFamily
                                ? std::optional{variant + 1}
                                : std::nullopt);
        builder.CreateBr(done);
      }
      builder.SetInsertPoint(done);
    }
  }
  void copyFunctionPayload(const ValueType &type, Value *address, Function *fn, Block *failed) {
    layout(type);
    auto *wrapper = valueType(type);
    llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto *input = entry.CreateAlloca(wrapper);
    auto *output = entry.CreateAlloca(wrapper);
    auto *value = builder.CreateLoad(scalarType(type), address);
    builder.CreateStore(
        builder.CreateInsertValue(llvm::Constant::getNullValue(wrapper), value, {1}), input);
    auto *ok = builder.CreateCall(copyCallbacks.at(key(type)),
                                  {fn->getArg(0), output, fn->getArg(2), input});
    auto *ready = Block::Create(context, "function.payload.copied", fn);
    builder.CreateCondBr(ok, ready, failed);
    builder.SetInsertPoint(ready);
    builder.CreateStore(builder.CreateExtractValue(builder.CreateLoad(wrapper, output), {1}),
                        address);
  }
  void emitAggregateCopy(const ValueType &type, Function *fn) {
    auto *wrapper = valueType(type);
    builder.SetInsertPoint(Block::Create(context, "entry", fn));
    auto *scratch = builder.CreateAlloca(wrapper);
    auto *denied = Block::Create(context, "copy.denied", fn);
    auto *start = Block::Create(context, "copy.start", fn);
    auto *failed = Block::Create(context, "copy.failed", fn);
    auto *done = Block::Create(context, "copy.done", fn);
    auto *success = Block::Create(context, "copy.success", fn);
    auto *entered = call("kalandra_call_enter_v1", boolean, {fn->getArg(0), c64(0), c64(0)});
    builder.CreateCondBr(entered, start, denied);
    builder.SetInsertPoint(denied);
    builder.CreateRet(builder.getFalse());
    builder.SetInsertPoint(start);
    auto *active = Block::Create(context, "copy.active", fn);
    builder.CreateCondBr(call("kalandra_invocation_ready_v1", boolean, {fn->getArg(0)}), active,
                         failed);
    builder.SetInsertPoint(active);
    copy(scratch, fn->getArg(3), wrapper);
    builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, scratch, 0)),
                         type.fallible ? done : failed, success);
    builder.SetInsertPoint(success);
    if (type.errorRecord) {
      auto *error = Block::Create(context, "copy.error", fn);
      auto *payload = Block::Create(context, "copy.payload", fn);
      builder.CreateCondBr(hasResultError(type, scratch), error, payload);
      builder.SetInsertPoint(error);
      retainRecord(errorRecordType(*type.errorRecord), application(type, scratch), fn, failed);
      builder.CreateBr(done);
      builder.SetInsertPoint(payload);
    }
    auto *payload = builder.CreateStructGEP(wrapper, scratch, 1);
    if (isFunctionArray(type) || isRecordArray(type)) {
      const auto cellType = recordArrayElement(type);
      auto *cell = scalarType(cellType);
      auto *ok = call("kalandra_function_array_clone_v1", boolean,
                      {fn->getArg(0), payload, fn->getArg(2),
                       builder.CreateStructGEP(wrapper, fn->getArg(3), 1), c64(size(cell)),
                       c64(alignment(cell)), fn});
      auto *begin = Block::Create(context, "array.begin", fn);
      builder.CreateCondBr(ok, begin, failed);
      builder.SetInsertPoint(begin);
      auto *data = builder.CreateLoad(descriptor(type), payload);
      auto *count = builder.CreateExtractValue(data, {3, 0});
      auto *storage = builder.CreateExtractValue(data, {1});
      auto *head = Block::Create(context, "array.head", fn);
      auto *body = Block::Create(context, "array.body", fn);
      builder.CreateBr(head);
      builder.SetInsertPoint(head);
      auto *index = builder.CreatePHI(i64, 2);
      index->addIncoming(c64(0), begin);
      builder.CreateCondBr(builder.CreateICmpULT(index, count), body, done);
      builder.SetInsertPoint(body);
      auto *address = builder.CreateInBoundsGEP(cell, storage, index);
      copyFunctionPayload(cellType, address, fn, failed);
      auto *next = builder.CreateAdd(index, c64(1));
      index->addIncoming(next, builder.GetInsertBlock());
      builder.CreateBr(head);
    } else if (type.builtin == BuiltinValue::Record) {
      retainRecord(type, payload, fn, failed);
      builder.CreateBr(done);
    } else {
      // Only an expected-error payload contains functions; the success value
      // still follows its existing ordinary retention rules.
      auto completed = type;
      completed.fallible = false;
      completed.errorRecord.reset();
      retainValue(completed, payload, fn, failed);
      builder.CreateBr(done);
    }
    builder.SetInsertPoint(done);
    copy(fn->getArg(1), scratch, wrapper);
    call("kalandra_call_leave_v1", voidType, {fn->getArg(0)});
    builder.CreateRet(builder.getTrue());
    builder.SetInsertPoint(failed);
    auto *status = call("kalandra_invocation_ready_v1", boolean, {fn->getArg(0)});
    auto *fault = Block::Create(context, "copy.invalid", fn);
    auto *leave = Block::Create(context, "copy.leave", fn);
    builder.CreateCondBr(status, fault, leave);
    builder.SetInsertPoint(fault);
    call("kalandra_fault_v1", voidType, {fn->getArg(0), c64(3), c64(0), c64(0)});
    builder.CreateBr(leave);
    builder.SetInsertPoint(leave);
    call("kalandra_call_leave_v1", voidType, {fn->getArg(0)});
    builder.CreateRet(builder.getFalse());
    if (auto error = nativeStackBudgetError(*fn, module.getDataLayout()))
      throw std::runtime_error(*error);
  }
  void emitFunctionCopy(const ValueType &type, Function *fn) {
    auto *wrapper = valueType(type);
    builder.SetInsertPoint(Block::Create(context, "entry", fn));
    auto *failed = Block::Create(context, "function.invalid", fn);
    auto *stopped = Block::Create(context, "function.stopped", fn);
    auto *header = Block::Create(context, "function.header", fn);
    auto *dispatch = Block::Create(context, "function.dispatch", fn);
    auto *denied = Block::Create(context, "function.denied", fn);
    auto *entered = call("kalandra_call_enter_v1", boolean, {fn->getArg(0), c64(0), c64(0)});
    builder.CreateCondBr(entered, header, denied);
    builder.SetInsertPoint(denied);
    builder.CreateRet(builder.getFalse());
    builder.SetInsertPoint(stopped);
    call("kalandra_call_leave_v1", voidType, {fn->getArg(0)});
    builder.CreateRet(builder.getFalse());
    builder.SetInsertPoint(header);
    auto *active = Block::Create(context, "function.active", fn);
    builder.CreateCondBr(call("kalandra_invocation_ready_v1", boolean, {fn->getArg(0)}), active,
                         stopped);
    builder.SetInsertPoint(active);
    Value *errorInput = fn->getArg(3);
    if (type.errorRecord) {
      errorInput = builder.CreateAlloca(wrapper);
      copy(errorInput, fn->getArg(3), wrapper);
    }
    auto *errorReady = type.fallible ? Block::Create(context, "function.error.ready", fn) : failed;
    auto *success = type.errorRecord ? Block::Create(context, "function.success", fn) : dispatch;
    builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), errorReady,
                         success);
    if (type.errorRecord) {
      builder.SetInsertPoint(success);
      auto *active = Block::Create(context, "function.application.error", fn);
      builder.CreateCondBr(hasResultError(type, fn->getArg(3)), active, dispatch);
      builder.SetInsertPoint(active);
      retainRecord(errorRecordType(*type.errorRecord), application(type, errorInput), fn, failed);
      builder.CreateBr(errorReady);
    }
    if (type.fallible) {
      builder.SetInsertPoint(errorReady);
      copy(fn->getArg(1), errorInput, wrapper);
      call("kalandra_call_leave_v1", voidType, {fn->getArg(0)});
      builder.CreateRet(builder.getTrue());
    }
    builder.SetInsertPoint(dispatch);
    auto *input = builder.CreateLoad(wrapper, fn->getArg(3));
    auto *value = builder.CreateExtractValue(input, {1});
    auto *code = builder.CreateExtractValue(value, {0});
    auto *environment = builder.CreateExtractValue(value, {1});
    TaskTraversal traversal(builder, *fn, environment, failed);
    std::unordered_set<Value *> accepted;
    const auto accept = [&](const TypedModule &inputModule, const FunctionReference &ref,
                            const ValueType &actual, Value *entry) {
      const auto &target = inputModule.functions.at(ref.function);
      auto signature = type;
      signature.fallible = false;
      signature.errorRecord.reset();
      if (!entry || !sameFunctionValueType(signature, actual) || accepted.contains(entry))
        return;
      std::vector<ValueType> captures;
      std::vector<Type *> fields;
      for (std::size_t n = target.parameters.size() - target.captureCount;
           n < target.parameters.size(); ++n) {
        auto capture = inputModule.symbols.at(target.parameters[n]).type;
        if (!workerClosureCaptureTypeSupported(inputModule, capture))
          return;
        captures.push_back(capture);
        fields.push_back(scalarType(capture));
      }
      accepted.insert(entry);
      auto *selected = Block::Create(context, "function.target", fn);
      auto *next = Block::Create(context, "function.next", fn);
      builder.CreateCondBr(builder.CreateICmpEQ(code, entry), selected, next);
      builder.SetInsertPoint(selected);
      Value *copied = environment;
      if (recursiveClosures) {
        TaskCallableOps(module).retain(builder, fn->getArg(0), fn->getArg(2), value, entry,
                                       llvm::StructType::get(context, fields), failed);
      } else if (captures.empty()) {
        auto *ready = Block::Create(context, "function.empty", fn);
        builder.CreateCondBr(
            builder.CreateICmpEQ(environment, llvm::ConstantPointerNull::get(builder.getPtrTy())),
            ready, failed);
        builder.SetInsertPoint(ready);
      } else {
        auto *captureLayout = llvm::StructType::get(context, fields);
        copied = call("kalandra_environment_clone_v1", pointer,
                      {fn->getArg(0), fn->getArg(2), environment, c64(size(captureLayout)),
                       c64(alignment(captureLayout)), entry});
        auto *ready = Block::Create(context, "function.cloned", fn);
        builder.CreateCondBr(builder.CreateIsNotNull(copied), ready, failed);
        builder.SetInsertPoint(ready);
        for (std::size_t i = 0; i < captures.size(); ++i) {
          auto *field = builder.CreateStructGEP(captureLayout, copied, i);
          if (captures[i].builtin == BuiltinValue::Function) {
            // Register layouts before their bodies so equal-signature nested
            // captures reuse this copy entry instead of expanding code forever.
            layout(captures[i]);
            auto *nested = copyCallbacks.at(key(captures[i]));
            auto *nestedType = valueType(captures[i]);
            auto *source = builder.CreateAlloca(nestedType);
            auto *destination = builder.CreateAlloca(nestedType);
            auto *payload = builder.CreateLoad(scalarType(captures[i]), field);
            builder.CreateStore(
                builder.CreateInsertValue(llvm::Constant::getNullValue(nestedType), payload, {1}),
                source);
            auto *ok =
                builder.CreateCall(nested, {fn->getArg(0), destination, fn->getArg(2), source});
            auto *ready = Block::Create(context, "capture.function.copied", fn);
            builder.CreateCondBr(ok, ready, failed);
            builder.SetInsertPoint(ready);
            builder.CreateStore(
                builder.CreateExtractValue(builder.CreateLoad(nestedType, destination), {1}),
                field);
          } else if (captures[i].builtin == BuiltinValue::Numeric && captures[i].tensor) {
            auto *ok = call("kalandra_environment_tensor_retain_v1", boolean,
                            {fn->getArg(0), fn->getArg(2), field, c64(captures[i].shape.size()),
                             c64(static_cast<unsigned>(captures[i].element))});
            for (std::size_t axis = 0; axis < captures[i].shape.size(); ++axis)
              if (captures[i].shape[axis].constant) {
                auto *dims = builder.CreateStructGEP(descriptor(captures[i]), field, 3);
                auto *extent = builder.CreateLoad(
                    i64,
                    builder.CreateInBoundsGEP(llvm::ArrayType::get(i64, captures[i].shape.size()),
                                              dims, {c64(0), c64(axis)}));
                ok = builder.CreateAnd(
                    ok, builder.CreateICmpEQ(extent, c64(*captures[i].shape[axis].constant)));
              }
            auto *ready = Block::Create(context, "capture.retained", fn);
            builder.CreateCondBr(ok, ready, failed);
            builder.SetInsertPoint(ready);
          } else
            retainValue(captures[i], field, fn, failed);
        }
      }
      auto *closure = builder.CreateInsertValue(value, copied, {1});
      builder.CreateStore(builder.CreateInsertValue(input, closure, {1}), fn->getArg(1));
      call("kalandra_call_leave_v1", voidType, {fn->getArg(0)});
      builder.CreateRet(builder.getTrue());
      builder.SetInsertPoint(next);
    };
    for (const auto &expression : planner.kernels.expressions)
      if (const auto *ref = std::get_if<FunctionReference>(&expression.operation))
        accept(planner.kernels, *ref, expression.type,
               module.getFunction("kalandra_callable_" + std::to_string(ref->function)));
    for (const auto &expression : planner.input.expressions)
      if (const auto *ref = std::get_if<FunctionReference>(&expression.operation))
        accept(planner.input, *ref, expression.type,
               coordinating.at(ref->function) ? static_cast<Value *>(entries.at(ref->function))
                                              : synchronousEntries.at(ref->function));
    builder.CreateBr(failed);
    builder.SetInsertPoint(failed);
    auto *fault = Block::Create(context, "function.fault", fn);
    auto *current = call("kalandra_invocation_ready_v1", boolean, {fn->getArg(0)});
    builder.CreateCondBr(current, fault, stopped);
    builder.SetInsertPoint(fault);
    call("kalandra_fault_v1", voidType, {fn->getArg(0), c64(3), c64(0), c64(0)});
    builder.CreateBr(stopped);
    traversal.finish();
    if (auto error = nativeStackBudgetError(*fn, module.getDataLayout()))
      throw std::runtime_error(*error);
  }
  llvm::GlobalVariable *layout(const ValueType &type) {
    const auto name = key(type);
    if (auto found = layouts.find(name); found != layouts.end())
      return found->second;
    llvm::IRBuilderBase::InsertPointGuard guard(builder);
    auto *wrapper = valueType(type);
    auto *fn = function("value.copy." + name, boolean, {pointer, pointer, pointer, pointer});
    copyCallbacks.emplace(name, fn);
    const auto layoutIndex = layouts.size();
    auto *result =
        new llvm::GlobalVariable(module, layoutType, true, llvm::GlobalValue::InternalLinkage,
                                 nullptr, "value.layout." + name);
    layouts[name] = result;
    const bool functionCopy = type.builtin == BuiltinValue::Function && !type.tensor && !type.task;
    if (functionCopy)
      emitFunctionCopy(type, fn);
    else if (!type.task && containsFunctionStorage(planner.input, type))
      emitAggregateCopy(type, fn);
    else {
      builder.SetInsertPoint(Block::Create(context, "entry", fn));
      auto *activeContext = Block::Create(context, "copy.active.context", fn);
      auto *stoppedContext = Block::Create(context, "copy.stopped.context", fn);
      builder.CreateCondBr(call("kalandra_invocation_ready_v1", boolean, {fn->getArg(0)}),
                           activeContext, stoppedContext);
      builder.SetInsertPoint(stoppedContext);
      builder.CreateRet(builder.getFalse());
      builder.SetInsertPoint(activeContext);
      auto *ready = Block::Create(context, "copy", fn);
      if (type.errorRecord) {
        auto *dispatch = Block::Create(context, "application.dispatch", fn);
        auto *active = Block::Create(context, "application.retain", fn);
        auto *success = Block::Create(context, "application.success", fn);
        auto *failed = Block::Create(context, "application.failed", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             dispatch);
        builder.SetInsertPoint(dispatch);
        builder.CreateCondBr(hasResultError(type, fn->getArg(3)), active, success);
        builder.SetInsertPoint(active);
        retainRecord(errorRecordType(*type.errorRecord), application(type, fn->getArg(3)), fn,
                     failed);
        builder.CreateBr(ready);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
        builder.SetInsertPoint(success);
      }
      if (type.builtin == BuiltinValue::TaskError && !type.task) {
        auto *validate = Block::Create(context, "task.value.success", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             validate);
        builder.SetInsertPoint(validate);
        auto *data = builder.CreateStructGEP(wrapper, fn->getArg(3), 1);
        auto *kind = builder.CreateLoad(i32, builder.CreateStructGEP(failureType, data, 0));
        auto *failed = Block::Create(context, "task.value.invalid", fn);
        builder.CreateCondBr(builder.CreateAnd(builder.CreateICmpUGE(kind, c32(1)),
                                               builder.CreateICmpULE(kind, c32(4))),
                             ready, failed);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else if (isStringArray(type) && !type.task) {
        auto *retain = Block::Create(context, "string.array.retain", fn);
        auto *failed = Block::Create(context, "string.array.failed", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             retain);
        builder.SetInsertPoint(retain);
        auto *success = call(
            "kalandra_string_array_retain_v1", boolean,
            {fn->getArg(0), fn->getArg(2), builder.CreateStructGEP(wrapper, fn->getArg(3), 1)});
        builder.CreateCondBr(success, ready, failed);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else if (type.builtin == BuiltinValue::String && !type.task) {
        auto *retain = Block::Create(context, "string.retain", fn);
        auto *failed = Block::Create(context, "string.failed", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             retain);
        builder.SetInsertPoint(retain);
        auto *success = call(
            "kalandra_string_retain_v1", boolean,
            {fn->getArg(0), fn->getArg(2), builder.CreateStructGEP(wrapper, fn->getArg(3), 1)});
        builder.CreateCondBr(success, ready, failed);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else if (isBoolArray(type)) {
        auto *retain = Block::Create(context, "bool.array.retain", fn);
        auto *failed = Block::Create(context, "bool.array.failed", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             retain);
        builder.SetInsertPoint(retain);
        auto *success = call(
            "kalandra_bool_array_retain_v1", boolean,
            {fn->getArg(0), fn->getArg(2), builder.CreateStructGEP(wrapper, fn->getArg(3), 1)});
        builder.CreateCondBr(success, ready, failed);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else if (isFunctionArray(type)) {
        auto *retain = Block::Create(context, "function.array.retain", fn);
        auto *failed = Block::Create(context, "function.array.failed", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             retain);
        builder.SetInsertPoint(retain);
        auto *success = call("kalandra_record_array_retain_v1", boolean,
                             {fn->getArg(0), fn->getArg(2),
                              builder.CreateStructGEP(wrapper, fn->getArg(3), 1), c64(16), c64(8)});
        builder.CreateCondBr(success, ready, failed);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else if (isRecordArray(type)) {
        auto *retain = Block::Create(context, "array.retain", fn);
        auto *failed = Block::Create(context, "array.failed", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             retain);
        builder.SetInsertPoint(retain);
        retainRecordArray(type, builder.CreateStructGEP(wrapper, fn->getArg(3), 1), fn, failed);
        builder.CreateBr(ready);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else if (type.tensor) {
        auto *retain = Block::Create(context, "retain", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             retain);
        builder.SetInsertPoint(retain);
        auto *tensor = builder.CreateStructGEP(wrapper, fn->getArg(3), 1);
        auto *success = call("kalandra_environment_tensor_retain_v1", boolean,
                             {fn->getArg(0), fn->getArg(2), tensor, c64(type.shape.size()),
                              c64(static_cast<unsigned>(type.element))});
        auto *failed = Block::Create(context, "failed", fn);
        builder.CreateCondBr(success, ready, failed);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else if (type.builtin == BuiltinValue::Record && !type.task) {
        auto *retain = Block::Create(context, "record.retain", fn);
        auto *failed = Block::Create(context, "record.failed", fn);
        builder.CreateCondBr(hasFailure(builder.CreateStructGEP(wrapper, fn->getArg(3), 0)), ready,
                             retain);
        builder.SetInsertPoint(retain);
        retainRecord(type, builder.CreateStructGEP(wrapper, fn->getArg(3), 1), fn, failed);
        builder.CreateBr(ready);
        builder.SetInsertPoint(failed);
        builder.CreateRet(builder.getFalse());
      } else
        builder.CreateBr(ready);
      builder.SetInsertPoint(ready);
      copy(fn->getArg(1), fn->getArg(3), wrapper);
      builder.CreateRet(builder.getTrue());
    }
    auto *constant = llvm::ConstantStruct::get(
        layoutType,
        {c64(type.errorRecord ? 0x1000000 + layoutIndex
             : type.builtin == BuiltinValue::Function && !type.tensor && !type.task
                 ? 0x200000000ULL + functionSignature(type)
             : isFunctionArray(type) && !type.task
                 ? 0x900000000ULL + functionSignature(recordArrayElement(type))
             : isStringArray(type) && !type.task                  ? 0x80000
             : isBoolArray(type) && !type.task                    ? 0x70000
             : isRecordArray(type) && !type.task                  ? 0x60000 + *type.record
             : type.builtin == BuiltinValue::Record && !type.task ? 0x50000 + *type.record
             : type.builtin != BuiltinValue::Numeric ? 0x10000 + static_cast<unsigned>(type.builtin)
             : type.array                            ? 0x40000 + static_cast<unsigned>(type.element)
             : type.tensor
                 ? (isInteger(type.element) || type.element == NumericType::Float64
                        ? 0x30000 + 16 * static_cast<unsigned>(type.element) + type.shape.size()
                        : type.shape.size() + 2)
             : isInteger(type.element)              ? 0x20000 + static_cast<unsigned>(type.element)
             : type.element == NumericType::Float64 ? 0x20000 + static_cast<unsigned>(type.element)
                                                    : 1),
         c64(size(wrapper)), c64(alignment(wrapper)), fn, destroy});
    result->setInitializer(constant);
    return result;
  }
  void fillPacket(Value *packet, std::size_t target, Value *data, Value *ctx) {
    builder.CreateStore(c64(target + 100), builder.CreateStructGEP(packetType, packet, 0));
    builder.CreateStore(data, builder.CreateStructGEP(packetType, packet, 1));
    builder.CreateStore(c64(size(packets[target])), builder.CreateStructGEP(packetType, packet, 2));
    builder.CreateStore(ctx, builder.CreateStructGEP(packetType, packet, 3));
  }
  void emitPlan(std::size_t id, bool synchronous = false) {
    const auto &plan = *planner.plans[id];
    auto *frameType = frames[id];
    Function *construct = nullptr;
    if (!synchronous) {
      construct =
          function("task.construct." + std::to_string(id), boolean, {pointer, pointer, pointer});
      builder.SetInsertPoint(Block::Create(context, "entry", construct));
      builder.CreateMemSet(construct->getArg(1), builder.getInt8(0), size(frameType),
                           llvm::MaybeAlign(alignment(frameType)));
      auto *data =
          builder.CreateLoad(pointer, builder.CreateStructGEP(packetType, construct->getArg(2), 1));
      auto *source =
          builder.CreateLoad(pointer, builder.CreateStructGEP(packetType, construct->getArg(2), 3));
      auto *failed = Block::Create(context, "failed", construct);
      for (std::size_t n = 0; n < plan.parameters.size(); ++n) {
        auto slot = plan.parameters[n];
        auto *target = builder.CreateStructGEP(frameType, construct->getArg(1), slot + 1);
        auto *input = builder.CreateStructGEP(packets[id], data, n);
        auto *layoutValue = layout(plan.slots[slot]);
        auto *fn = llvm::cast<llvm::ConstantStruct>(layoutValue->getInitializer())->getOperand(3);
        auto *copyFn = llvm::cast<Function>(fn);
        auto *ok = builder.CreateCall(copyFn, {construct->getArg(0), target, source, input});
        auto *next = Block::Create(context, "parameter", construct);
        builder.CreateCondBr(ok, next, failed);
        builder.SetInsertPoint(next);
      }
      builder.CreateRet(builder.getTrue());
      builder.SetInsertPoint(failed);
      builder.CreateRet(builder.getFalse());
    }
    const auto resultType = plan.slots[plan.result];
    auto *resume = synchronous ? synchronousEntries.at(id)
                               : function("task.resume." + std::to_string(id), voidType, {pointer});
    builder.SetInsertPoint(Block::Create(context, "entry", resume));
    Value *task = synchronous
                      ? static_cast<Value *>(llvm::ConstantPointerNull::get(builder.getPtrTy()))
                      : resume->getArg(0);
    Value *ctx = synchronous ? resume->getArg(resume->arg_size() - 1)
                             : call("kalandra_task_context_v1", pointer, {task});
    Value *frame;
    auto returnZero = [&](bool admitted = true) {
      if (synchronous && admitted)
        call("kalandra_call_leave_v1", voidType, {ctx});
      if (resume->getReturnType()->isVoidTy())
        builder.CreateRetVoid();
      else
        builder.CreateRet(llvm::Constant::getNullValue(resume->getReturnType()));
    };
    if (synchronous) {
      auto *ready = Block::Create(context, "call.ready", resume);
      auto *stopped = Block::Create(context, "call.stopped", resume);
      const auto [line, column] =
          planner.input.source->location(planner.input.functions[id].declaration.begin);
      auto *admitted = call("kalandra_call_enter_v1", boolean, {ctx, c64(line), c64(column)});
      builder.CreateCondBr(admitted, ready, stopped);
      builder.SetInsertPoint(stopped);
      returnZero(false);
      builder.SetInsertPoint(ready);
      if (recursiveClosures) {
        auto *active = Block::Create(context, "call.active", resume);
        auto *inactive = Block::Create(context, "call.inactive", resume);
        builder.CreateCondBr(call("kalandra_invocation_ready_v1", boolean, {ctx}), active,
                             inactive);
        builder.SetInsertPoint(inactive);
        returnZero();
        builder.SetInsertPoint(active);
      }
      frame = call("kalandra_alloc_v1", pointer, {ctx, c64(size(frameType))});
    } else
      frame = call("kalandra_task_frame_v1", pointer, {task});
    auto *packet = builder.CreateAlloca(packetType);
    // Packet staging belongs to one resume interval, never to a suspended frame.
    std::unordered_map<std::size_t, Value *> staging;
    for (const auto &instruction : plan.code)
      if (instruction.code == Code::Spawn && !staging.contains(instruction.target))
        staging[instruction.target] = builder.CreateAlloca(packets[instruction.target]);
    auto *spawnFailure = staging.empty() ? nullptr : builder.CreateAlloca(failureType);
    std::unordered_map<std::size_t, Value *> callResults;
    for (const auto &[instruction, field] : recursiveResultFields[id])
      callResults[instruction] = builder.CreateStructGEP(frameType, frame, field);
    std::unordered_map<std::size_t, Value *> observations;
    for (const auto &[target, field] : observationFields[id])
      observations[target] = builder.CreateStructGEP(frameType, frame, field);
    std::vector<std::size_t> groupExits(plan.groups, SIZE_MAX);
    for (std::size_t n = 0; n < plan.code.size(); ++n)
      if (plan.code[n].code == Code::Join)
        groupExits.at(plan.code[n].group) = n;
    auto *leave = Block::Create(context, "suspend", resume);
    auto *abort = Block::Create(context, "fault", resume);
    std::vector<Block *> blocks;
    for (std::size_t n = 0; n < plan.code.size(); ++n)
      blocks.push_back(Block::Create(context, "step." + std::to_string(n), resume));
    auto *pc = builder.CreateStructGEP(frameType, frame, 0);
    if (synchronous) {
      auto *initialized = Block::Create(context, "call.initialize", resume);
      auto *status = call("kalandra_poll_v1", i64, {ctx});
      builder.CreateCondBr(builder.CreateICmpEQ(status, c64(0)), initialized, abort);
      builder.SetInsertPoint(initialized);
      builder.CreateMemSet(frame, builder.getInt8(0), size(frameType),
                           llvm::MaybeAlign(alignment(frameType)));
      const auto captures = planner.input.functions[id].captureCount;
      const auto explicitCount = plan.parameters.size() - captures;
      std::vector<Type *> captureTypes;
      for (std::size_t n = explicitCount; n < plan.parameters.size(); ++n)
        captureTypes.push_back(scalarType(plan.slots[plan.parameters[n]]));
      auto *environment = llvm::StructType::get(context, captureTypes);
      Value *captureAddress = resume->getArg(0);
      if (recursiveClosures && captures) {
        captureAddress = call("kalandra_recursive_environment_read_v1", pointer,
                              {ctx, captureAddress, c64(size(environment)), resume});
        auto *ready = Block::Create(context, "captures.ready", resume);
        builder.CreateCondBr(builder.CreateIsNotNull(captureAddress), ready, abort);
        builder.SetInsertPoint(ready);
      }
      if (recursiveClosures && !captures) {
        auto *ready = Block::Create(context, "captures.empty", resume);
        builder.CreateCondBr(builder.CreateIsNull(captureAddress), ready, abort);
        builder.SetInsertPoint(ready);
      }
      for (std::size_t n = 0; n < plan.parameters.size(); ++n) {
        const auto parameter = plan.parameters[n];
        auto *slot = builder.CreateStructGEP(frameType, frame, parameter + 1);
        Value *value =
            n < explicitCount
                ? static_cast<Value *>(resume->getArg(1 + n + (resultType.fallible ? 1 : 0)))
                : builder.CreateLoad(
                      captureTypes[n - explicitCount],
                      builder.CreateStructGEP(environment, captureAddress, n - explicitCount));
        builder.CreateStore(value,
                            builder.CreateStructGEP(valueType(plan.slots[parameter]), slot, 1));
      }
      builder.CreateBr(blocks.front());
    } else {
      auto *dispatch = builder.CreateSwitch(builder.CreateLoad(i32, pc), abort, blocks.size());
      for (std::size_t n = 0; n < blocks.size(); ++n)
        dispatch->addCase(c32(n), blocks[n]);
    }
    builder.SetInsertPoint(leave);
    returnZero();
    builder.SetInsertPoint(abort);
    if (synchronous) {
      auto *invalid = Block::Create(context, "call.invalid", resume);
      auto *cleanup = Block::Create(context, "call.cleanup", resume);
      auto *status = call("kalandra_poll_v1", i64, {ctx});
      builder.CreateCondBr(builder.CreateICmpEQ(status, c64(0)), invalid, cleanup);
      builder.SetInsertPoint(invalid);
      const auto [line, column] =
          planner.input.source->location(planner.input.functions[id].declaration.begin);
      call("kalandra_fault_v1", voidType, {ctx, c64(3), c64(line), c64(column)});
      builder.CreateBr(cleanup);
      builder.SetInsertPoint(cleanup);
      auto *release = Block::Create(context, "call.release", resume);
      auto *done = Block::Create(context, "call.failed", resume);
      builder.CreateCondBr(
          builder.CreateICmpNE(frame, llvm::ConstantPointerNull::get(builder.getPtrTy())), release,
          done);
      builder.SetInsertPoint(release);
      call("kalandra_free_v1", voidType, {ctx, frame});
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      returnZero();
    } else {
      auto *status = call("kalandra_status_v1", i64, {ctx});
      auto *cancelled = call("kalandra_task_cancelled_v1", boolean, {task});
      auto *cancelExit = Block::Create(context, "cancelled", resume);
      auto *faultExit = Block::Create(context, "checked.fault", resume);
      builder.CreateCondBr(builder.CreateAnd(cancelled, builder.CreateICmpEQ(status, c64(0))),
                           cancelExit, faultExit);
      builder.SetInsertPoint(cancelExit);
      builder.CreateRetVoid();
      builder.SetInsertPoint(faultExit);
      status = builder.CreateSelect(builder.CreateICmpEQ(status, c64(0)), c64(3), status);
      call("kalandra_task_fault_v1", voidType, {task, status, c64(0), c64(0)});
      builder.CreateRetVoid();
    }
    const auto slot = [&](Slot n) { return builder.CreateStructGEP(frameType, frame, n + 1); };
    const auto payload = [&](Slot n) {
      return builder.CreateStructGEP(valueType(plan.slots[n]), slot(n), 1);
    };
    const auto group = [&](std::size_t n) {
      return builder.CreateStructGEP(frameType, frame, plan.slots.size() + 1 + n);
    };
    for (std::size_t n = 0; n < plan.code.size(); ++n) {
      const auto &instruction = plan.code[n];
      builder.SetInsertPoint(blocks[n]);
      auto *next = n + 1 < blocks.size() ? blocks[n + 1] : abort;
      auto check = [&] {
        auto *status = call("kalandra_poll_v1", i64, {ctx});
        builder.CreateCondBr(builder.CreateICmpEQ(status, c64(0)), next, abort);
      };
      auto wait = [&](Value *ready) {
        builder.CreateStore(c32(n), pc);
        auto *proceed = Block::Create(context, "ready", resume);
        builder.CreateCondBr(ready, proceed, leave);
        builder.SetInsertPoint(proceed);
      };
      const auto &type = plan.slots[instruction.output];
      switch (instruction.code) {
      case Code::Literal: {
        const auto &literal = planner.input.expressions[instruction.source].operation;
        if (const auto *integer = std::get_if<Int32Literal>(&literal)) {
          builder.CreateStore(
              llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(scalarType(type)),
                                     static_cast<std::uint64_t>(integer->value), true),
              payload(instruction.output));
          builder.CreateBr(next);
          break;
        }
        if (const auto *integer = std::get_if<Int64Literal>(&literal)) {
          builder.CreateStore(c64(static_cast<std::uint64_t>(integer->value)),
                              payload(instruction.output));
          builder.CreateBr(next);
          break;
        }
        if (const auto *integer = std::get_if<UInt64Literal>(&literal)) {
          builder.CreateStore(c64(integer->value), payload(instruction.output));
          builder.CreateBr(next);
          break;
        }
        if (const auto *integer = std::get_if<UInt32Literal>(&literal)) {
          builder.CreateStore(llvm::ConstantInt::get(
                                  llvm::cast<llvm::IntegerType>(scalarType(type)), integer->value),
                              payload(instruction.output));
          builder.CreateBr(next);
          break;
        }
        if (const auto *real = std::get_if<Float64Literal>(&literal)) {
          builder.CreateStore(
              llvm::ConstantFP::get(
                  context, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, real->bits))),
              payload(instruction.output));
          builder.CreateBr(next);
          break;
        }
        if (const auto *boolean = std::get_if<BoolLiteral>(&literal)) {
          builder.CreateStore(builder.getInt1(boolean->value), payload(instruction.output));
          builder.CreateBr(next);
          break;
        }
        auto bits = std::get<Float32Literal>(literal).bits;
        builder.CreateStore(
            llvm::ConstantFP::get(
                context, llvm::APFloat(llvm::APFloat::IEEEsingle(), llvm::APInt(32, bits))),
            payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::Unary: {
        Value *value = builder.CreateLoad(f32, payload(instruction.args[0]));
        if (std::get<ScalarUnary>(planner.input.expressions[instruction.source].operation)
                .operation == TokenKind::Minus)
          value = builder.CreateFNeg(value);
        builder.CreateStore(value, payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::Binary: {
        auto *a = builder.CreateLoad(f32, payload(instruction.args[0]));
        auto *b = builder.CreateLoad(f32, payload(instruction.args[1]));
        auto op = std::get<ScalarBinary>(planner.input.expressions[instruction.source].operation)
                      .operation;
        Value *value = op == TokenKind::Plus    ? builder.CreateFAdd(a, b)
                       : op == TokenKind::Minus ? builder.CreateFSub(a, b)
                       : op == TokenKind::Star  ? builder.CreateFMul(a, b)
                                                : builder.CreateFDiv(a, b);
        builder.CreateStore(value, payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::Kernel: {
        // A shape refinement on a result must preserve an error without reading
        // its absent tensor payload. Only the success path enters the kernel.
        if (std::holds_alternative<ShapeCheck>(
                planner.input.expressions[instruction.source].operation) &&
            plan.slots[instruction.args[0]].fallible) {
          auto input = instruction.args[0];
          auto *error = Block::Create(context, "refine.error", resume);
          auto *success = Block::Create(context, "refine.success", resume);
          builder.CreateCondBr(hasResultError(plan.slots[input], slot(input)), error, success);
          builder.SetInsertPoint(error);
          copyErrors(type, slot(instruction.output), plan.slots[input], slot(input));
          builder.CreateBr(next);
          builder.SetInsertPoint(success);
        }
        auto *fn =
            module.getFunction("_mlir_ciface_kalandra_fn_" + std::to_string(instruction.target));
        std::vector<Value *> arguments;
        // MLIR C wrappers return both memrefs and LLVM aggregate values through
        // a leading output pointer, while record arguments remain values.
        const bool indirectResult = type.tensor || type.builtin == BuiltinValue::Record ||
                                    type.builtin == BuiltinValue::String ||
                                    type.builtin == BuiltinValue::TaskError ||
                                    type.builtin == BuiltinValue::Function;
        if (indirectResult)
          arguments.push_back(payload(instruction.output));
        for (auto argument : instruction.args)
          arguments.push_back(
              plan.slots[argument].tensor && !isOwnedArray(plan.slots[argument])
                  ? payload(argument)
                  : builder.CreateLoad(scalarType(plan.slots[argument]), payload(argument)));
        arguments.push_back(ctx);
        auto *value = builder.CreateCall(fn, arguments);
        if (!indirectResult) {
          auto *good = Block::Create(context, "kernel.result", resume);
          auto *status = call("kalandra_poll_v1", i64, {ctx});
          builder.CreateCondBr(builder.CreateICmpEQ(status, c64(0)), good, abort);
          builder.SetInsertPoint(good);
          builder.CreateStore(value, payload(instruction.output));
          builder.CreateBr(next);
        } else
          check();
        break;
      }
      case Code::FunctionReference: {
        Value *code = coordinating.at(instruction.target)
                          ? static_cast<Value *>(entries.at(instruction.target))
                          : synchronousEntries.at(instruction.target);
        Value *environment = llvm::ConstantPointerNull::get(builder.getPtrTy());
        if (!instruction.args.empty()) {
          std::vector<Type *> fields;
          for (auto capture : instruction.args)
            fields.push_back(scalarType(plan.slots[capture]));
          auto *layout = llvm::StructType::get(context, fields);
          if (recursiveClosures) {
            llvm::IRBuilder<> scratch(&resume->getEntryBlock(), resume->getEntryBlock().begin());
            auto *contents = scratch.CreateAlloca(layout);
            for (std::size_t i = 0; i < fields.size(); ++i)
              builder.CreateStore(builder.CreateLoad(fields[i], payload(instruction.args[i])),
                                  builder.CreateStructGEP(layout, contents, i));
            environment =
                builder.CreateCall(environmentBoxes.at(instruction.target), {contents, ctx});
            auto *boxed = Block::Create(context, "closure.boxed", resume);
            builder.CreateCondBr(builder.CreateIsNotNull(environment), boxed, abort);
            builder.SetInsertPoint(boxed);
            environment =
                call("kalandra_recursive_environment_bind_v1", pointer, {ctx, environment, code});
            auto *bound = Block::Create(context, "closure.bound", resume);
            builder.CreateCondBr(builder.CreateIsNotNull(environment), bound, abort);
            builder.SetInsertPoint(bound);
          } else {
            environment =
                call("kalandra_environment_alloc_v1", pointer, {ctx, c64(size(layout)), code});
            auto *ready = Block::Create(context, "closure.ready", resume);
            auto *status = call("kalandra_poll_v1", i64, {ctx});
            builder.CreateCondBr(builder.CreateICmpEQ(status, c64(0)), ready, abort);
            builder.SetInsertPoint(ready);
            for (std::size_t i = 0; i < fields.size(); ++i)
              builder.CreateStore(builder.CreateLoad(fields[i], payload(instruction.args[i])),
                                  builder.CreateStructGEP(layout, environment, i));
          }
        }
        auto *layout = scalarType(type);
        Value *closure = llvm::Constant::getNullValue(layout);
        closure = builder.CreateInsertValue(closure, code, {0});
        closure = builder.CreateInsertValue(closure, environment, {1});
        builder.CreateStore(closure, payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::CallableTarget: {
        const auto input = instruction.args[0];
        auto *closure = builder.CreateLoad(scalarType(plan.slots[input]), payload(input));
        builder.CreateStore(builder.CreateICmpEQ(builder.CreateExtractValue(closure, {0}),
                                                 entries.at(instruction.target)),
                            payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::CallableCaptures:
      case Code::CallableSyncGuard: {
        layout(type);
        auto *copy = copyCallbacks.at(key(type));
        auto *ready = Block::Create(context, "callable.checked", resume);
        auto *ok = builder.CreateCall(
            copy, {ctx, slot(instruction.output), ctx, slot(instruction.args[0])});
        builder.CreateCondBr(ok, ready, abort);
        builder.SetInsertPoint(ready);
        auto *closure = builder.CreateLoad(scalarType(type), payload(instruction.output));
        if (instruction.code == Code::CallableCaptures) {
          auto *environment = builder.CreateExtractValue(closure, {1});
          std::vector<Type *> fields;
          for (std::size_t i = 1; i < instruction.args.size(); ++i)
            fields.push_back(scalarType(plan.slots[instruction.args[i]]));
          auto *captureType = llvm::StructType::get(context, fields);
          for (std::size_t i = 0; i < fields.size(); ++i)
            builder.CreateStore(
                builder.CreateLoad(fields[i], builder.CreateStructGEP(captureType, environment, i)),
                payload(instruction.args[i + 1]));
          builder.CreateBr(next);
        } else {
          auto *code = builder.CreateExtractValue(closure, {0});
          Value *valid = builder.getTrue();
          for (std::size_t i = 0; i < coordinating.size(); ++i)
            if (coordinating[i])
              valid = builder.CreateAnd(valid, builder.CreateICmpNE(code, entries[i]));
          auto *invalid = Block::Create(context, "callable.invalid", resume);
          builder.CreateCondBr(valid, next, invalid);
          builder.SetInsertPoint(invalid);
          call("kalandra_fault_v1", voidType, {ctx, c64(3), c64(0), c64(0)});
          builder.CreateBr(abort);
        }
        break;
      }
      case Code::InvalidCallable:
        call("kalandra_fault_v1", voidType, {ctx, c64(3), c64(0), c64(0)});
        builder.CreateBr(abort);
        break;
      case Code::SynchronousCall: {
        const auto &target = planner.input.functions.at(instruction.target);
        std::vector<Value *> arguments{llvm::ConstantPointerNull::get(builder.getPtrTy())};
        if (target.resultType.fallible)
          arguments.push_back(callResults.at(n));
        for (auto argument : instruction.args)
          arguments.push_back(
              builder.CreateLoad(scalarType(plan.slots[argument]), payload(argument)));
        arguments.push_back(ctx);
        auto *value = builder.CreateCall(synchronousEntries.at(instruction.target), arguments);
        auto *ready = Block::Create(context, "recursive.call.ready", resume);
        auto *status = call("kalandra_poll_v1", i64, {ctx});
        builder.CreateCondBr(builder.CreateICmpEQ(status, c64(0)), ready, abort);
        builder.SetInsertPoint(ready);
        if (target.resultType.fallible)
          copyResult(type, slot(instruction.output), target.resultType, callResults.at(n));
        else
          builder.CreateStore(value, payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::FunctionInvoke: {
        const auto &operation = std::get<kalandra::FunctionInvoke>(
            planner.input.expressions[instruction.source].operation);
        const auto &signature = *planner.input.expressions[operation.callee].type.callable;
        auto *closure = builder.CreateLoad(scalarType(plan.slots[instruction.args[0]]),
                                           payload(instruction.args[0]));
        std::vector<Type *> parameters{pointer, pointer};
        std::vector<Value *> arguments{builder.CreateExtractValue(closure, {1}),
                                       slot(instruction.output)};
        for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
          auto *parameter = scalarType(signature.parameters[i]);
          parameters.push_back(parameter);
          arguments.push_back(builder.CreateLoad(parameter, payload(instruction.args[i + 1])));
        }
        parameters.push_back(pointer);
        arguments.push_back(ctx);
        auto *callee = builder.CreateExtractValue(closure, {0});
        builder.CreateCall(llvm::FunctionType::get(voidType, parameters, false), callee, arguments);
        check();
        break;
      }
      case Code::MapInit: {
        const auto input = instruction.args[0];
        const auto inputElement = recordArrayElement(plan.slots[input]);
        const auto outputElement = recordArrayElement(type);
        const auto inputBytes = size(scalarType(inputElement));
        const auto outputBytes = size(scalarType(outputElement));
        if (plan.slots[input].builtin == BuiltinValue::Numeric)
          call("kalandra_environment_tensor_retain_v1", boolean,
               {ctx, ctx, payload(input), c64(1),
                c64(static_cast<unsigned>(plan.slots[input].element))});
        else
          call("kalandra_record_array_validate_v1", voidType,
               {ctx, payload(input), c64(inputBytes), c64(alignment(scalarType(inputElement)))});
        auto *valid = Block::Create(context, "map.valid", resume);
        builder.CreateCondBr(builder.CreateICmpEQ(call("kalandra_poll_v1", i64, {ctx}), c64(0)),
                             valid, abort);
        builder.SetInsertPoint(valid);
        auto *source = builder.CreateLoad(descriptor(plan.slots[input]), payload(input));
        auto *count = builder.CreateExtractValue(source, {3, 0});
        auto *allocate = Block::Create(context, "map.allocate", resume);
        auto *invalid = Block::Create(context, "map.size.invalid", resume);
        builder.CreateCondBr(builder.CreateICmpULE(count, c64(INT64_MAX / outputBytes)), allocate,
                             invalid);
        builder.SetInsertPoint(invalid);
        call("kalandra_fault_v1", voidType, {ctx, c64(2), c64(0), c64(0)});
        builder.CreateBr(abort);
        builder.SetInsertPoint(allocate);
        auto *empty = Block::Create(context, "map.empty", resume);
        auto *storage = Block::Create(context, "map.storage", resume);
        auto *ready = Block::Create(context, "map.allocated", resume);
        builder.CreateCondBr(builder.CreateICmpEQ(count, c64(0)), empty, storage);
        builder.SetInsertPoint(empty);
        builder.CreateBr(ready);
        builder.SetInsertPoint(storage);
        auto *data = call(isOwnedArray(type) ? "kalandra_record_alloc_v1" : "kalandra_alloc_v1",
                          pointer, {ctx, builder.CreateMul(count, c64(outputBytes))});
        auto *allocated = builder.GetInsertBlock();
        builder.CreateCondBr(builder.CreateICmpEQ(call("kalandra_poll_v1", i64, {ctx}), c64(0)),
                             ready, abort);
        builder.SetInsertPoint(ready);
        auto *buffer = builder.CreatePHI(pointer, 2);
        buffer->addIncoming(llvm::ConstantPointerNull::get(builder.getPtrTy()), empty);
        buffer->addIncoming(data, allocated);
        Value *value = llvm::Constant::getNullValue(descriptor(type));
        value = builder.CreateInsertValue(value, buffer, {0});
        value = builder.CreateInsertValue(value, buffer, {1});
        value = builder.CreateInsertValue(value, count, {3, 0});
        value = builder.CreateInsertValue(
            value, builder.CreateSelect(builder.CreateICmpEQ(count, c64(0)), c64(0), c64(1)),
            {4, 0});
        builder.CreateStore(value, payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::MapCheck: {
        auto *value = builder.CreateLoad(descriptor(plan.slots[instruction.args[0]]),
                                         payload(instruction.args[0]));
        auto *index = builder.CreateAdd(builder.CreateLoad(i64, payload(instruction.args[1])),
                                        c64(instruction.target));
        builder.CreateStore(builder.CreateICmpULT(index, builder.CreateExtractValue(value, {3, 0})),
                            payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::MapLoad: {
        auto *value = builder.CreateLoad(descriptor(plan.slots[instruction.args[0]]),
                                         payload(instruction.args[0]));
        auto *index = builder.CreateAdd(builder.CreateLoad(i64, payload(instruction.args[1])),
                                        c64(instruction.target));
        auto *position =
            builder.CreateAdd(builder.CreateExtractValue(value, {2}),
                              builder.CreateMul(index, builder.CreateExtractValue(value, {4, 0})));
        auto *address = builder.CreateInBoundsGEP(scalarType(type),
                                                  builder.CreateExtractValue(value, {1}), position);
        builder.CreateStore(builder.CreateLoad(scalarType(type), address),
                            payload(instruction.output));
        builder.CreateBr(next);
        break;
      }
      case Code::MapStore: {
        auto *value = builder.CreateLoad(descriptor(type), payload(instruction.output));
        auto *index = builder.CreateAdd(builder.CreateLoad(i64, payload(instruction.args[0])),
                                        c64(instruction.target));
        auto *cell = scalarType(recordArrayElement(type));
        auto *address =
            builder.CreateInBoundsGEP(cell, builder.CreateExtractValue(value, {1}), index);
        builder.CreateStore(builder.CreateLoad(cell, payload(instruction.args[1])), address);
        check();
        break;
      }
      case Code::MapAdvance:
        builder.CreateStore(builder.CreateAdd(builder.CreateLoad(i64, payload(instruction.output)),
                                              c64(instruction.target)),
                            payload(instruction.output));
        check();
        break;
      case Code::MapSuccess:
        builder.CreateStore(builder.CreateNot(hasResultError(plan.slots[instruction.args[0]],
                                                             slot(instruction.args[0]))),
                            payload(instruction.output));
        builder.CreateBr(next);
        break;
      case Code::PropagateGroup: {
        if (!spawnFailure || instruction.group >= plan.groups)
          throw std::runtime_error("recursive call failure requires a scheduled group");
        call("kalandra_group_failure_v1", voidType, {task, group(instruction.group), spawnFailure});
        auto *failed = Block::Create(context, "call.failed", resume);
        builder.CreateCondBr(hasFailure(spawnFailure), failed, next);
        builder.SetInsertPoint(failed);
        call("kalandra_task_propagate_v1", voidType, {task, spawnFailure});
        builder.CreateBr(leave);
        break;
      }
      case Code::ReleaseGroup:
        builder.CreateCondBr(
            call("kalandra_task_release_group_v1", boolean, {task, group(instruction.group)}), next,
            abort);
        break;
      case Code::Copy:
        copyResult(type, slot(instruction.output), plan.slots[instruction.args[0]],
                   slot(instruction.args[0]));
        builder.CreateBr(next);
        break;
      case Code::Open: {
        call("kalandra_task_open_v1", voidType,
             {task,
              instruction.target == SIZE_MAX
                  ? static_cast<Value *>(llvm::ConstantPointerNull::get(builder.getPtrTy()))
                  : group(instruction.target),
              group(instruction.group)});
        auto *valid = builder.CreateLoad(
            i32, builder.CreateStructGEP(handleType, group(instruction.group), 0));
        builder.CreateCondBr(builder.CreateICmpNE(valid, c32(UINT32_MAX)), next, leave);
        break;
      }
      case Code::Spawn: {
        for (std::size_t i = 0; i < instruction.args.size(); ++i)
          copy(builder.CreateStructGEP(packets[instruction.target], staging[instruction.target], i),
               slot(instruction.args[i]), valueType(plan.slots[instruction.args[i]]));
        fillPacket(packet, instruction.target, staging[instruction.target], ctx);
        call("kalandra_task_spawn_v1", voidType,
             {task, group(instruction.group), entries[instruction.target], packet,
              payload(instruction.output)});
        auto *valid = builder.CreateLoad(
            i32, builder.CreateStructGEP(handleType, payload(instruction.output), 0));
        auto *rejected = Block::Create(context, "spawn.rejected", resume);
        builder.CreateCondBr(builder.CreateICmpNE(valid, c32(UINT32_MAX)), next, rejected);
        builder.SetInsertPoint(rejected);
        // A failed group may settle between dispatches. Preserve that failure
        // and join its children instead of awaiting an invalid task handle.
        call("kalandra_group_failure_v1", voidType, {task, group(instruction.group), spawnFailure});
        auto exit = groupExits.at(instruction.group);
        if (exit == SIZE_MAX)
          throw std::runtime_error("spawn has no owning group exit");
        builder.CreateCondBr(hasFailure(spawnFailure), blocks[exit], abort);
        break;
      }
      case Code::Await: {
        auto *handle = payload(instruction.args[0]);
        wait(call("kalandra_task_await_v1", boolean, {task, handle}));
        auto *header = builder.CreateStructGEP(valueType(type), slot(instruction.output), 0);
        call("kalandra_task_failure_v1", voidType, {task, handle, header});
        auto *read = Block::Create(context, "await.success", resume);
        builder.CreateCondBr(hasFailure(header), next, read);
        builder.SetInsertPoint(read);
        const auto &child = *planner.plans[instruction.target];
        const auto &childType = child.slots[child.result];
        if (key(childType) == key(type)) {
          auto *ok = call("kalandra_task_read_v1", boolean,
                          {task, handle, layout(childType), slot(instruction.output)});
          builder.CreateCondBr(ok, next, abort);
          break;
        }
        auto *observed = observations.at(instruction.target);
        auto *ok =
            call("kalandra_task_read_v1", boolean, {task, handle, layout(childType), observed});
        auto *copied = Block::Create(context, "await.observed", resume);
        builder.CreateCondBr(ok, copied, abort);
        builder.SetInsertPoint(copied);
        copyResult(type, slot(instruction.output), childType, observed);
        builder.CreateBr(next);
        break;
      }
      case Code::Fail:
      case Code::Try: {
        const auto input = instruction.args[0];
        const auto &source = plan.slots[input];
        const auto &target = plan.slots[instruction.target];
        const bool explicitFail = instruction.code == Code::Fail;
        auto *header =
            builder.CreateStructGEP(valueType(source), slot(input), explicitFail ? 1 : 0);
        auto *propagate = Block::Create(context, "try.propagate", resume);
        auto *success = Block::Create(context, "try.success", resume);
        if (explicitFail)
          builder.CreateBr(propagate);
        else
          builder.CreateCondBr(hasResultError(source, slot(input)), propagate, success);
        builder.SetInsertPoint(propagate);
        if (instruction.group != SIZE_MAX && target.errorRecord) {
          auto *accepted =
              call("kalandra_task_stop_group_v1", boolean, {task, group(instruction.group)});
          auto *store = Block::Create(context, "group.primary", resume);
          builder.CreateCondBr(accepted, store, blocks[instruction.jump]);
          builder.SetInsertPoint(store);
          if (explicitFail && source.builtin == BuiltinValue::Record)
            copyErrorValue(*target.errorRecord, application(target, slot(instruction.target)),
                           *source.record, payload(input));
          else if (explicitFail)
            injectTaskError(*target.errorRecord, application(target, slot(instruction.target)),
                            header);
          else
            copyErrors(target, slot(instruction.target), source, slot(input));
        } else if (instruction.group != SIZE_MAX) {
          call("kalandra_task_fail_group_v1", voidType, {task, group(instruction.group), header});
        } else if (explicitFail && source.builtin == BuiltinValue::Record) {
          if (!target.errorRecord || !source.record)
            throw std::runtime_error("application fail lost its declared error family");
          copyErrorValue(*target.errorRecord, application(target, slot(instruction.target)),
                         *source.record, payload(input));
        } else if (explicitFail) {
          if (target.errorRecord)
            injectTaskError(*target.errorRecord, application(target, slot(instruction.target)),
                            header);
          else
            copy(builder.CreateStructGEP(valueType(target), slot(instruction.target), 0), header,
                 failureType);
        } else
          copyErrors(target, slot(instruction.target), source, slot(input));
        builder.CreateBr(blocks[instruction.jump]);
        builder.SetInsertPoint(success);
        if (!explicitFail)
          copyResult(type, slot(instruction.output), source, slot(input));
        builder.CreateBr(next);
        break;
      }
      case Code::Catch: {
        auto input = instruction.args[0];
        auto *header = builder.CreateStructGEP(valueType(plan.slots[input]), slot(input), 0);
        auto *handle = Block::Create(context, "catch.error", resume);
        auto *success = Block::Create(context, "catch.success", resume);
        builder.CreateCondBr(hasResultError(plan.slots[input], slot(input)), handle, success);
        builder.SetInsertPoint(handle);
        if (plan.slots[input].errorRecord)
          copyErrorPayload(*plan.slots[input].errorRecord, payload(instruction.args[1]),
                           plan.slots[input], slot(input));
        else
          copy(payload(instruction.args[1]), header, failureType);
        builder.CreateBr(blocks[instruction.jump]);
        builder.SetInsertPoint(success);
        builder.CreateStore(llvm::Constant::getNullValue(valueType(type)),
                            slot(instruction.output));
        copy(builder.CreateStructGEP(valueType(type), slot(instruction.output), 1), payload(input),
             valueType(type)->getElementType(1));
        builder.CreateBr(blocks[instruction.target]);
        break;
      }
      case Code::Branch:
        builder.CreateCondBr(builder.CreateLoad(boolean, payload(instruction.args[0])), next,
                             blocks[instruction.jump]);
        break;
      case Code::Case: {
        auto subject = instruction.args[0];
        auto *tagAddress = plan.slots[subject].builtin == BuiltinValue::TaskError
                               ? builder.CreateStructGEP(failureType, payload(subject), 0)
                               : payload(subject);
        auto *tag = builder.CreateLoad(i32, tagAddress);
        builder.CreateCondBr(builder.CreateICmpEQ(tag, c32(instruction.target)), next,
                             instruction.jump == SIZE_MAX ? abort : blocks[instruction.jump]);
        break;
      }
      case Code::Jump:
        builder.CreateBr(blocks[instruction.jump]);
        break;
      case Code::Field: {
        if (std::holds_alternative<ErrorNarrow>(
                planner.input.expressions[instruction.source].operation)) {
          narrowErrorValue(type, payload(instruction.output), plan.slots[instruction.args[0]],
                           payload(instruction.args[0]), abort);
          check();
          break;
        }
        if (const auto *read = std::get_if<RecordFieldRead>(
                &planner.input.expressions[instruction.source].operation)) {
          const auto &subject = plan.slots[instruction.args[0]];
          if (subject.builtin != BuiltinValue::Record || !subject.record || subject.tensor ||
              subject.task || subject.fallible)
            throw std::runtime_error("task record projection requires a completed scalar record");
          auto *value = TaskRecordOps(planner.input, *taskTypes, module)
                            .project(builder, payload(instruction.args[0]), *subject.record,
                                     read->field, ctx, abort);
          builder.CreateStore(value, payload(instruction.output));
          check();
          break;
        }
        const auto &read =
            std::get<ErrorFieldRead>(planner.input.expressions[instruction.source].operation);
        auto *error = payload(instruction.args[0]);
        if (read.taskSlot)
          error = builder.CreateStructGEP(scalarType(plan.slots[instruction.args[0]]), error,
                                          *read.taskSlot);
        auto *code = loadFailure(error, 1);
        auto field = static_cast<ErrorField>(instruction.target);
        if (field == ErrorField::Kind) {
          auto *kind = builder.CreateSelect(
              builder.CreateICmpEQ(code, c64(1)), c32(3),
              builder.CreateSelect(
                  builder.CreateICmpEQ(code, c64(2)), c32(4),
                  builder.CreateSelect(
                      builder.CreateICmpEQ(code, c64(4)), c32(1),
                      builder.CreateSelect(builder.CreateICmpEQ(code, c64(5)), c32(2), c32(5)))));
          builder.CreateStore(kind, payload(instruction.output));
        } else {
          auto *textType = llvm::cast<llvm::StructType>(valueType(type)->getElementType(1));
          auto text = [&](const char *message) -> Value * {
            return llvm::ConstantStruct::get(
                textType, {llvm::ConstantPointerNull::get(builder.getPtrTy()),
                           llvm::cast<llvm::Constant>(builder.CreateGlobalString(message)),
                           c64(std::char_traits<char>::length(message))});
          };
          Value *message;
          if (field == ErrorField::Reason)
            message = text("task admission rejected");
          else
            message = builder.CreateSelect(
                builder.CreateICmpEQ(code, c64(1)), text("tensor shape mismatch"),
                builder.CreateSelect(
                    builder.CreateICmpEQ(code, c64(2)), text("allocation failed"),
                    builder.CreateSelect(builder.CreateICmpEQ(code, c64(4)),
                                         text("integer arithmetic fault"),
                                         builder.CreateSelect(builder.CreateICmpEQ(code, c64(5)),
                                                              text("index out of bounds"),
                                                              text("native execution fault")))));
          call("kalandra_string_literal_v1", voidType,
               {ctx, payload(instruction.output), builder.CreateExtractValue(message, {1}),
                builder.CreateExtractValue(message, {2})});
          check();
          break;
        }
        builder.CreateBr(next);
        break;
      }
      case Code::Join: {
        call("kalandra_task_close_v1", voidType, {task, group(instruction.group)});
        wait(call("kalandra_task_join_v1", boolean, {task, group(instruction.group)}));
        auto *header = builder.CreateStructGEP(valueType(type), slot(instruction.output), 0);
        // The primary typed error lives in this frame until all children settle.
        // Do not replace it with the scheduler's internal stop marker.
        if (type.errorRecord) {
          auto *settled = Block::Create(context, "group.outcome", resume);
          builder.CreateCondBr(hasResultError(type, slot(instruction.output)), next, settled);
          builder.SetInsertPoint(settled);
        }
        call("kalandra_group_failure_v1", voidType, {task, group(instruction.group), header});
        auto *success = Block::Create(context, "group.success", resume);
        builder.CreateCondBr(hasFailure(header), next, success);
        builder.SetInsertPoint(success);
        copyResult(type, slot(instruction.output), plan.slots[instruction.args[0]],
                   slot(instruction.args[0]));
        builder.CreateBr(next);
        break;
      }
      case Code::Label:
        builder.CreateBr(next);
        break;
      case Code::Call:
      case Code::Device:
      case Code::Transfer:
        throw std::runtime_error("CPU emitter received an unsupported placed operation");
      case Code::Return:
        if (synchronous) {
          if (resultType.fallible) {
            copyResult(resultType, resume->getArg(1), plan.slots[instruction.args[0]],
                       slot(instruction.args[0]));
            call("kalandra_free_v1", voidType, {ctx, frame});
            call("kalandra_call_leave_v1", voidType, {ctx});
            builder.CreateRetVoid();
          } else {
            auto *value = builder.CreateLoad(scalarType(resultType), payload(instruction.args[0]));
            call("kalandra_free_v1", voidType, {ctx, frame});
            call("kalandra_call_leave_v1", voidType, {ctx});
            builder.CreateRet(value);
          }
        } else {
          call("kalandra_task_complete_v1", voidType, {task, slot(instruction.args[0])});
          builder.CreateRetVoid();
        }
        break;
      }
    }
    if (!synchronous) {
      auto *resultLayout = layout(plan.slots[plan.result]);
      entries[id]->setInitializer(llvm::ConstantStruct::get(
          entryType, {c32(1), c64(id + 100), c64(size(packets[id])), c64(alignment(packets[id])),
                      c64(size(frameType)), c64(alignment(frameType)), construct, destroy, resume,
                      resultLayout}));
    }
  }
};
} // namespace
llvm::GlobalVariable *emitTaskValueLayout(const TypedModule &input, llvm::Module &module,
                                          const ValueType &type) {
  if (containsFunctionStorage(input, type))
    throw std::invalid_argument("standalone task layout emission requires non-callable storage");
  continuation::ModulePlan planner(input);
  return Emitter(planner, module).emitValueLayout(type);
}
llvm::GlobalVariable *emitTaskKernelValueLayout(const TypedModule &input,
                                                const TypedModule &kernels, llvm::Module &module,
                                                const ValueType &type) {
  if (!input.functions.empty() || !input.expressions.empty())
    throw std::invalid_argument("component task layout requires a schema-only source module");
  continuation::ModulePlan planner(input);
  planner.kernels = kernels;
  return Emitter(planner, module).emitValueLayout(type);
}
std::vector<llvm::GlobalVariable *> emitTaskEntries(continuation::ModulePlan &planner,
                                                    llvm::Module &module) {
  if (planner.plans.empty() || planner.plans.size() != planner.input.functions.size() ||
      std::ranges::any_of(planner.plans,
                          [](const auto &plan) { return !plan || plan->code.empty(); }))
    throw std::invalid_argument("task entry emission requires complete checked function plans");
  Emitter emitter(planner, module);
  emitter.emitEntries();
  return emitter.taskEntries();
}
std::expected<BackendOutput, std::string> lowerTaskNative(const TypedModule &input,
                                                          std::optional<std::string> testEntry,
                                                          ArithmeticMode arithmetic) {
  if (auto error = numericCapabilityError(input, NumericTarget::Cpu))
    return std::unexpected(*error);
  if (hasPlacement(input))
    return std::unexpected("placement-aware IR requires a placement-capable execution backend");
  try {
    auto planned = continuation::plan(input);
    if (!planned)
      return std::unexpected(planned.error());
    std::optional<continuation::ModulePlan> fallback;
    auto *planner = &*planned;
    auto kernels = lowerNative(planner->kernels, {}, {}, arithmetic, true);
    if (!kernels) {
      // Native subgraphs are an optimization, not a new source admission
      // requirement. A large synchronous body can still use budgeted checked
      // continuation storage when the native physical frame would be too big.
      auto legacy = continuation::plan(input, {.nativeSubgraphs = false});
      if (!legacy) return std::unexpected(legacy.error());
      fallback.emplace(std::move(*legacy));
      planner = &*fallback;
      kernels = lowerNative(planner->kernels, {}, {}, arithmetic, true);
    }
    if (!kernels)
      return kernels;
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(kernels->llvm, diagnostic, context);
    if (!module)
      return std::unexpected("cannot read verified kernel LLVM module");
    std::set<llvm::Function *> kernelFunctions;
    for (auto &fn : *module) if (!fn.isDeclaration()) kernelFunctions.insert(&fn);
    Emitter(*planner, *module).run(testEntry);
    for (auto &fn : *module)
      if (!fn.isDeclaration() && !kernelFunctions.contains(&fn))
        fn.addFnAttr("kalandra.task.frame");
    if (auto error = optimizeNativeCalls(module)) return std::unexpected(*error);
    std::string errors;
    llvm::raw_string_ostream stream(errors);
    if (llvm::verifyModule(*module, &stream))
      return std::unexpected("task LLVM verification failed: " + errors);
    BackendOutput result;
    result.mlir = std::move(kernels->mlir);
    {
      llvm::raw_string_ostream output(result.llvm);
      module->print(output, nullptr);
    }
    return result;
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
} // namespace kalandra
