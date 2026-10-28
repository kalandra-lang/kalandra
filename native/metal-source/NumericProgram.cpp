#include "NumericProgram.h"
#include "TensorTransformShader.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
namespace kalandra {
namespace {
struct Rejected {
  Diagnostic diagnostic;
};
struct Ref {
  ValueId expression;
  ValueType type;
  std::optional<std::uint32_t> buffer{};
};
class Builder {
  ArithmeticMode arithmetic_;
  const TypedModule &input;
  const TypedFunction &root;
  TypedModule normalized;
  NumericProgram program;
  std::map<SymbolId, std::uint32_t> buffers, hostScalars;
  std::set<FunctionId> active;
  std::map<std::string, std::uint32_t> kernelCache;
  std::size_t work = 0;
  std::uint32_t next = 0;
  [[noreturn]] void reject(SourceSpan span, const std::string &message) {
    throw Rejected{{"G001", message, span}};
  }
  void budget(SourceSpan span) {
    if (++work > 300000)
      reject(span, "typed numerical program expansion limit exceeded");
  }
  ValueId expression(TypedExpr node) {
    budget(node.span);
    auto id = normalized.expressions.size();
    normalized.expressions.push_back(std::move(node));
    return id;
  }
  Ref leaf(ValueType type, std::uint32_t id, bool host = false) {
    auto symbol = normalized.symbols.size();
    normalized.symbols.push_back(
        {"capture" + std::to_string(symbol), type, root.declaration, 0, true});
    (host ? hostScalars : buffers).emplace(symbol, id);
    return {expression({root.declaration, type, SymbolRead{symbol}}), type,
            host ? std::nullopt : std::optional{id}};
  }
  Ref resident(ValueType type, std::uint32_t id) { return leaf(std::move(type), id); }
  std::uint32_t value() {
    if (next == UINT32_MAX)
      reject(root.declaration, "typed numerical program value limit exceeded");
    return next++;
  }
  void check(NumericProgramKind kind, std::vector<std::uint32_t> inputs,
             std::vector<std::uint64_t> constants = {}) {
    program.code.push_back({kind, UINT32_MAX, std::move(inputs), {}, std::move(constants)});
  }
  static bool pureLiteral(const TypedExpr &node) {
    return std::holds_alternative<Float32Literal>(node.operation) ||
           std::holds_alternative<Float64Literal>(node.operation) ||
           std::holds_alternative<Int32Literal>(node.operation) ||
           std::holds_alternative<UInt32Literal>(node.operation) ||
           std::holds_alternative<Int64Literal>(node.operation) ||
           std::holds_alternative<UInt64Literal>(node.operation) ||
           std::holds_alternative<BoolLiteral>(node.operation);
  }
  void children(ValueId id, const std::function<void(ValueId)> &visit) {
    budget(normalized.expressions.at(id).span);
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, ScalarUnary> || std::is_same_v<T, TensorUnary>)
            visit(op.operand);
          else if constexpr (std::is_same_v<T, ScalarBinary> || std::is_same_v<T, TensorBinary> ||
                             std::is_same_v<T, ScalarCompare> || std::is_same_v<T, Matmul>) {
            visit(op.left);
            visit(op.right);
          } else if constexpr (std::is_same_v<T, TensorTransform>) {
            visit(op.input);
            if (op.epsilon)
              visit(*op.epsilon);
          } else if constexpr (std::is_same_v<T, NumericConvert> || std::is_same_v<T, ReduceSum>)
            visit(op.input);
        },
        normalized.expressions.at(id).operation);
  }
  std::uint32_t kernel(NumericShader shader) {
    auto key = shader.mlir + shader.nativeMetal +
               "\n#schema:" + std::to_string(static_cast<unsigned>(shader.element));
    auto append = [&](const auto &values) {
      key += '|';
      for (auto value : values)
        key += std::to_string(static_cast<unsigned>(value)) + ",";
    };
    append(shader.tensorTypes);
    append(shader.scalarTypes);
    append(shader.scalarBuffers);
    append(shader.reads);
    key += shader.scalar ? "s" : "t";
    auto [entry, inserted] = kernelCache.emplace(std::move(key), program.kernels.size());
    if (inserted)
      program.kernels.push_back(std::move(shader));
    return entry->second;
  }
  Ref map(Ref ref) {
    if (ref.buffer)
      return ref;
    TypedFunction fn{};
    fn.name = "numeric_stage" + std::to_string(program.kernels.size());
    fn.declaration = root.declaration;
    fn.result = ref.expression;
    fn.resultType = ref.type;
    std::set<SymbolId> used;
    std::set<ValueId> visited;
    std::function<void(ValueId)> find = [&](ValueId id) {
      if (!visited.insert(id).second)
        return;
      const auto &node = normalized.expressions.at(id);
      if (auto read = std::get_if<SymbolRead>(&node.operation))
        used.insert(read->symbol);
      children(id, find);
    };
    find(ref.expression);
    std::vector<SymbolId> tensors, scalars, hosts;
    for (auto symbol : used) {
      if (buffers.contains(symbol))
        (normalized.symbols.at(symbol).type.tensor ? tensors : scalars).push_back(symbol);
      else if (hostScalars.contains(symbol))
        hosts.push_back(symbol);
      else
        reject(root.declaration, "unbound numerical program capture");
    }
    NumericShaderOptions options;
    options.arithmetic = arithmetic_;
    options.boolResult = ref.type.builtin == BuiltinValue::Bool;
    std::vector<std::uint8_t> roles;
    NumericProgramInstruction instruction{NumericProgramKind::Map};
    for (auto symbol : tensors) {
      fn.parameters.push_back(symbol);
      roles.push_back(0);
      instruction.inputs.push_back(buffers.at(symbol));
    }
    for (auto symbol : scalars) {
      fn.parameters.push_back(symbol);
      roles.push_back(1);
      instruction.inputs.push_back(buffers.at(symbol));
    }
    for (auto symbol : hosts) {
      fn.parameters.push_back(symbol);
      roles.push_back(0);
      instruction.scalarSources.push_back(hostScalars.at(symbol));
    }
    options.deviceScalarParameters = roles;
    if (!tensors.empty()) {
      const auto &domain = normalized.symbols.at(tensors[0]).type.shape;
      for (auto symbol : tensors) {
        const auto &shape = normalized.symbols.at(symbol).type.shape;
        if (shape.size() != domain.size())
          reject(root.declaration, "typed map ranks differ");
        for (std::size_t axis = 0; axis < shape.size(); ++axis)
          fn.preconditions.push_back({shape[axis], domain[axis]});
      }
      if (ref.type.tensor) {
        if (ref.type.shape.size() != domain.size())
          reject(root.declaration, "typed map result rank differs");
        for (std::size_t axis = 0; axis < domain.size(); ++axis)
          fn.preconditions.push_back({ref.type.shape[axis], domain[axis]});
      }
    }
    const auto id = normalized.functions.size();
    normalized.functions.push_back(std::move(fn));
    auto shader = emitNumericShader(normalized, id, options);
    if (!shader)
      throw Rejected{shader.error()};
    // Emit optional matrix profiles only for an actual adjacent producer. Plain
    // maps must not pay for creating unused matrix pipeline states at startup.
    if (shader->noArithmeticFault && ref.type.tensor && !shader->indirect &&
        shader->reads.size() == 1 && shader->reads[0] == 0 && shader->tensorTypes.size() == 1 &&
        shader->tensorTypes[0] == NumericType::Float32 &&
        std::all_of(shader->scalarTypes.begin(), shader->scalarTypes.end(),
                    [](auto type) { return type == NumericType::Float32; }) &&
        std::none_of(shader->scalarBuffers.begin(), shader->scalarBuffers.end(),
                     [](auto role) { return role; }) &&
        instruction.inputs.size() == 1 && !program.code.empty() &&
        program.code.back().kind == NumericProgramKind::Matmul &&
        instruction.inputs[0] == program.code.back().output)
      shader->mlir = "// kalandra.numeric.matmul.epilogue 1 " +
                     std::to_string(shader->scalarTypes.size()) + "\n" + shader->mlir;
    instruction.kernel = kernel(std::move(*shader));
    instruction.output = value();
    program.code.push_back(std::move(instruction));
    return resident(ref.type, program.code.back().output);
  }
  Ref force(Ref ref) { return ref.buffer ? ref : map(std::move(ref)); }
  Ref transform(const TypedExpr &original, const TensorTransform &operation, Ref input,
                std::optional<Ref> epsilon) {
    input = force(input);
    if (operation.kind != TensorTransformKind::Transpose &&
        (arithmetic_ == ArithmeticMode::Software || original.type.element != NumericType::Float32))
      reject(original.span, "GPU row math requires hardware float32; no software transcendental "
                            "fallback is implemented");
    NumericShader shader{original.type.element, {}, {0}};
    shader.transform = operation.kind;
    shader.nativeMetal = tensorTransformMetal(operation.kind, original.type.element);
    shader.tensorTypes = {original.type.element};
    NumericProgramInstruction op{NumericProgramKind::Map};
    op.inputs = {*input.buffer};
    if (epsilon) {
      shader.scalarTypes = {original.type.element};
      const auto &scalar = normalized.expressions.at(epsilon->expression);
      if (auto literal = std::get_if<Float32Literal>(&scalar.operation)) {
        op.constants.push_back(literal->bits);
        shader.scalarBuffers = {0};
      } else if (auto read = std::get_if<SymbolRead>(&scalar.operation);
                 read && hostScalars.contains(read->symbol)) {
        op.scalarSources.push_back(hostScalars.at(read->symbol));
        shader.scalarBuffers = {0};
      } else {
        auto scalar = force(*epsilon);
        op.inputs.push_back(*scalar.buffer);
        shader.reads.push_back(1);
        shader.tensorTypes.push_back(original.type.element);
        shader.scalarTypes.clear();
        shader.scalarBuffers = {0, 1};
      }
    }
    op.kernel = kernel(std::move(shader));
    op.output = value();
    program.code.push_back(std::move(op));
    return resident(original.type, program.code.back().output);
  }
  Ref primitive(NumericProgramKind kind, const TypedExpr &original, Ref left,
                std::optional<Ref> right = {}) {
    if (!left.buffer || (right && !right->buffer))
      reject(original.span, "numerical primitive requires resident tensors");
    TypedFunction fn{};
    fn.name = "numeric_primitive" + std::to_string(program.kernels.size());
    fn.declaration = original.span;
    fn.resultType = original.type;
    auto parameter = [&](const Ref &ref) {
      auto symbol = normalized.symbols.size();
      normalized.symbols.push_back({"primitive" + std::to_string(symbol), ref.type, original.span,
                                    normalized.functions.size(), true});
      fn.parameters.push_back(symbol);
      return expression({original.span, ref.type, SymbolRead{symbol}});
    };
    auto node = original;
    const auto a = parameter(left);
    NumericProgramInstruction op{kind};
    op.inputs.push_back(*left.buffer);
    if (right) {
      const auto b = parameter(*right);
      op.inputs.push_back(*right->buffer);
      auto matrix = std::get<Matmul>(node.operation);
      matrix.left = a;
      matrix.right = b;
      node.operation = matrix;
    } else {
      auto sum = std::get<ReduceSum>(node.operation);
      sum.input = a;
      node.operation = sum;
    }
    fn.result = expression(std::move(node));
    const auto id = normalized.functions.size();
    normalized.functions.push_back(std::move(fn));
    auto shader = emitNumericShader(normalized, id, {.arithmetic = arithmetic_});
    if (!shader)
      throw Rejected{shader.error()};
    if (kind == NumericProgramKind::Reduce) {
      if (shader->reduction.empty())
        reject(original.span, "typed reduction stage missing");
      shader->mlir = std::move(shader->reduction);
      shader->reduction.clear();
    }
    op.kernel = kernel(std::move(*shader));
    op.output = value();
    program.code.push_back(std::move(op));
    return resident(original.type, program.code.back().output);
  }
  Ref invoke(FunctionId function, const std::vector<Ref> &arguments) {
    const auto &fn = input.functions.at(function);
    budget(fn.declaration);
    if (active.size() >= 128 || !active.insert(function).second)
      reject(fn.declaration, "recursive GPU numerical helper expansion");
    if (arguments.size() != fn.parameters.size())
      reject(fn.declaration, "numerical helper capture count mismatch");
    std::map<SymbolId, Ref> symbols;
    std::map<ValueId, Ref> cache;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      const auto &type = input.symbols.at(fn.parameters[i]).type;
      auto ref = arguments[i];
      if (type.tensor) {
        if (!ref.buffer || !ref.type.tensor || ref.type.shape.size() != type.shape.size())
          reject(fn.declaration, "numerical helper tensor rank mismatch");
        std::vector<std::uint64_t> constants;
        for (std::size_t axis = 0; axis < type.shape.size(); ++axis)
          if (type.shape[axis].constant) {
            constants.push_back(axis);
            constants.push_back(*type.shape[axis].constant);
          }
        if (!constants.empty())
          check(NumericProgramKind::Extent, {*ref.buffer}, std::move(constants));
        ref = resident(type, *ref.buffer);
      }
      symbols.emplace(fn.parameters[i], std::move(ref));
    }
    auto locate = [&](const Extent &extent) -> std::pair<std::uint32_t, unsigned> {
      for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
        const auto &type = input.symbols.at(fn.parameters[i]).type;
        if (!type.tensor)
          continue;
        for (unsigned axis = 0; axis < type.shape.size(); ++axis)
          if (!extent.constant && type.shape[axis].identity == extent.identity)
            return {*arguments[i].buffer, axis};
      }
      reject(fn.declaration, "numerical helper shape contract has no parameter axis");
    };
    for (const auto &requirement : fn.preconditions) {
      if (requirement.left.constant && requirement.right.constant) {
        if (requirement.left.constant != requirement.right.constant)
          check(NumericProgramKind::ShapeFault, {});
      } else if (requirement.left.constant || requirement.right.constant) {
        const auto &dynamic = requirement.left.constant ? requirement.right : requirement.left;
        const auto &constant = requirement.left.constant ? requirement.left : requirement.right;
        auto [buffer, axis] = locate(dynamic);
        check(NumericProgramKind::Extent, {buffer}, {axis, *constant.constant});
      } else {
        auto [a, ax] = locate(requirement.left);
        auto [b, bx] = locate(requirement.right);
        check(NumericProgramKind::Extent, {a, b}, {ax, bx});
      }
    }
    std::function<Ref(ValueId)> emit;
    auto statements = [&](const std::vector<TypedStatement> &body) {
      for (const auto &statement : body) {
        auto ref = emit(statement.value);
        // Even an unused checked scalar statement must execute at its source point.
        if (!ref.buffer && !pureLiteral(input.expressions.at(statement.value)))
          ref = force(ref);
        cache.insert_or_assign(statement.value, ref);
        if (statement.binding)
          symbols.insert_or_assign(*statement.binding, ref);
      }
    };
    emit = [&](ValueId id) -> Ref {
      budget(input.expressions.at(id).span);
      if (auto found = cache.find(id); found != cache.end())
        return found->second;
      const auto &node = input.expressions.at(id);
      if (node.type.task || node.type.fallible || node.type.array || node.type.errorRecord ||
          (node.type.builtin != BuiltinValue::Numeric && node.type.builtin != BuiltinValue::Bool) ||
          (node.type.tensor && node.type.shape.size() > 8))
        reject(node.span, "unsupported typed GPU numerical program value");
      Ref out{0, node.type};
      if (auto read = std::get_if<SymbolRead>(&node.operation)) {
        auto found = symbols.find(read->symbol);
        if (found == symbols.end())
          reject(node.span, "unbound numerical helper symbol");
        out = found->second;
      } else if (auto call = std::get_if<FunctionCall>(&node.operation)) {
        std::vector<Ref> args;
        for (auto value : call->arguments)
          args.push_back(emit(value));
        out = invoke(call->function, args);
      } else if (auto choice = std::get_if<ConditionalOperation>(&node.operation)) {
        auto condition = force(emit(choice->condition));
        const auto branch = program.code.size();
        check(NumericProgramKind::Branch, {*condition.buffer});
        const auto result = value();
        auto saved = symbols;
        statements(choice->whenTrue);
        auto a = force(emit(choice->trueResult));
        program.code.push_back(
            {NumericProgramKind::Alias, UINT32_MAX, {*a.buffer}, {}, {}, result});
        const auto jump = program.code.size();
        check(NumericProgramKind::Jump, {});
        program.code[branch].target = program.code.size();
        symbols = saved;
        statements(choice->whenFalse);
        auto b = force(emit(choice->falseResult));
        program.code.push_back(
            {NumericProgramKind::Alias, UINT32_MAX, {*b.buffer}, {}, {}, result});
        program.code[jump].target = program.code.size();
        symbols = std::move(saved);
        out = resident(node.type, result);
      } else if (auto shape = std::get_if<ShapeCheck>(&node.operation)) {
        out = emit(shape->input);
        if (!out.buffer)
          reject(node.span, "shape check requires a resident tensor");
        std::vector<std::uint64_t> extents;
        for (const auto &requirement : shape->requirements) {
          extents.push_back(requirement.axis);
          extents.push_back(requirement.extent);
        }
        check(NumericProgramKind::Extent, {*out.buffer}, std::move(extents));
        out.type = node.type;
        out = resident(out.type, *out.buffer);
      } else if (auto operation = std::get_if<TensorTransform>(&node.operation)) {
        auto input = force(emit(operation->input));
        auto epsilon =
            operation->epsilon ? std::optional<Ref>{emit(*operation->epsilon)} : std::nullopt;
        out = transform(node, *operation, input, epsilon);
      } else if (auto sum = std::get_if<ReduceSum>(&node.operation)) {
        auto a = emit(sum->input);
        out = primitive(NumericProgramKind::Reduce, node, a);
      } else if (auto matrix = std::get_if<Matmul>(&node.operation)) {
        auto a = emit(matrix->left), b = emit(matrix->right);
        if (!a.buffer || !b.buffer)
          reject(node.span, "matrix inputs require resident tensor values");
        check(NumericProgramKind::MatrixShape, {*a.buffer, *b.buffer});
        out = primitive(NumericProgramKind::Matmul, node, a, b);
      } else {
        auto copy = node;
        if (auto unary = std::get_if<ScalarUnary>(&copy.operation))
          unary->operand = emit(unary->operand).expression;
        else if (auto unary = std::get_if<TensorUnary>(&copy.operation))
          unary->operand = emit(unary->operand).expression;
        else if (auto binary = std::get_if<ScalarBinary>(&copy.operation)) {
          auto a = emit(binary->left), b = emit(binary->right);
          binary->left = a.expression;
          binary->right = b.expression;
        } else if (auto compare = std::get_if<ScalarCompare>(&copy.operation)) {
          auto a = emit(compare->left), b = emit(compare->right);
          compare->left = a.expression;
          compare->right = b.expression;
        } else if (auto binary = std::get_if<TensorBinary>(&copy.operation)) {
          auto a = emit(binary->left), b = emit(binary->right);
          if (a.type.tensor && b.type.tensor) {
            if (!a.buffer || !b.buffer)
              reject(node.span, "tensor operands require resident values");
            check(NumericProgramKind::EqualShape, {*a.buffer, *b.buffer});
          }
          binary->left = a.expression;
          binary->right = b.expression;
        } else if (auto convert = std::get_if<NumericConvert>(&copy.operation))
          convert->input = emit(convert->input).expression;
        else if (!pureLiteral(node))
          reject(node.span, "typed GPU numerical program operation is not implemented");
        out = {expression(std::move(copy)), node.type};
        if (node.type.tensor)
          out = force(std::move(out));
      }
      cache.insert_or_assign(id, out);
      return out;
    };
    statements(fn.body);
    auto result = emit(fn.result);
    active.erase(function);
    return result;
  }

public:
  Builder(const TypedModule &module, FunctionId id, ArithmeticMode arithmetic)
      : arithmetic_(arithmetic), input(module), root(module.functions.at(id)) {
    normalized.source = input.source;
  }
  NumericProgram run(FunctionId id) {
    if (root.resultType.builtin != BuiltinValue::Numeric || root.resultType.array ||
        root.resultType.task || root.resultType.fallible)
      reject(root.declaration, "GPU numerical program requires a numeric scalar or tensor result");
    std::vector<Ref> args;
    std::uint32_t scalar = 0;
    for (auto parameter : root.parameters) {
      const auto &type = input.symbols.at(parameter).type;
      if (type.builtin != BuiltinValue::Numeric || type.array || type.task || type.fallible)
        reject(root.declaration,
               "GPU numerical programs capture numeric scalars and resident tensors");
      args.push_back(type.tensor ? resident(type, value()) : leaf(type, scalar++, true));
    }
    auto result = force(invoke(id, args));
    program.result = *result.buffer;
    program.type = root.resultType;
    if (program.kernels.empty())
      reject(root.declaration, "GPU numerical program has no device stage");
    return std::move(program);
  }
};
} // namespace
std::expected<NumericProgram, Diagnostic>
emitNumericProgram(const TypedModule &input, FunctionId function, ArithmeticMode arithmetic) {
  try {
    return Builder(input, function, arithmetic).run(function);
  } catch (const Rejected &error) {
    return std::unexpected(error.diagnostic);
  }
}
} // namespace kalandra
