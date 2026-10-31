#include "kalandra/Continuation.h"
#include "ErrorFamilies.h"
#include "RecordStorage.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
namespace kalandra::continuation {
namespace {
ValueType plain(ValueType type) {
  type.task = type.fallible = false;
  type.identity = 0;
  type.errorRecord.reset();
  return type;
}
std::vector<ValueId> operands(const TypedExpr &expression) {
  return std::visit(
      [](const auto &op) -> std::vector<ValueId> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, FunctionReference>)
          return op.captures;
        else if constexpr (std::is_same_v<T, FunctionInvoke>) {
          auto values = op.arguments;
          values.insert(values.begin(), op.callee);
          return values;
        } else if constexpr (std::is_same_v<T, FunctionCall> || std::is_same_v<T, RuntimeInput>)
          return op.arguments;
        else if constexpr (std::is_same_v<T, RecordValue>) {
          std::vector<ValueId> values;
          if (op.base)
            values.push_back(*op.base);
          for (const auto &field : op.fields)
            values.push_back(field.value);
          return values;
        } else if constexpr (std::is_same_v<T, RecordFieldRead> || std::is_same_v<T, ErrorNarrow>)
          return {op.input};
        else if constexpr (std::is_same_v<T, TensorLiteral>)
          return op.elements;
        else if constexpr (std::is_same_v<T, TensorTransform>) {
          std::vector<ValueId> result{op.input};
          if (op.epsilon) result.push_back(*op.epsilon);
          return result;
        } else if constexpr (std::is_same_v<T, TensorUnary> || std::is_same_v<T, ScalarUnary>)
          return {op.operand};
        else if constexpr (std::is_same_v<T, Matmul> || std::is_same_v<T, TensorBinary> ||
                           std::is_same_v<T, ScalarBinary> || std::is_same_v<T, ScalarCompare> ||
                           std::is_same_v<T, StringConcat> || std::is_same_v<T, ArrayConcat>)
          return {op.left, op.right};
        else if constexpr (std::is_same_v<T, ReduceSum> || std::is_same_v<T, CollectionQuery> ||
                           std::is_same_v<T, StringQuery> || std::is_same_v<T, NumericConvert> ||
                           std::is_same_v<T, TensorToArray> || std::is_same_v<T, ShapeCheck>)
          return {op.input};
        else if constexpr (std::is_same_v<T, StringSlice> || std::is_same_v<T, ArraySlice>)
          return {op.input, op.start, op.end};
        else if constexpr (std::is_same_v<T, ArrayIndex> || std::is_same_v<T, StringIndex>)
          return {op.input, op.index};
        else if constexpr (std::is_same_v<T, TensorIndex>) {
          auto result = op.indices;
          result.insert(result.begin(), op.input);
          return result;
        } else if constexpr (std::is_same_v<T, TensorReshape>) {
          auto result = op.dimensions;
          result.insert(result.begin(), op.input);
          return result;
        } else if constexpr (std::is_same_v<T, ArrayToTensor>) {
          auto result = op.dimensions.value_or(std::vector<ValueId>{});
          result.insert(result.begin(), op.input);
          return result;
        } else if constexpr (std::is_same_v<T, ArrayMap>) {
          auto result = op.captures;
          result.insert(result.begin(), op.input);
          return result;
        } else
          return {};
      },
      expression.operation);
}
class Builder : public ModulePlan {
  struct Boundary {
    Slot output;
    std::optional<std::size_t> group;
    std::vector<std::size_t> jumps;
  };
  struct Environment {
    std::unordered_map<ValueId, Slot> values;
    std::unordered_map<SymbolId, Slot> symbols;
    std::unordered_map<Slot, std::size_t> targets;
    Boundary *boundary = nullptr;
    std::optional<std::size_t> group;
  };

public:
  std::size_t totalSlots = 0;
  Options options;
  std::vector<bool> resumable, coordinating;
  std::optional<FunctionInvocationFlow> invocationFlow;
  std::vector<FunctionId> pending;
  std::vector<bool> scheduled;
  explicit Builder(const TypedModule &input, Options options)
      : ModulePlan(input), options(options), resumable(resumableFunctions(input)),
        coordinating(coordinatingFunctions(input)), invocationFlow(functionInvocationFlow(input)) {
    if (options.entry && *options.entry >= input.functions.size())
      throw std::runtime_error("continuation entry function is out of range");
    if (hasPlacement(input) && !options.preserveCalls)
      throw std::runtime_error("placement-aware plans require preserved target calls");
    kernels.source = input.source;
    kernels.records = input.records;
    plans.resize(input.functions.size());
    for (auto &plan : plans)
      plan = std::make_unique<Plan>();
    scheduled.resize(input.functions.size());
    for (FunctionId id = 0; id < input.functions.size(); ++id)
      if (!options.entry || *options.entry == id) {
        pending.push_back(id);
        scheduled[id] = true;
      }
    for (std::size_t index = 0; index < pending.size(); ++index) {
      const auto id = pending[index];
      auto &plan = *plans[id];
      std::vector<Slot> parameters;
      for (auto parameter : input.functions[id].parameters)
        parameters.push_back(slot(plan, input.symbols[parameter].type));
      plan.parameters = parameters;
      std::set<FunctionId> stack;
      plan.result = function(plan, id, parameters, stack);
      plan.code.push_back({Code::Return, 0, {plan.result}});
    }
    if (options.preserveCalls) {
      std::vector<unsigned> state(plans.size());
      auto visit = [&](auto &&self, FunctionId id, unsigned depth) -> void {
        if (state[id] == 1 || depth > 128)
          throw std::runtime_error("recursive resumable task entries are not implemented yet");
        if (state[id] == 2)
          return;
        state[id] = 1;
        for (const auto &op : plans[id]->code)
          if (op.code == Code::Spawn && resumable.at(op.target))
            self(self, op.target, depth + 1);
        state[id] = 2;
      };
      for (auto id : pending)
        visit(visit, id, 0);
    }
    TypedFunction main;
    main.name = "main";
    main.result = kernels.expressions.size();
    main.resultType = {};
    main.declaredResult = ValueType{};
    main.expressions = {main.result};
    main.body = {{{}, {}, main.result}};
    kernels.expressions.push_back({{}, {}, Float32Literal{0}});
    kernels.functions.push_back(std::move(main));
  }

private:
  Slot slot(Plan &plan, ValueType type) {
    if (++totalSlots > 300000 || plan.slots.size() >= 20000)
      throw std::runtime_error("continuation frame value limit exceeded");
    plan.slots.push_back(std::move(type));
    return plan.slots.size() - 1;
  }
  Slot function(Plan &plan, FunctionId id, const std::vector<Slot> &args,
                std::set<FunctionId> &stack, std::optional<std::size_t> ambient = {}) {
    if (!options.preserveCalls && options.nativeSubgraphs && nativeFunction(id)) {
      const auto target = importFunction(id);
      // A spawned synchronous function still needs its outer task descriptor,
      // but all of its computation belongs to one native subgraph/boundary.
      kernels.functions[target].name = "__step_native_" + std::to_string(id);
      const auto output = slot(plan, input.functions[id].resultType);
      plan.code.push_back({Code::Kernel, output, args, target, 0, 0,
                           input.functions[id].result});
      return output;
    }
    if (!stack.insert(id).second || stack.size() > 128)
      throw std::runtime_error("recursive calls are not implemented in task-enabled modules yet");
    Environment env;
    env.group = ambient;
    const auto &fn = input.functions[id];
    for (std::size_t i = 0; i < args.size(); ++i)
      env.symbols[fn.parameters[i]] = args[i];
    Boundary boundary{slot(plan, fn.resultType), {}, {}};
    env.boundary = &boundary;
    Slot value = boundary.output;
    for (const auto &statement : fn.body) {
      value = expression(plan, statement.value, env, stack);
      if (statement.binding)
        env.symbols[*statement.binding] = value;
    }
    plan.code.push_back({Code::Copy, boundary.output, {value}});
    const auto exit = plan.code.size();
    plan.code.push_back({Code::Label, 0, {}});
    for (auto jump : boundary.jumps)
      plan.code[jump].jump = exit;
    stack.erase(id);
    return boundary.output;
  }
  std::unordered_map<FunctionId, FunctionId> importedFunctions;
  std::unordered_map<ValueId, ValueId> importedValues;
  std::unordered_map<SymbolId, SymbolId> importedSymbols;
  bool nativeFunction(FunctionId id) const {
    return !resumable.at(id) &&
           std::ranges::none_of(input.functions[id].parameters, [&](auto parameter) {
             const auto &type = input.symbols[parameter].type;
             return type.fallible || type.task;
           });
  }
  SymbolId importSymbol(SymbolId id) {
    if (auto found = importedSymbols.find(id); found != importedSymbols.end())
      return found->second;
    auto symbol = input.symbols.at(id);
    symbol.owner = importedFunctions.at(symbol.owner);
    auto copied = kernels.symbols.size();
    kernels.symbols.push_back(std::move(symbol));
    importedSymbols[id] = copied;
    return copied;
  }
  TypedStatement importStatement(TypedStatement statement) {
    if (statement.binding)
      statement.binding = importSymbol(*statement.binding);
    statement.value = importValue(statement.value);
    return statement;
  }
  ValueId importValue(ValueId id) {
    if (auto found = importedValues.find(id); found != importedValues.end())
      return found->second;
    auto value = input.expressions.at(id);
    std::visit(
        [&](auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, SymbolRead>)
            op.symbol = importSymbol(op.symbol);
          else if constexpr (std::is_same_v<T, TensorTransform>) {
            op.input = importValue(op.input);
            if (op.epsilon) op.epsilon = importValue(*op.epsilon);
          } else if constexpr (std::is_same_v<T, ScalarUnary> || std::is_same_v<T, TensorUnary>)
            op.operand = importValue(op.operand);
          else if constexpr (std::is_same_v<T, ScalarBinary> || std::is_same_v<T, TensorBinary> ||
                             std::is_same_v<T, ScalarCompare> || std::is_same_v<T, Matmul> ||
                             std::is_same_v<T, StringConcat> || std::is_same_v<T, ArrayConcat>) {
            op.left = importValue(op.left);
            op.right = importValue(op.right);
          } else if constexpr (std::is_same_v<T, RecordValue>) {
            if (op.base)
              op.base = importValue(*op.base);
            for (auto &field : op.fields)
              field.value = importValue(field.value);
          } else if constexpr (std::is_same_v<T, RecordFieldRead> ||
                               std::is_same_v<T, ErrorNarrow>) {
            op.input = importValue(op.input);
          } else if constexpr (std::is_same_v<T, TensorLiteral>) {
            for (auto &element : op.elements)
              element = importValue(element);
          } else if constexpr (std::is_same_v<T, FunctionReference>) {
            op.function = importFunction(op.function);
            for (auto &capture : op.captures)
              capture = importValue(capture);
          } else if constexpr (std::is_same_v<T, FunctionInvoke>) {
            op.callee = importValue(op.callee);
            for (auto &argument : op.arguments)
              argument = importValue(argument);
          } else if constexpr (std::is_same_v<T, RuntimeInput>) {
            for (auto &argument : op.arguments) argument = importValue(argument);
          } else if constexpr (std::is_same_v<T, FunctionCall>) {
            op.function = importFunction(op.function);
            for (auto &argument : op.arguments)
              argument = importValue(argument);
          } else if constexpr (std::is_same_v<T, ArrayMap>) {
            op.function = importFunction(op.function);
            op.input = importValue(op.input);
            for (auto &capture : op.captures)
              capture = importValue(capture);
          } else if constexpr (std::is_same_v<T, StringSlice> || std::is_same_v<T, ArraySlice>) {
            op.input = importValue(op.input);
            op.start = importValue(op.start);
            op.end = importValue(op.end);
          } else if constexpr (std::is_same_v<T, ArrayIndex> || std::is_same_v<T, StringIndex>) {
            op.input = importValue(op.input);
            op.index = importValue(op.index);
          } else if constexpr (std::is_same_v<T, TensorIndex>) {
            op.input = importValue(op.input);
            for (auto &index : op.indices)
              index = importValue(index);
          } else if constexpr (std::is_same_v<T, TensorReshape>) {
            op.input = importValue(op.input);
            for (auto &axis : op.dimensions)
              axis = importValue(axis);
          } else if constexpr (std::is_same_v<T, ArrayToTensor>) {
            op.input = importValue(op.input);
            if (op.dimensions)
              for (auto &axis : *op.dimensions)
                axis = importValue(axis);
          } else if constexpr (std::is_same_v<T, NumericConvert> ||
                               std::is_same_v<T, TensorToArray> ||
                               std::is_same_v<T, CollectionQuery> ||
                               std::is_same_v<T, StringQuery> || std::is_same_v<T, ReduceSum> ||
                               std::is_same_v<T, ShapeCheck>)
            op.input = importValue(op.input);
          else if constexpr (std::is_same_v<T, ConditionalOperation>) {
            op.condition = importValue(op.condition);
            for (auto &statement : op.whenTrue)
              statement = importStatement(statement);
            for (auto &statement : op.whenFalse)
              statement = importStatement(statement);
            op.trueResult = importValue(op.trueResult);
            op.falseResult = importValue(op.falseResult);
          } else if constexpr (!(std::is_same_v<T, Float32Literal> ||
                                 std::is_same_v<T, Float64Literal> ||
                                 std::is_same_v<T, Int32Literal> ||
                                 std::is_same_v<T, Int64Literal> ||
                                 std::is_same_v<T, UInt32Literal> ||
                                 std::is_same_v<T, UInt64Literal> ||
                                 std::is_same_v<T, BoolLiteral> ||
                                 std::is_same_v<T, StringLiteral>))
            throw std::runtime_error("map callback requires synchronous native operations");
        },
        value.operation);
    const auto copied = kernels.expressions.size();
    kernels.expressions.push_back(std::move(value));
    importedValues[id] = copied;
    return copied;
  }
  FunctionId importFunction(FunctionId id) {
    if (auto found = importedFunctions.find(id); found != importedFunctions.end())
      return found->second;
    if (resumable.at(id))
      throw std::runtime_error("map callback cannot suspend");
    const auto copied = kernels.functions.size();
    importedFunctions[id] = copied;
    kernels.functions.emplace_back(); // Reserve before traversing recursive helper references.
    auto fn = input.functions.at(id);
    fn.name = "__map_callback_" + std::to_string(id);
    for (auto &parameter : fn.parameters)
      parameter = importSymbol(parameter);
    for (auto &value : fn.expressions)
      value = importValue(value);
    for (auto &statement : fn.body)
      statement = importStatement(statement);
    fn.result = importValue(fn.result);
    kernels.functions[copied] = std::move(fn);
    return copied;
  }
  ValueType mapCallableType(const ArrayMap &map) const {
    const auto &fn = input.functions.at(map.function);
    ValueType type;
    type.builtin = BuiltinValue::Function;
    type.callable = std::make_shared<const FunctionSignature>(
        FunctionSignature{{input.symbols.at(fn.parameters.at(0)).type}, fn.resultType});
    return type;
  }
  // Keep the MLIR loop ordinary; its extra argument carries a checked
  // synchronous entry rather than importing control flow into a pure kernel.
  FunctionId mapAdapter(const ArrayMap &map, SourceSpan span) {
    const auto id = kernels.functions.size();
    const auto callable = mapCallableType(map);
    TypedFunction fn;
    fn.name = "__map_control_" + std::to_string(id);
    fn.declaration = span;
    std::vector<ValueId> reads;
    for (const auto &type : {callable.callable->parameters[0], callable}) {
      const auto symbol = kernels.symbols.size();
      fn.parameters.push_back(symbol);
      kernels.symbols.push_back({"argument", type, span, id, true});
      reads.push_back(kernels.expressions.size());
      fn.expressions.push_back(reads.back());
      kernels.expressions.push_back({span, type, SymbolRead{symbol}});
    }
    fn.resultType = callable.callable->result;
    fn.declaredResult = fn.resultType;
    fn.result = kernels.expressions.size();
    fn.expressions.push_back(fn.result);
    fn.body = {{span, {}, fn.result}};
    kernels.expressions.push_back({span, fn.resultType, FunctionInvoke{reads[1], {reads[0]}}});
    kernels.functions.push_back(std::move(fn));
    return id;
  }
  FunctionId kernel(ValueId id) {
    if (auto found = kernelIds.find(id); found != kernelIds.end())
      return found->second;
    const auto &source = input.expressions[id];
    const auto *map = std::get_if<ArrayMap>(&source.operation);
    const bool controlMap = map && resumable.at(map->function);
    const auto callback =
        map ? (controlMap ? mapAdapter(*map, source.span) : importFunction(map->function))
            : FunctionId{};
    const auto *reference = std::get_if<FunctionReference>(&source.operation);
    const auto referenced = reference ? importFunction(reference->function) : FunctionId{};
    const auto *call = std::get_if<FunctionCall>(&source.operation);
    const auto called = call ? importFunction(call->function) : FunctionId{};
    std::vector<ValueType> parameters;
    if (controlMap)
      parameters = {plain(input.expressions[map->input].type), mapCallableType(*map)};
    else
      for (auto arg : operands(source))
        parameters.push_back(plain(input.expressions[arg].type));
    TypedFunction fn;
    const auto fnId = kernels.functions.size();
    fn.name = "__step_" + std::to_string(fnId);
    fn.declaration = source.span;
    std::vector<ValueId> reads;
    for (const auto &type : parameters) {
      const auto symbol = kernels.symbols.size();
      fn.parameters.push_back(symbol);
      kernels.symbols.push_back({"argument", type, source.span, fnId, true});
      reads.push_back(kernels.expressions.size());
      fn.expressions.push_back(reads.back());
      kernels.expressions.push_back({source.span, type, SymbolRead{symbol}});
    }
    auto op = source.operation;
    if (auto *input = std::get_if<RuntimeInput>(&op)) {
      input->arguments = reads;
    } else if (auto *call = std::get_if<FunctionCall>(&op)) {
      call->function = called;
      call->arguments = reads;
    } else if (auto *reference = std::get_if<FunctionReference>(&op)) {
      reference->function = referenced;
      reference->captures = reads;
    } else if (auto *call = std::get_if<FunctionInvoke>(&op)) {
      call->callee = reads[0];
      call->arguments.assign(reads.begin() + 1, reads.end());
    } else if (auto *record = std::get_if<RecordValue>(&op)) {
      std::size_t next = 0;
      if (record->base)
        record->base = reads[next++];
      for (auto &field : record->fields)
        field.value = reads[next++];
    } else if (auto *field = std::get_if<RecordFieldRead>(&op))
      field->input = reads[0];
    else if (auto *narrow = std::get_if<ErrorNarrow>(&op))
      narrow->input = reads[0];
    else if (auto *literal = std::get_if<TensorLiteral>(&op))
      literal->elements = reads;
    else if (auto *multiply = std::get_if<Matmul>(&op)) {
      multiply->left = reads[0];
      multiply->right = reads[1];
    } else if (auto *transform = std::get_if<TensorTransform>(&op)) {
      transform->input = reads[0];
      if (transform->epsilon) transform->epsilon = reads[1];
    } else if (auto *unary = std::get_if<TensorUnary>(&op)) {
      unary->operand = reads[0];
    } else if (auto *elementwise = std::get_if<TensorBinary>(&op)) {
      elementwise->left = reads[0];
      elementwise->right = reads[1];
    } else if (auto *reduce = std::get_if<ReduceSum>(&op))
      reduce->input = reads[0];
    else if (auto *comparison = std::get_if<ScalarCompare>(&op)) {
      comparison->left = reads[0];
      comparison->right = reads[1];
    } else if (auto *convert = std::get_if<NumericConvert>(&op))
      convert->input = reads[0];
    else if (auto *concat = std::get_if<StringConcat>(&op)) {
      concat->left = reads[0];
      concat->right = reads[1];
    } else if (auto *concat = std::get_if<ArrayConcat>(&op)) {
      concat->left = reads[0];
      concat->right = reads[1];
    } else if (auto *query = std::get_if<StringQuery>(&op))
      query->input = reads[0];
    else if (auto *read = std::get_if<StringIndex>(&op)) {
      read->input = reads[0];
      read->index = reads[1];
    } else if (auto *slice = std::get_if<StringSlice>(&op)) {
      slice->input = reads[0];
      slice->start = reads[1];
      slice->end = reads[2];
    } else if (auto *slice = std::get_if<ArraySlice>(&op)) {
      slice->input = reads[0];
      slice->start = reads[1];
      slice->end = reads[2];
    } else if (auto *query = std::get_if<CollectionQuery>(&op))
      query->input = reads[0];
    else if (auto *construction = std::get_if<TensorToArray>(&op))
      construction->input = reads[0];
    else if (auto *read = std::get_if<ArrayIndex>(&op)) {
      read->input = reads[0];
      read->index = reads[1];
    } else if (auto *read = std::get_if<TensorIndex>(&op)) {
      read->input = reads[0];
      read->indices.assign(reads.begin() + 1, reads.end());
    } else if (auto *reshape = std::get_if<TensorReshape>(&op)) {
      reshape->input = reads[0];
      reshape->dimensions.assign(reads.begin() + 1, reads.end());
    } else if (auto *construction = std::get_if<ArrayToTensor>(&op)) {
      construction->input = reads[0];
      if (construction->dimensions)
        construction->dimensions->assign(reads.begin() + 1, reads.end());
    } else if (auto *map = std::get_if<ArrayMap>(&op)) {
      map->input = reads[0];
      map->function = callback;
      map->captures.assign(reads.begin() + 1, reads.end());
    } else if (auto *shape = std::get_if<ShapeCheck>(&op))
      shape->input = reads[0];
    if (auto *unary = std::get_if<ScalarUnary>(&op))
      unary->operand = reads[0];
    else if (auto *binary = std::get_if<ScalarBinary>(&op)) {
      binary->left = reads[0];
      binary->right = reads[1];
    }
    fn.result = kernels.expressions.size();
    fn.resultType = plain(source.type);
    fn.declaredResult = fn.resultType;
    fn.expressions.push_back(fn.result);
    fn.body = {{source.span, {}, fn.result}};
    kernels.expressions.push_back({source.span, fn.resultType, std::move(op)});
    kernels.functions.push_back(std::move(fn));
    kernelIds[id] = fnId;
    return fnId;
  }
  void parallelMap(Plan &plan, ValueId id, Slot output, const ArrayMap &map, Environment &env,
                   std::set<FunctionId> &stack) {
    if (options.preserveCalls)
      throw std::runtime_error("parallelMap requires ordinary CPU execution");
    const auto inputSlot = expression(plan, map.input, env, stack);
    std::vector<Slot> captures;
    for (auto capture : map.captures)
      captures.push_back(expression(plan, capture, env, stack));
    const auto buffer = slot(plan, plain(input.expressions[id].type));
    ValueType integer;
    integer.element = NumericType::Int64;
    const auto index = slot(plan, integer);
    ValueType boolean;
    boolean.builtin = BuiltinValue::Bool;
    const auto active = slot(plan, boolean);
    plan.code.push_back({Code::MapInit, buffer, {inputSlot}, 0, 0, 0, id});
    plan.code.push_back({Code::Copy, output, {buffer}});
    const auto loop = plan.code.size();
    plan.code.push_back({Code::MapCheck, active, {inputSlot, index}});
    const auto empty = plan.code.size();
    plan.code.push_back({Code::Branch, 0, {active}});
    const auto group = plan.groups++;
    plan.code.push_back({Code::Open, 0, {}, env.group.value_or(SIZE_MAX), group, 0, id});
    std::vector<Slot> handles;
    constexpr unsigned width = 8;
    const auto &fn = input.functions[map.function];
    const auto elementType = recordArrayElement(input.expressions[map.input].type);
    auto childResult = fn.declaredResult.value_or(fn.resultType);
    auto observedType = childResult;
    observedType.fallible = true;
    observedType.errorRecord = input.expressions[id].type.errorRecord;
    std::vector<std::size_t> errors;
    for (unsigned lane = 0; lane < width; ++lane) {
      plan.code.push_back({Code::MapCheck, active, {inputSlot, index}, lane});
      const auto skip = plan.code.size();
      plan.code.push_back({Code::Branch, 0, {active}});
      const auto element = slot(plan, elementType);
      plan.code.push_back({Code::MapLoad, element, {inputSlot, index}, lane, 0, 0, id});
      auto taskType = childResult;
      taskType.task = true;
      const auto handle = slot(plan, taskType);
      handles.push_back(handle);
      std::vector<Slot> arguments{element};
      arguments.insert(arguments.end(), captures.begin(), captures.end());
      plan.code.push_back({Code::Spawn, handle, arguments, map.function, group, 0, id});
      plan.code[skip].jump = plan.code.size();
      plan.code.push_back({Code::Label, 0, {}});
    }
    for (unsigned lane = 0; lane < width; ++lane) {
      plan.code.push_back({Code::MapCheck, active, {inputSlot, index}, lane});
      const auto skip = plan.code.size();
      plan.code.push_back({Code::Branch, 0, {active}});
      const auto observed = slot(plan, observedType);
      const auto element = slot(plan, plain(childResult));
      plan.code.push_back({Code::Await, observed, {handles[lane]}, map.function});
      errors.push_back(plan.code.size());
      plan.code.push_back({Code::Try, element, {observed}, output, group, 0, id});
      plan.code.push_back({Code::MapStore, buffer, {index, element}, lane, 0, 0, id});
      plan.code[skip].jump = plan.code.size();
      plan.code.push_back({Code::Label, 0, {}});
    }
    const auto join = plan.code.size();
    for (auto error : errors)
      plan.code[error].jump = join;
    plan.code.push_back({Code::Join, output, {buffer}, 0, group, 0, id});
    plan.code.push_back({Code::ReleaseGroup, 0, {}, 0, group, 0, id});
    plan.code.push_back({Code::MapSuccess, active, {output}});
    const auto failed = plan.code.size();
    plan.code.push_back({Code::Branch, 0, {active}});
    plan.code.push_back({Code::MapAdvance, index, {}, width});
    plan.code.push_back({Code::Jump, 0, {}, 0, 0, loop});
    plan.code[empty].jump = plan.code[failed].jump = plan.code.size();
    plan.code.push_back({Code::Label, 0, {}});
  }
  void indirectCall(Plan &plan, ValueId id, Slot output, const FunctionInvoke &call,
                    Environment &env, std::set<FunctionId> &stack) {
    if (options.preserveCalls || !invocationFlow)
      throw std::runtime_error("resumable callable dispatch requires proven ordinary CPU targets");
    if (invocationFlow->unknown.at(id)) {
      if (invocationFlow->reachable.at(id))
        throw std::runtime_error(
            "resumable callable dispatch requires proven ordinary CPU targets");
      // Uncalled generic library bodies have no incoming callback proof. Keep a
      // checked rejection entry; never guess a target or issue a native call.
      plan.code.push_back({Code::InvalidCallable, 0, {}});
      return;
    }
    const auto callee = expression(plan, call.callee, env, stack);
    std::vector<Slot> arguments;
    for (auto arg : call.arguments)
      arguments.push_back(expression(plan, arg, env, stack));
    std::vector<std::size_t> exits;
    bool synchronous = false;
    for (auto target : invocationFlow->targets.at(id)) {
      if (!coordinating.at(target)) {
        synchronous = true;
        continue;
      }
      const auto &fn = input.functions[target];
      ValueType boolean;
      boolean.builtin = BuiltinValue::Bool;
      const auto selected = slot(plan, boolean);
      plan.code.push_back({Code::CallableTarget, selected, {callee}, target});
      const auto skip = plan.code.size();
      plan.code.push_back({Code::Branch, 0, {selected}});
      const auto copied = slot(plan, input.expressions[call.callee].type);
      std::vector<Slot> captures{callee};
      auto args = arguments;
      for (std::size_t n = fn.parameters.size() - fn.captureCount; n < fn.parameters.size(); ++n) {
        auto capture = slot(plan, input.symbols[fn.parameters[n]].type);
        captures.push_back(capture);
        args.push_back(capture);
      }
      plan.code.push_back({Code::CallableCaptures, copied, captures, target});
      auto value = stack.contains(target) ? recursiveCall(plan, id, target, args, env)
                                          : function(plan, target, args, stack, env.group);
      plan.code.push_back({Code::Copy, output, {value}});
      exits.push_back(plan.code.size());
      plan.code.push_back({Code::Jump, 0, {}});
      plan.code[skip].jump = plan.code.size();
      plan.code.push_back({Code::Label, 0, {}});
    }
    if (synchronous) {
      const auto checked = slot(plan, input.expressions[call.callee].type);
      plan.code.push_back({Code::CallableSyncGuard, checked, {callee}});
      std::vector<Slot> args{checked};
      args.insert(args.end(), arguments.begin(), arguments.end());
      if (input.expressions[id].type.fallible)
        plan.code.push_back({Code::FunctionInvoke, output, args, 0, 0, 0, id});
      else
        plan.code.push_back({Code::Kernel, output, args, kernel(id), 0, 0, id});
    } else
      plan.code.push_back({Code::InvalidCallable, 0, {}});
    for (auto exit : exits)
      plan.code[exit].jump = plan.code.size();
    plan.code.push_back({Code::Label, 0, {}});
  }
  std::size_t builtinPlan(ValueId id) {
    auto plan = std::make_unique<Plan>();
    std::vector<Slot> args;
    for (auto arg : operands(input.expressions[id]))
      args.push_back(slot(*plan, input.expressions[arg].type));
    plan->parameters = args;
    if (const auto *map = std::get_if<ArrayMap>(&input.expressions[id].operation);
        (map && (map->parallel || resumable.at(map->function))) ||
        std::holds_alternative<FunctionInvoke>(input.expressions[id].operation)) {
      // Evaluate indirect calls in the child, after the callee and arguments
      // have crossed the checked copy boundary. Preserve their error channel.
      // Control-flow maps instead construct their callback from copied captures.
      Environment env;
      const auto inputs = operands(input.expressions[id]);
      for (std::size_t i = 0; i < inputs.size(); ++i)
        env.values[inputs[i]] = args[i];
      std::set<FunctionId> stack;
      plan->result = expression(*plan, id, env, stack);
    } else {
      plan->result = slot(*plan, plain(input.expressions[id].type));
      plan->code.push_back({Code::Kernel, plan->result, args, kernel(id), 0, 0, id});
    }
    plan->code.push_back({Code::Return, 0, {plan->result}});
    plans.push_back(std::move(plan));
    return plans.size() - 1;
  }
  bool constant(ValueId id) const {
    const auto &op = input.expressions[id].operation;
    if ((std::holds_alternative<BoolLiteral>(op) || std::holds_alternative<Float64Literal>(op) ||
         std::holds_alternative<Float32Literal>(op)) ||
        (std::holds_alternative<UInt64Literal>(op) || std::holds_alternative<Int64Literal>(op) ||
         std::holds_alternative<Int32Literal>(op) || std::holds_alternative<UInt32Literal>(op)))
      return true;
    const auto *unary = std::get_if<ScalarUnary>(&op);
    return unary && constant(unary->operand);
  }
  Slot recursiveCall(Plan &plan, ValueId id, FunctionId target, const std::vector<Slot> &args,
                     Environment &env) {
    // A coordinating backedge suspends in a bounded child record, rather than
    // expanding its body or recursing on the native worker stack.
    if (!scheduled.at(target)) {
      scheduled[target] = true;
      pending.push_back(target);
    }
    const auto result = input.expressions.at(id).type;
    auto handleType = input.functions.at(target).resultType;
    handleType.task = true;
    auto observedType = result;
    observedType.fallible = true;
    const auto output = slot(plan, result), handle = slot(plan, handleType);
    const auto observed = slot(plan, observedType), joined = slot(plan, observedType);
    const auto group = plan.groups++;
    plan.code.push_back({Code::Open, 0, {}, env.group.value_or(SIZE_MAX), group, 0, id});
    plan.code.push_back({Code::Spawn, handle, args, target, group, 0, id});
    plan.code.push_back({Code::Await, observed, {handle}, target, 0, 0, id});
    plan.code.push_back({Code::Join, joined, {observed}, 0, group, 0, id});
    // Scheduler failure is an execution failure of this call, not an invented
    // member of the callee's declared application-error family.
    if (!result.fallible || !errorSubset(input, {}, result.errorRecord))
      plan.code.push_back({Code::PropagateGroup, 0, {}, 0, group, 0, id});
    plan.code.push_back({Code::Copy, output, {joined}});
    plan.code.push_back({Code::ReleaseGroup, 0, {}, 0, group});
    return output;
  }
  Slot expression(Plan &plan, ValueId id, Environment &env, std::set<FunctionId> &stack) {
    if (auto found = env.values.find(id); found != env.values.end())
      return found->second;
    const auto &node = input.expressions[id];
    if (const auto *read = std::get_if<SymbolRead>(&node.operation))
      return env.symbols.at(read->symbol);
    if (const auto *call = std::get_if<FunctionCall>(&node.operation)) {
      std::vector<Slot> args;
      for (auto arg : call->arguments)
        args.push_back(expression(plan, arg, env, stack));
      // Synchronous native subgraphs share the ordinary checked call ABI,
      // including recursive/function-value calls and their cancellation boundary.
      // A task owns only the outer continuation; it must not impose a heap
      // continuation frame on every non-suspending source call within that work.
      if (!options.preserveCalls && options.nativeSubgraphs && nativeFunction(call->function)) {
        const auto value = slot(plan, node.type);
        plan.code.push_back({Code::Kernel, value, args, kernel(id), 0, 0, id});
        env.values[id] = value;
        return value;
      }
      if (!options.preserveCalls && stack.contains(call->function) &&
          coordinating.at(call->function)) {
        auto value = recursiveCall(plan, id, call->function, args, env);
        env.values[id] = value;
        return value;
      }
      if (!options.preserveCalls && stack.contains(call->function) &&
          !coordinating.at(call->function)) {
        if (input.functions.at(call->function).captureCount)
          throw std::runtime_error("direct recursive calls require a named function entry");
        auto value = slot(plan, node.type);
        plan.code.push_back({Code::SynchronousCall, value, args, call->function, 0, 0, id});
        env.values[id] = value;
        return value;
      }
      const bool preserve = options.preserveCalls && !resumable.at(call->function);
      auto value =
          preserve ? slot(plan, node.type) : function(plan, call->function, args, stack, env.group);
      if (preserve)
        plan.code.push_back({Code::Call, value, args, call->function, 0, 0, id});
      env.values[id] = value;
      return value;
    }
    auto output = slot(plan, node.type);
    if (const auto *map = std::get_if<ArrayMap>(&node.operation); map && map->parallel) {
      parallelMap(plan, id, output, *map, env, stack);
    } else if (const auto *reference = std::get_if<kalandra::FunctionReference>(&node.operation);
               reference && resumable.at(reference->function)) {
      std::vector<Slot> captures;
      for (auto capture : reference->captures)
        captures.push_back(expression(plan, capture, env, stack));
      plan.code.push_back(
          {Code::FunctionReference, output, captures, reference->function, 0, 0, id});
    } else if (const auto *map = std::get_if<ArrayMap>(&node.operation);
               map && resumable.at(map->function)) {
      if (options.preserveCalls)
        throw std::runtime_error("control-flow maps require CPU continuation lowering");
      const auto inputSlot = expression(plan, map->input, env, stack);
      std::vector<Slot> captures;
      for (auto capture : map->captures)
        captures.push_back(expression(plan, capture, env, stack));
      const auto callable = slot(plan, mapCallableType(*map));
      plan.code.push_back({Code::FunctionReference, callable, captures, map->function, 0, 0, id});
      plan.code.push_back({Code::Kernel, output, {inputSlot, callable}, kernel(id), 0, 0, id});
    } else if (const auto *call = std::get_if<kalandra::FunctionInvoke>(&node.operation);
               call && invocationFlow &&
               std::ranges::any_of(invocationFlow->targets.at(id),
                                   [&](auto target) { return coordinating.at(target); })) {
      indirectCall(plan, id, output, *call, env, stack);
    } else if (const auto *call = std::get_if<kalandra::FunctionInvoke>(&node.operation);
               call && node.type.fallible) {
      std::vector<Slot> args{expression(plan, call->callee, env, stack)};
      for (auto argument : call->arguments)
        args.push_back(expression(plan, argument, env, stack));
      plan.code.push_back({Code::FunctionInvoke, output, args, 0, 0, 0, id});
    } else if (const auto *block = std::get_if<ParallelOperation>(&node.operation)) {
      const auto group = plan.groups++;
      Boundary boundary{output, group, {}};
      auto nested = env;
      nested.boundary = &boundary;
      nested.group = group;
      plan.code.push_back({Code::Open, 0, {}, env.group.value_or(SIZE_MAX), group, 0, id});
      Slot value = output;
      for (const auto &statement : block->body) {
        value = expression(plan, statement.value, nested, stack);
        if (statement.binding)
          nested.symbols[*statement.binding] = value;
      }
      const auto exit = plan.code.size();
      plan.code.push_back({Code::Join, output, {value}, 0, group, 0, id});
      for (auto jump : boundary.jumps)
        plan.code[jump].jump = exit;
    } else if (const auto *device = std::get_if<DeviceSelection>(&node.operation)) {
      plan.code.push_back(
          {Code::Device, output, {}, device->device ? *device->device : SIZE_MAX, 0, 0, id});
    } else if (const auto *transfer = std::get_if<Transfer>(&node.operation)) {
      if (!env.group)
        throw std::runtime_error("transfer plan requires an owning group");
      auto input = expression(plan, transfer->input, env, stack);
      auto target = expression(plan, transfer->target, env, stack);
      plan.code.push_back({Code::Transfer, output, {input, target}, 0, *env.group, 0, id});
      env.targets[output] = SIZE_MAX;
    } else if (const auto *task = std::get_if<TaskOperation>(&node.operation)) {
      if (task->prefix == TaskPrefix::Spawn) {
        const auto &invocation = input.expressions[task->operand];
        std::vector<ValueId> operands;
        std::size_t target;
        if (const auto *call = std::get_if<FunctionCall>(&invocation.operation)) {
          operands = call->arguments;
          target = call->function;
          if ((!options.preserveCalls || resumable.at(target)) && !scheduled.at(target)) {
            scheduled[target] = true;
            pending.push_back(target);
          }
          if (options.preserveCalls && resumable.at(target)) {
            if (task->placement && input.expressions.at(*task->placement).type.device)
              throw std::runtime_error("resumable helpers require CPU placement; GPU placements "
                                       "require numeric kernels");
          }
        } else {
          if (options.preserveCalls)
            throw std::runtime_error("mixed spawn requires a resolved function entry");
          operands = kalandra::continuation::operands(invocation);
          target = builtinPlan(task->operand);
        }
        std::vector<Slot> args;
        for (auto arg : operands)
          args.push_back(expression(plan, arg, env, stack));
        if (!env.group)
          throw std::runtime_error("spawn plan requires an owning group");
        std::optional<Slot> placement;
        if (task->placement)
          placement = expression(plan, *task->placement, env, stack);
        plan.code.push_back({Code::Spawn, output, args, target, *env.group, 0, id, placement});
        env.targets[output] = target;
      } else {
        auto operand = expression(plan, task->operand, env, stack);
        if (task->prefix == TaskPrefix::Await)
          plan.code.push_back({Code::Await, output, {operand}, env.targets.at(operand)});
        else {
          const auto index = plan.code.size();
          plan.code.push_back({Code::Try,
                               output,
                               {operand},
                               env.boundary->output,
                               env.boundary->group.value_or(SIZE_MAX)});
          env.boundary->jumps.push_back(index);
        }
      }
    } else if (const auto *handler = std::get_if<CatchOperation>(&node.operation)) {
      auto operand = expression(plan, handler->operand, env, stack);
      const auto error = slot(plan, input.symbols[handler->parameter].type);
      auto nested = env;
      nested.symbols[handler->parameter] = error;
      Boundary boundary{output, {}, {}};
      nested.boundary = &boundary;
      const auto dispatch = plan.code.size();
      plan.code.push_back({Code::Catch, output, {operand, error}});
      plan.code[dispatch].jump = plan.code.size();
      Slot result = output;
      for (const auto &statement : handler->body) {
        result = expression(plan, statement.value, nested, stack);
        if (statement.binding)
          nested.symbols[*statement.binding] = result;
      }
      plan.code.push_back({Code::Copy, output, {result}});
      const auto exit = plan.code.size();
      plan.code.push_back({Code::Label, 0, {}});
      plan.code[dispatch].target = exit;
      for (auto jump : boundary.jumps)
        plan.code[jump].jump = exit;
    } else if (const auto *guard = std::get_if<GuardOperation>(&node.operation)) {
      auto condition = expression(plan, guard->condition, env, stack);
      const auto dispatch = plan.code.size();
      plan.code.push_back({Code::Branch, output, {condition}});
      auto branch = env;
      for (const auto &statement : guard->body) {
        auto value = expression(plan, statement.value, branch, stack);
        if (statement.binding)
          branch.symbols[*statement.binding] = value;
      }
      plan.code[dispatch].jump = plan.code.size();
      plan.code.push_back({Code::Label, 0, {}});
    } else if (const auto *choice = std::get_if<ConditionalOperation>(&node.operation)) {
      auto condition = expression(plan, choice->condition, env, stack);
      const auto dispatch = plan.code.size();
      plan.code.push_back({Code::Branch, output, {condition}});
      for (unsigned index = 0; index < 2; ++index) {
        if (index == 1)
          plan.code[dispatch].jump = plan.code.size();
        auto branch = env;
        for (const auto &statement : index == 0 ? choice->whenTrue : choice->whenFalse) {
          auto value = expression(plan, statement.value, branch, stack);
          if (statement.binding)
            branch.symbols[*statement.binding] = value;
        }
        auto result =
            expression(plan, index == 0 ? choice->trueResult : choice->falseResult, branch, stack);
        plan.code.push_back({Code::Copy, output, {result}});
        if (index == 0) {
          plan.code[dispatch].target = plan.code.size();
          plan.code.push_back({Code::Jump, 0, {}});
        }
      }
      plan.code[plan.code[dispatch].target].jump = plan.code.size();
      plan.code.push_back({Code::Label, 0, {}});
    } else if (const auto *match = std::get_if<MatchOperation>(&node.operation)) {
      auto subject = expression(plan, match->subject, env, stack);
      std::vector<std::size_t> exits;
      std::optional<std::size_t> previous;
      for (const auto &arm : match->cases) {
        if (previous)
          plan.code[*previous].jump = plan.code.size();
        previous.reset();
        if (arm.tag) {
          previous = plan.code.size();
          plan.code.push_back({Code::Case, output, {subject}, *arm.tag});
        }
        auto branch = env;
        Slot result = output;
        for (const auto &statement : arm.body) {
          result = expression(plan, statement.value, branch, stack);
          if (statement.binding)
            branch.symbols[*statement.binding] = result;
        }
        plan.code.push_back({Code::Copy, output, {result}});
        exits.push_back(plan.code.size());
        plan.code.push_back({Code::Jump, 0, {}});
      }
      // A corrupt internal tag is a checked fault, never an implicit wildcard.
      if (previous)
        plan.code[*previous].jump = SIZE_MAX;
      const auto exit = plan.code.size();
      plan.code.push_back({Code::Label, 0, {}});
      for (auto jump : exits)
        plan.code[jump].jump = exit;
    } else if (const auto *field = std::get_if<RecordFieldRead>(&node.operation);
               field && !options.preserveCalls) {
      auto subject = expression(plan, field->input, env, stack);
      plan.code.push_back({Code::Field, output, {subject}, field->field, 0, 0, id});
    } else if (const auto *narrow = std::get_if<ErrorNarrow>(&node.operation);
               narrow && !options.preserveCalls) {
      auto subject = expression(plan, narrow->input, env, stack);
      plan.code.push_back({Code::Field, output, {subject}, 0, 0, 0, id});
    } else if (const auto *field = std::get_if<ErrorFieldRead>(&node.operation)) {
      auto subject = expression(plan, field->subject, env, stack);
      plan.code.push_back(
          {Code::Field, output, {subject}, static_cast<std::size_t>(field->field), 0, 0, id});
    } else if (const auto *failure = std::get_if<FailOperation>(&node.operation)) {
      auto error = expression(plan, failure->error, env, stack);
      const auto index = plan.code.size();
      plan.code.push_back({Code::Fail,
                           output,
                           {error},
                           env.boundary->output,
                           env.boundary->group.value_or(SIZE_MAX)});
      env.boundary->jumps.push_back(index);
    } else if ((std::holds_alternative<BoolLiteral>(node.operation) ||
                std::holds_alternative<Float64Literal>(node.operation) ||
                std::holds_alternative<Float32Literal>(node.operation)) ||
               (std::holds_alternative<UInt64Literal>(node.operation) ||
                std::holds_alternative<Int64Literal>(node.operation) ||
                std::holds_alternative<Int32Literal>(node.operation) ||
                std::holds_alternative<UInt32Literal>(node.operation)))
      plan.code.push_back({Code::Literal, output, {}, 0, 0, 0, id});
    else if ((isInteger(node.type.element) || node.type.element == NumericType::Float64) &&
             (std::holds_alternative<ScalarUnary>(node.operation) ||
              std::holds_alternative<ScalarBinary>(node.operation))) {
      std::vector<Slot> args;
      for (auto arg : operands(node))
        args.push_back(expression(plan, arg, env, stack));
      plan.code.push_back({Code::Kernel, output, args,
                           options.preserveCalls ? 0 : kernel(id), 0, 0, id});
    } else if (const auto *unary = std::get_if<ScalarUnary>(&node.operation)) {
      auto arg = expression(plan, unary->operand, env, stack);
      plan.code.push_back({Code::Unary, output, {arg}, 0, 0, 0, id});
    } else if (const auto *binary = std::get_if<ScalarBinary>(&node.operation)) {
      auto a = expression(plan, binary->left, env, stack);
      auto b = expression(plan, binary->right, env, stack);
      plan.code.push_back({Code::Binary, output, {a, b}, 0, 0, 0, id});
    } else {
      std::vector<Slot> args;
      const auto *literal = std::get_if<TensorLiteral>(&node.operation);
      // Target-preserved constants are embedded tensor data, not thousands of
      // live continuation values. Nonconstant leaves still evaluate in order.
      const bool embedded =
          options.preserveCalls && literal &&
          std::ranges::all_of(literal->elements, [&](auto value) { return constant(value); });
      if (!embedded)
        for (auto arg : operands(node))
          args.push_back(expression(plan, arg, env, stack));
      plan.code.push_back(
          {Code::Kernel, output, args, options.preserveCalls ? 0 : kernel(id), 0, 0, id});
    }
    env.values[id] = output;
    return output;
  }
};
// Creation/capture is not execution. Only indirect invocation adds may-call
// edges; parameter and returned-function flow is shared with worker proofs.
void indirectEffects(const TypedModule &input, std::vector<std::vector<FunctionId>> &callers,
                     std::vector<bool> &result) {
  if (!std::ranges::any_of(input.expressions, [](const auto &value) {
        return std::holds_alternative<FunctionInvoke>(value.operation);
      }))
    return;
  const auto flow = functionInvocationFlow(input);
  for (FunctionId id = 0; id < input.functions.size(); ++id)
    for (auto value : input.functions[id].expressions)
      if (std::holds_alternative<FunctionInvoke>(input.expressions.at(value).operation)) {
        if (!flow) {
          // Exhaustion must never certify an incomplete call graph as synchronous.
          result[id] = true;
          continue;
        }
        for (auto target : flow->targets.at(value))
          callers.at(target).push_back(id);
      }
}
} // namespace
std::vector<bool> resumableFunctions(const TypedModule &input) {
  // Indirect synchronous entries already encapsulate their expected-error
  // control flow. Only actual coordination crosses that calling boundary.
  auto result = coordinatingFunctions(input);
  std::vector<std::vector<FunctionId>> callers(input.functions.size());
  std::vector<FunctionId> pending;
  for (FunctionId id = 0; id < input.functions.size(); ++id) {
    result[id] = result[id] || input.functions[id].resultType.fallible;
    for (auto value : input.functions[id].expressions) {
      const auto &op = input.expressions.at(value).operation;
      if (const auto *call = std::get_if<FunctionCall>(&op))
        callers.at(call->function).push_back(id);
      if (const auto *reference = std::get_if<FunctionReference>(&op))
        callers.at(reference->function).push_back(id);
      if (const auto *map = std::get_if<ArrayMap>(&op))
        callers.at(map->function).push_back(id);
      if (input.expressions[value].type.fallible || std::holds_alternative<TaskOperation>(op) ||
          std::holds_alternative<ParallelOperation>(op) ||
          std::holds_alternative<CatchOperation>(op) ||
          std::holds_alternative<MatchOperation>(op) ||
          std::holds_alternative<GuardOperation>(op) || std::holds_alternative<FailOperation>(op) ||
          std::holds_alternative<ErrorFieldRead>(op) ||
          std::holds_alternative<DeviceSelection>(op) || std::holds_alternative<Transfer>(op))
        result[id] = true;
    }
    if (result[id])
      pending.push_back(id);
  }
  while (!pending.empty()) {
    auto id = pending.back();
    pending.pop_back();
    for (auto caller : callers[id])
      if (!result[caller]) {
        result[caller] = true;
        pending.push_back(caller);
      }
  }
  return result;
}
std::vector<bool> coordinatingFunctions(const TypedModule &input) {
  std::vector<bool> result(input.functions.size());
  std::vector<std::vector<FunctionId>> callers(input.functions.size());
  std::vector<FunctionId> pending;
  indirectEffects(input, callers, result);
  for (FunctionId id = 0; id < input.functions.size(); ++id) {
    for (auto value : input.functions[id].expressions) {
      const auto &op = input.expressions.at(value).operation;
      if (const auto *call = std::get_if<FunctionCall>(&op))
        callers.at(call->function).push_back(id);
      if (const auto *map = std::get_if<ArrayMap>(&op))
        callers.at(map->function).push_back(id);
      const auto *task = std::get_if<TaskOperation>(&op);
      const auto *map = std::get_if<ArrayMap>(&op);
      if ((map && map->parallel) || (task && task->prefix != TaskPrefix::Try) ||
          std::holds_alternative<ParallelOperation>(op) ||
          std::holds_alternative<DeviceSelection>(op) || std::holds_alternative<Transfer>(op))
        result[id] = true;
    }
    if (result[id])
      pending.push_back(id);
  }
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    for (auto caller : callers[id])
      if (!result[caller]) {
        result[caller] = true;
        pending.push_back(caller);
      }
  }
  return result;
}
std::expected<ModulePlan, std::string> plan(const TypedModule &input, Options options) {
  if (!input.records.empty()) {
    auto storage = planRecordStorage(input);
    if (!storage)
      return std::unexpected(storage.error().message);
    if (hasPlacement(input))
      return std::unexpected("records are not implemented in placement-aware continuations");
    if (auto error = numericCapabilityError(input, NumericTarget::Cpu))
      return std::unexpected(*error);
  }
  try {
    Builder builder(input, options);
    return std::move(static_cast<ModulePlan &>(builder));
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
std::string dump(const ModulePlan &module) {
  static constexpr const char *names[] = {"literal",
                                          "unary",
                                          "binary",
                                          "kernel",
                                          "copy",
                                          "open",
                                          "spawn",
                                          "await",
                                          "try",
                                          "join",
                                          "label",
                                          "return",
                                          "catch",
                                          "case",
                                          "jump",
                                          "field",
                                          "fail",
                                          "call",
                                          "device",
                                          "transfer",
                                          "branch",
                                          "function.reference",
                                          "function.invoke",
                                          "synchronous.call",
                                          "map.init",
                                          "map.check",
                                          "map.load",
                                          "map.store",
                                          "map.advance",
                                          "map.success",
                                          "group.release",
                                          "callable.target",
                                          "callable.captures",
                                          "callable.sync.guard",
                                          "callable.invalid",
                                          "group.propagate"};
  std::ostringstream out;
  for (std::size_t f = 0; f < module.plans.size(); ++f) {
    const auto &plan = *module.plans[f];
    if (plan.code.empty())
      continue;
    out << "plan @" << f << " slots=" << plan.slots.size() << " groups=" << plan.groups
        << " result=$" << plan.result << '\n';
    for (std::size_t i = 0; i < plan.slots.size(); ++i)
      out << "  $" << i << " : " << describeType(plan.slots[i]) << '\n';
    for (std::size_t i = 0; i < plan.code.size(); ++i) {
      const auto &op = plan.code[i];
      out << "  " << i << ": " << names[static_cast<unsigned>(op.code)] << " $" << op.output
          << " (";
      for (auto arg : op.args)
        out << '$' << arg << ',';
      out << ") target=" << op.target << " group=" << op.group << " jump=" << op.jump;
      if (op.placement)
        out << " on $" << *op.placement;
      out << '\n';
    }
  }
  return out.str();
}
} // namespace kalandra::continuation
