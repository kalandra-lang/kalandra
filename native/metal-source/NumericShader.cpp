#include "DeviceArrayReturn.h"
#include "../gpu-source/AggregateLayout.h"
#include "../gpu-runtime/ControlExport.h"
#include "../runtime/DeviceGraphWire.h"
#include "NumericShader.h"
#include "DeviceHeap.h"
#include "DeviceLiveness.h"
#include "MatmulShader.h"
#include "kalandra/Continuation.h"
#include "kalandra/GpuConversion.h"
#include "kalandra/GpuInteger.h"
#include "kalandra/NumericKernelName.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <sstream>
using namespace mlir;
namespace kalandra {
namespace {
struct Rejected {
  Diagnostic diagnostic;
};
struct ShaderValue {
  Value value, valid, uniform;
  bool tensor = false, boolean = false;
  std::optional<std::uint32_t> parameter{};
  std::vector<Extent> shape{};
  NumericType element = NumericType::Float32;
};
class Emitter {
  const TypedModule &input;
  const TypedFunction &root;
  const NumericShaderOptions &options;
  OpBuilder &lane, &header;
  gpu::GPUFuncOp kernel;
  Value index, yes;
  std::size_t work = 0;
  std::set<FunctionId> active;
  std::map<unsigned, Value> tensorValues;
  using Key = std::pair<bool, std::uint64_t>;
  std::map<Key, Key> parents;
  std::vector<std::vector<Extent>> domains;
  [[noreturn]] void reject(SourceSpan span, const std::string &message) {
    throw Rejected{{"G001", message, span}};
  }
  Location loc() { return header.getUnknownLoc(); }
  Value c(OpBuilder &b, std::uint64_t value, unsigned width = 64) {
    return arith::ConstantOp::create(
        b, loc(), b.getIntegerType(width),
        b.getIntegerAttr(b.getIntegerType(width), llvm::APInt(width, value)));
  }
  Value both(OpBuilder &b, Value a, Value d) { return arith::AndIOp::create(b, loc(), a, d); }
  Value either(OpBuilder &b, Value a, Value d) { return arith::OrIOp::create(b, loc(), a, d); }
  Value choose(OpBuilder &b, Value condition, Value a, Value d) {
    return arith::SelectOp::create(b, loc(), condition, a, d);
  }
  Value cmp(OpBuilder &b, arith::CmpIPredicate pred, Value a, Value d) {
    return arith::CmpIOp::create(b, loc(), pred, a, d);
  }
  Value negate(OpBuilder &b, Value value) { return arith::XOrIOp::create(b, loc(), value, yes); }
  Key representative(Key value) {
    auto result = value;
    for (;;) {
      if (++work > 300000)
        reject(root.declaration, "GPU shape proof budget exceeded");
      auto [found, inserted] = parents.emplace(result, result);
      if (inserted || found->second == result)
        break;
      result = found->second;
    }
    while (value != result) {
      auto next = parents.at(value);
      parents[value] = result;
      value = next;
    }
    return result;
  }
  static Key key(const Extent &extent) {
    return {extent.constant.has_value(), extent.constant.value_or(extent.identity)};
  }
  void sameShape(const std::vector<Extent> &a, const std::vector<Extent> &d, SourceSpan span) {
    if (a.size() != d.size())
      reject(span, "GPU numerical map ranks differ");
    for (std::size_t i = 0; i < a.size(); ++i)
      if (representative(key(a[i])) != representative(key(d[i])))
        reject(span, "GPU numerical maps require proven equal complete shapes; "
                     "independent domains "
                     "require separate stages");
  }
  Value materialize(ShaderValue &expr) {
    if (expr.value)
      return expr.value;
    if (!expr.parameter)
      reject(root.declaration, "missing typed GPU value");
    const auto p = *expr.parameter;
    if (auto found = tensorValues.find(p); found != tensorValues.end())
      return expr.value = found->second;
    auto &builder = expr.tensor ? lane : header;
    Value at = expr.tensor ? index : Value(arith::ConstantIndexOp::create(header, loc(), 0));
    auto value = memref::LoadOp::create(builder, loc(), kernel.getArgument(p), ValueRange{at});
    expr.value = describeNumeric(expr.element)->bits == 64
                     ? Value(value)
                     : Value(arith::ExtUIOp::create(builder, loc(), builder.getI64Type(), value));
    if (expr.boolean)
      expr.value = cmp(header, arith::CmpIPredicate::ne, expr.value, c(header, 0));
    tensorValues.emplace(p, expr.value);
    return expr.value;
  }
  ShaderValue literal(std::uint64_t value, bool boolean = false) {
    return {c(header, value, boolean ? 1 : 64), yes, yes, false, boolean};
  }
  ShaderValue binary(ShaderValue a, ShaderValue d, TokenKind op, SourceSpan span) {
    const auto element = a.element;
    const auto bits = describeNumeric(element)->bits;
    if (a.element != d.element)
      reject(span, "GPU arithmetic requires an explicit numeric conversion");
    const bool tensor = a.tensor || d.tensor;
    if (a.boolean || d.boolean)
      reject(span, "numeric GPU arithmetic requires numeric operands");
    if (a.tensor && d.tensor)
      sameShape(a.shape, d.shape, span);
    auto &b = tensor ? lane : header;
    auto x = materialize(a), y = materialize(d);
    Value result, valid = yes;
    if (isInteger(element)) {
      std::optional<GpuIntegerOperation> operation;
      if (op == TokenKind::Plus)
        operation = GpuIntegerOperation::Add;
      if (op == TokenKind::Minus)
        operation = GpuIntegerOperation::Subtract;
      if (op == TokenKind::Star)
        operation = GpuIntegerOperation::Multiply;
      if (op == TokenKind::Slash)
        operation = GpuIntegerOperation::Divide;
      if (op == TokenKind::Percent)
        operation = GpuIntegerOperation::Remainder;
      if (!operation)
        reject(span, "unsupported checked GPU integer operation");
      auto emitted = *emitCheckedGpuInteger(b, loc(), element, *operation, x, y);
      result = emitted.bits;
      valid = emitted.valid;
    } else {
      const auto floating = bits == 32 ? Type(b.getF32Type()) : Type(b.getF64Type());
      auto decode = [&](Value v) -> Value {
        if (bits == 32)
          v = arith::TruncIOp::create(b, loc(), b.getI32Type(), v);
        return arith::BitcastOp::create(b, loc(), floating, v);
      };
      x = decode(x);
      y = decode(y);
      if (op == TokenKind::Plus)
        result = arith::AddFOp::create(b, loc(), x, y);
      else if (op == TokenKind::Minus)
        result = arith::SubFOp::create(b, loc(), x, y);
      else if (op == TokenKind::Star)
        result = arith::MulFOp::create(b, loc(), x, y);
      else if (op == TokenKind::Slash)
        result = arith::DivFOp::create(b, loc(), x, y);
      else
        reject(span, "GPU floating arithmetic supports +, -, * and /");
      result = arith::BitcastOp::create(b, loc(), b.getIntegerType(bits), result);
      if (bits == 32)
        result = arith::ExtUIOp::create(b, loc(), b.getI64Type(), result);
    }
    auto uniform = both(header, a.uniform, d.uniform);
    if (!tensor)
      uniform = both(header, uniform, valid);
    auto checked = both(lane, both(lane, a.valid, d.valid), valid);
    auto shape = a.tensor ? a.shape : d.shape;
    if (tensor)
      domains.push_back(shape);
    return {result, checked, uniform, tensor, false, {}, std::move(shape)};
  }
  ShaderValue comparison(ShaderValue a, ShaderValue d, ComparisonKind op, SourceSpan span) {
    const auto element = a.element;
    const auto bits = describeNumeric(element)->bits;
    if (!a.boolean && a.element != d.element)
      reject(span, "GPU comparisons require an explicit numeric conversion");
    using P = arith::CmpIPredicate;
    if (a.tensor || d.tensor || a.boolean != d.boolean)
      reject(span, "GPU comparisons require matching scalar values");
    auto x = materialize(a), y = materialize(d);
    Value result;
    if (a.boolean) {
      if (op != ComparisonKind::Equal && op != ComparisonKind::NotEqual)
        reject(span, "GPU Bool comparisons support only equality");
      result = cmp(header, op == ComparisonKind::Equal ? P::eq : P::ne, x, y);
    } else if (isInteger(element)) {
      if (!isUnsigned(element) && bits < 64) {
        auto shift = c(header, 64 - bits);
        x = arith::ShRSIOp::create(header, loc(), arith::ShLIOp::create(header, loc(), x, shift),
                                   shift);
        y = arith::ShRSIOp::create(header, loc(), arith::ShLIOp::create(header, loc(), y, shift),
                                   shift);
      }
      const bool u = isUnsigned(element);
      const auto p = op == ComparisonKind::Equal       ? P::eq
                     : op == ComparisonKind::NotEqual  ? P::ne
                     : op == ComparisonKind::Less      ? (u ? P::ult : P::slt)
                     : op == ComparisonKind::LessEqual ? (u ? P::ule : P::sle)
                     : op == ComparisonKind::Greater   ? (u ? P::ugt : P::sgt)
                                                       : (u ? P::uge : P::sge);
      result = cmp(header, p, x, y);
    } else if (bits == 32 && options.arithmetic != ArithmeticMode::Software) {
      auto fx =
          arith::BitcastOp::create(header, loc(), header.getF32Type(),
                                   arith::TruncIOp::create(header, loc(), header.getI32Type(), x));
      auto fy =
          arith::BitcastOp::create(header, loc(), header.getF32Type(),
                                   arith::TruncIOp::create(header, loc(), header.getI32Type(), y));
      using F = arith::CmpFPredicate;
      const auto predicate = op == ComparisonKind::Equal       ? F::OEQ
                             : op == ComparisonKind::NotEqual  ? F::UNE
                             : op == ComparisonKind::Less      ? F::OLT
                             : op == ComparisonKind::LessEqual ? F::OLE
                             : op == ComparisonKind::Greater   ? F::OGT
                                                               : F::OGE;
      result = arith::CmpFOp::create(header, loc(), predicate, fx, fy);
    } else {
      auto mask = c(header, (UINT64_C(1) << (bits - 1)) - 1),
           sign = c(header, UINT64_C(1) << (bits - 1));
      auto inf = c(header, bits == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000));
      auto ax = both(header, x, mask), ay = both(header, y, mask), zero = c(header, 0);
      auto ordered = both(header, cmp(header, P::ule, ax, inf), cmp(header, P::ule, ay, inf));
      auto equal = either(header, cmp(header, P::eq, x, y),
                          both(header, cmp(header, P::eq, ax, zero), cmp(header, P::eq, ay, zero)));
      auto sx = both(header, x, sign), sy = both(header, y, sign);
      auto negative = cmp(header, P::ne, sx, zero);
      auto same = choose(header, negative, cmp(header, P::ugt, x, y), cmp(header, P::ult, x, y));
      auto less = both(header, negate(header, equal),
                       choose(header, cmp(header, P::ne, sx, sy), negative, same));
      if (op == ComparisonKind::Equal)
        result = both(header, ordered, equal);
      else if (op == ComparisonKind::NotEqual)
        result = negate(header, both(header, ordered, equal));
      else if (op == ComparisonKind::Less)
        result = both(header, ordered, less);
      else if (op == ComparisonKind::LessEqual)
        result = both(header, ordered, either(header, less, equal));
      else if (op == ComparisonKind::Greater)
        result = both(header, ordered, negate(header, either(header, less, equal)));
      else
        result = both(header, ordered, negate(header, less));
    }
    return {result, both(lane, a.valid, d.valid), both(header, a.uniform, d.uniform), false, true};
  }

public:
  Emitter(const TypedModule &module, FunctionId id, OpBuilder &l, OpBuilder &h, gpu::GPUFuncOp fn,
          Value i, const NumericShaderOptions &options)
      : input(module), root(module.functions.at(id)), options(options), lane(l), header(h),
        kernel(fn), index(i), yes(c(header, 1, 1)) {
    for (const auto &requirement : root.preconditions)
      parents[representative(key(requirement.left))] = representative(key(requirement.right));
  }
  std::vector<std::uint32_t> reads() const {
    std::vector<std::uint32_t> result;
    for (auto &[p, v] : tensorValues) {
      (void)v;
      result.push_back(p);
    }
    return result;
  }
  ShaderValue invoke(FunctionId function, const std::vector<ShaderValue> &args) {
    const auto &fn = input.functions.at(function);
    if (usesScalarControl(input, function))
      reject(fn.declaration, "device local control requires checkpointed scalar execution");
    if (++work > 300000 || active.size() >= 128 || !active.insert(function).second)
      reject(fn.declaration, "recursive or oversized typed GPU helper expansion");
    std::map<SymbolId, ShaderValue> symbols;
    std::map<ValueId, ShaderValue> values;
    for (std::size_t i = 0; i < args.size(); ++i)
      symbols.emplace(fn.parameters.at(i), args[i]);
    std::function<ShaderValue(ValueId)> emit = [&](ValueId id) -> ShaderValue {
      if (++work > 300000)
        reject(fn.declaration, "typed GPU expression budget exceeded");
      if (auto found = values.find(id); found != values.end())
        return found->second;
      const auto &node = input.expressions.at(id);
      if (node.type.task || node.type.fallible || node.type.array ||
          (node.type.builtin != BuiltinValue::Bool &&
           (node.type.builtin != BuiltinValue::Numeric || !describeNumeric(node.type.element))))
        reject(node.span, "typed GPU maps require homogeneous numerical values "
                          "and internal Bool conditions");
      ShaderValue out;
      if (auto read = std::get_if<SymbolRead>(&node.operation))
        out = symbols.at(read->symbol);
      else if (auto value = std::get_if<BoolLiteral>(&node.operation))
        out = literal(value->value, true);
      else if (auto compare = std::get_if<ScalarCompare>(&node.operation))
        out = comparison(emit(compare->left), emit(compare->right), compare->operation, node.span);
      else if (std::holds_alternative<ScalarBinary>(node.operation) ||
               std::holds_alternative<TensorBinary>(node.operation)) {
        const auto scalar = std::get_if<ScalarBinary>(&node.operation);
        const auto tensor = std::get_if<TensorBinary>(&node.operation);
        out = binary(emit(scalar ? scalar->left : tensor->left),
                     emit(scalar ? scalar->right : tensor->right),
                     scalar ? scalar->operation : tensor->operation, node.span);
      } else if (std::holds_alternative<ScalarUnary>(node.operation) ||
                 std::holds_alternative<TensorUnary>(node.operation)) {
        const auto scalar = std::get_if<ScalarUnary>(&node.operation);
        const auto tensor = std::get_if<TensorUnary>(&node.operation);
        out = emit(scalar ? scalar->operand : tensor->operand);
        const auto op = scalar ? scalar->operation : tensor->operation;
        const auto element = out.element;
        const auto bits = describeNumeric(element)->bits;
        if (out.boolean)
          out.value = negate(header, materialize(out));
        else if (op == TokenKind::Relu) {
          auto &builder = out.tensor ? lane : header;
          auto value = materialize(out);
          if (!isUnsigned(element)) {
            auto sign =
                arith::AndIOp::create(builder, loc(), value, c(builder, UINT64_C(1) << (bits - 1)));
            Value clear = cmp(builder, arith::CmpIPredicate::ne, sign, c(builder, 0));
            if (!isInteger(element)) {
              auto magnitude = arith::AndIOp::create(
                  builder, loc(), value,
                  c(builder, bits == 32 ? UINT64_C(0x7fffffff) : UINT64_C(0x7fffffffffffffff)));
              auto notNaN =
                  cmp(builder, arith::CmpIPredicate::ule, magnitude,
                      c(builder, bits == 32 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000)));
              clear = arith::AndIOp::create(builder, loc(), clear, notNaN);
            }
            out.value = arith::SelectOp::create(builder, loc(), clear, c(builder, 0), value);
            out.parameter.reset();
          }
        } else if (op == TokenKind::Minus) {
          auto &builder = out.tensor ? lane : header;
          if (isInteger(element)) {
            auto numeric =
                *emitCheckedGpuInteger(builder, loc(), element, GpuIntegerOperation::Negate,
                                       materialize(out), c(builder, 0));
            out.value = numeric.bits;
            out.valid = both(lane, out.valid, numeric.valid);
            if (!out.tensor)
              out.uniform = both(header, out.uniform, numeric.valid);
          } else
            out.value = arith::XOrIOp::create(builder, loc(), materialize(out),
                                              c(builder, UINT64_C(1) << (bits - 1)));
          out.parameter.reset();
        }
        if (out.tensor)
          domains.push_back(out.shape);
      } else if (auto range = std::get_if<IntegerRangeOperation>(&node.operation)) {
        auto a = emit(range->current), stop = emit(range->stop), step = emit(range->step);
        auto x = materialize(a), end = materialize(stop), stride = materialize(step);
        auto nonzero = cmp(header, arith::CmpIPredicate::ne, stride, c(header, 0));
        auto positive = cmp(header, arith::CmpIPredicate::sgt, stride, c(header, 0));
        out = a;
        out.valid = both(lane, both(lane, a.valid, stop.valid), step.valid);
        out.uniform = both(header, both(header, a.uniform, stop.uniform), step.uniform);
        if (range->kind == IntegerRangeOperation::Kind::Validate) {
          out.value = x;
          out.valid = both(lane, out.valid, nonzero);
          out.uniform = both(header, out.uniform, nonzero);
        } else if (range->kind == IntegerRangeOperation::Kind::Test) {
          out.value = choose(header, positive, cmp(header, arith::CmpIPredicate::slt, x, end),
                             cmp(header, arith::CmpIPredicate::sgt, x, end));
          out.boolean = true;
        } else {
          auto sum = *emitCheckedGpuInteger(header, loc(), NumericType::Int64,
                                            GpuIntegerOperation::Add, x, stride);
          out.value = choose(header, sum.valid, sum.bits, end);
        }
        out.parameter.reset();
      } else if (auto choice = std::get_if<ConditionalOperation>(&node.operation)) {
        auto condition = emit(choice->condition);
        if (!condition.boolean)
          reject(node.span, "GPU choice requires Bool");
        auto saved = symbols;
        auto arm = [&](const std::vector<TypedStatement> &body, ValueId result) {
          auto valid = yes, uniform = yes;
          for (const auto &statement : body) {
            auto value = emit(statement.value);
            valid = both(lane, valid, value.valid);
            uniform = both(header, uniform, value.uniform);
            if (statement.binding)
              symbols[*statement.binding] = value;
          }
          auto value = emit(result);
          value.valid = both(lane, valid, value.valid);
          value.uniform = both(header, uniform, value.uniform);
          return value;
        };
        auto a = arm(choice->whenTrue, choice->trueResult);
        symbols = saved;
        auto d = arm(choice->whenFalse, choice->falseResult);
        symbols = std::move(saved);
        if (a.tensor != d.tensor || a.boolean != d.boolean)
          reject(node.span, "GPU choice result categories differ");
        if (a.tensor)
          sameShape(a.shape, d.shape, node.span);
        auto &builder = a.tensor ? lane : header;
        out = {
            choose(builder, condition.value, materialize(a), materialize(d)),
            both(lane, condition.valid, choose(lane, condition.value, a.valid, d.valid)),
            both(header, condition.uniform, choose(header, condition.value, a.uniform, d.uniform)),
            a.tensor,
            a.boolean,
            {},
            a.shape};
      } else if (auto call = std::get_if<FunctionCall>(&node.operation)) {
        std::vector<ShaderValue> inputs;
        for (auto argument : call->arguments)
          inputs.push_back(emit(argument));
        out = invoke(call->function, inputs);
      } else if (auto conversion = std::get_if<NumericConvert>(&node.operation)) {
        out = emit(conversion->input);
        if (out.boolean)
          reject(node.span, "GPU numeric conversion requires a numeric value");
        auto &b = out.tensor ? lane : header;
        auto [converted, valid] =
            gpuNumericConversion(b, loc(), materialize(out), out.element, node.type.element,
                                 options.arithmetic, options.nativeFloat64Conversions);
        out.value = converted;
        out.valid = both(lane, out.valid, valid);
        if (!out.tensor)
          out.uniform = both(header, out.uniform, valid);
        out.parameter.reset();
      } else if (auto check = std::get_if<ShapeCheck>(&node.operation))
        out = emit(check->input);
      else {
        std::optional<std::uint64_t> raw;
        std::visit(
            [&](const auto &literal) {
              using T = std::decay_t<decltype(literal)>;
              if constexpr (std::is_same_v<T, Float32Literal> || std::is_same_v<T, Float64Literal>)
                raw = literal.bits;
              else if constexpr (std::is_same_v<T, Int32Literal> ||
                                 std::is_same_v<T, UInt32Literal> ||
                                 std::is_same_v<T, Int64Literal> ||
                                 std::is_same_v<T, UInt64Literal>)
                raw = static_cast<std::uint64_t>(literal.value) &
                      (describeNumeric(node.type.element)->bits == 64
                           ? UINT64_MAX
                           : (UINT64_C(1) << describeNumeric(node.type.element)->bits) - 1);
            },
            node.operation);
        if (!raw)
          reject(node.span, "numerical graph primitives and conversions "
                            "require their typed GPU stages");
        out = literal(*raw);
      }
      if (node.type.builtin == BuiltinValue::Numeric)
        out.element = node.type.element;
      values.emplace(id, out);
      return out;
    };
    auto valid = yes, uniform = yes;
    for (const auto &statement : fn.body) {
      auto value = emit(statement.value);
      valid = both(lane, valid, value.valid);
      uniform = both(header, uniform, value.uniform);
      if (statement.binding)
        symbols[*statement.binding] = value;
    }
    auto result = emit(fn.result);
    result.valid = both(lane, valid, result.valid);
    result.uniform = both(header, uniform, result.uniform);
    active.erase(function);
    return result;
  }
  ShaderValue rootCall(FunctionId id, unsigned tensorCount) {
    std::vector<ShaderValue> args;
    unsigned tensors = 0, scalarWords = 0, position = 0, realTensors = 0;
    auto parameters = kernel.getArgument(tensorCount + 1);
    for (auto parameter : root.parameters) {
      const auto &type = input.symbols.at(parameter).type;
      const bool deviceScalar =
          !options.deviceScalarParameters.empty() && options.deviceScalarParameters[position];
      ++position;
      if ((type.builtin != BuiltinValue::Numeric &&
           !(type.builtin == BuiltinValue::Bool && !type.tensor)) ||
          type.array || !describeNumeric(type.element))
        reject(root.declaration, "typed GPU captures require one exact numerical type");
      if (type.tensor) {
        ++realTensors;
        args.push_back({{}, yes, yes, true, false, tensors++, type.shape});
      } else if (deviceScalar) {
        args.push_back({{}, yes, yes, false, type.builtin == BuiltinValue::Bool, tensors++, {}});
      } else {
        const auto numeric = type.builtin == BuiltinValue::Bool ? NumericType::UInt8 : type.element;
        const auto bits = describeNumeric(numeric)->bits;
        const auto offset = 1 + scalarWords;
        scalarWords += bits == 64 ? 2 : 1;
        auto load = [&](unsigned i) -> Value {
          auto index = arith::ConstantIndexOp::create(header, loc(), i);
          auto word = memref::LoadOp::create(header, loc(), parameters, ValueRange{index});
          return arith::ExtUIOp::create(header, loc(), header.getI64Type(), word);
        };
        Value value = load(offset);
        if (bits == 64)
          value = arith::OrIOp::create(
              header, loc(), value,
              arith::ShLIOp::create(header, loc(), load(offset + 1), c(header, 32)));
        const bool boolean = type.builtin == BuiltinValue::Bool;
        if (boolean)
          value = cmp(header, arith::CmpIPredicate::ne, value, c(header, 0));
        args.push_back({value, yes, yes, false, boolean});
      }
      args.back().element = type.builtin == BuiltinValue::Bool ? NumericType::UInt8 : type.element;
    }
    auto result = invoke(id, args);
    if ((result.boolean && !options.boolResult) || (root.resultType.tensor && !result.tensor) ||
        (!root.resultType.tensor && realTensors))
      reject(root.declaration, "typed scalar maps require scalar captures and results");
    result.value = materialize(result);
    if (result.boolean) {
      result.value = arith::ExtUIOp::create(header, loc(), header.getI64Type(), result.value);
      result.boolean = false;
      result.element = NumericType::UInt8;
    }
    for (auto &[parameter, value] : tensorValues) {
      (void)value;
      if (argsForTensor(args, parameter).tensor)
        sameShape(argsForTensor(args, parameter).shape, result.shape, root.declaration);
    }
    for (const auto &shape : domains)
      sameShape(shape, result.shape, root.declaration);
    return result;
  }
  static const ShaderValue &argsForTensor(const std::vector<ShaderValue> &args,
                                          unsigned parameter) {
    for (const auto &arg : args)
      if (arg.parameter && *arg.parameter == parameter)
        return arg;
    std::terminate();
  }
};
} // namespace
static std::string numericMatmulShader(NumericType type) {
  const auto bits = describeNumeric(type)->bits;
  std::string source = matmulShader;
  auto replace = [&](std::string_view from, const std::string &to) {
    std::size_t pos = 0;
    while ((pos = source.find(from, pos)) != std::string::npos) {
      source.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace("@kalandra_matmul",
          "@kalandra_numeric_v1_matmul_" + std::string(describeNumeric(type)->name));
  replace("memref<3xi32>", "memref<4xi32>");
  replace("%floatzero = arith.bitcast %z32 : i32 to f32", "%floatzero = arith.constant 0 : f32");
  replace("%product = arith.mulf %av, %bv : f32",
          "%product = arith.muli %av, %bv : f32 loc(\"numeric.multiply\")");
  replace("%sum = arith.addf %previous, %v : f32",
          "%sum = arith.addi %previous, %v : f32 loc(\"numeric.add\")");
  replace("%sum = arith.addf %root, %acc : f32",
          "%sum = arith.addi %root, %acc : f32 loc(\"numeric.add\")");
  replace("f32", "i64");
  if (bits < 64) {
    const auto scalar = "i" + std::to_string(bits);
    replace("memref<?xi64>", "memref<?x" + scalar + ">");
    replace("%av = memref.load %a[%ai] : memref<?x" + scalar + ">",
            "%avRaw = memref.load %a[%ai] : memref<?x" + scalar +
                ">\n%av = arith.extui %avRaw : " + scalar + " to i64");
    replace("%bv = memref.load %b[%bi] : memref<?x" + scalar + ">",
            "%bvRaw = memref.load %b[%bi] : memref<?x" + scalar +
                ">\n%bv = arith.extui %bvRaw : " + scalar + " to i64");
    replace("memref.store %result, %output[%cell]", "%narrow = arith.trunci %result : i64 to " +
                                                        scalar +
                                                        "\nmemref.store %narrow, %output[%cell]");
  }
  std::string capabilities = "Shader, Int64", extensions = "SPV_KHR_storage_buffer_storage_class";
  if (bits == 8) {
    capabilities += ", Int8, StorageBuffer8BitAccess";
    extensions += ", SPV_KHR_8bit_storage";
  }
  if (bits == 16) {
    capabilities += ", Int16, StorageBuffer16BitAccess";
    extensions += ", SPV_KHR_16bit_storage";
  }
  replace("Shader, Int64", capabilities);
  replace("[SPV_KHR_storage_buffer_storage_class]", "[" + extensions + "]");
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  auto module = parseSourceString<ModuleOp>(source, &context);
  if (!module)
    return {};
  gpu::GPUFuncOp kernel;
  Value cell;
  SmallVector<Operation *> operations;
  module->walk([&](gpu::GPUFuncOp op) { kernel = op; });
  module->walk([&](Operation *op) {
    if (auto name = dyn_cast<NameLoc>(op->getLoc());
        name && (name.getName() == "numeric.multiply" || name.getName() == "numeric.add"))
      operations.push_back(op);
    if (auto store = dyn_cast<memref::StoreOp>(op);
        store && store.getMemRef() == kernel.getArgument(2))
      cell = store.getIndices()[0];
  });
  if (!cell || operations.size() != 3)
    return {};
  for (auto op : operations) {
    OpBuilder b(op);
    auto loc = b.getUnknownLoc();
    Value result, valid;
    const auto multiply = isa<arith::MulIOp>(op);
    auto a = op->getOperand(0), d = op->getOperand(1);
    if (isInteger(type)) {
      auto out = *emitCheckedGpuInteger(
          b, loc, type, multiply ? GpuIntegerOperation::Multiply : GpuIntegerOperation::Add, a, d);
      result = out.bits;
      valid = out.valid;
    } else {
      const auto floating = bits == 64 ? Type(b.getF64Type()) : Type(b.getF32Type());
      if (bits == 32) {
        a = arith::TruncIOp::create(b, loc, b.getI32Type(), a);
        d = arith::TruncIOp::create(b, loc, b.getI32Type(), d);
      }
      a = arith::BitcastOp::create(b, loc, floating, a);
      d = arith::BitcastOp::create(b, loc, floating, d);
      Value value = multiply ? Value(arith::MulFOp::create(b, loc, a, d))
                             : Value(arith::AddFOp::create(b, loc, a, d));
      result = arith::BitcastOp::create(b, loc, b.getIntegerType(bits), value);
      if (bits == 32)
        result = arith::ExtUIOp::create(b, loc, b.getI64Type(), result);
      valid = arith::ConstantIntOp::create(b, loc, 1, 1);
    }
    auto yes = arith::ConstantIntOp::create(b, loc, 1, 1);
    auto bad = arith::XOrIOp::create(b, loc, valid, yes);
    scf::IfOp::create(b, loc, bad, [&](OpBuilder &inner, Location l) {
      auto index = arith::ConstantIndexOp::create(inner, l, 3);
      auto lane = arith::IndexCastOp::create(inner, l, inner.getI32Type(), cell);
      memref::AtomicRMWOp::create(inner, l, arith::AtomicRMWKind::minu, lane, kernel.getArgument(3),
                                  ValueRange{index});
      scf::YieldOp::create(inner, l);
    });
    op->getResult(0).replaceAllUsesWith(result);
    op->erase();
  }
  if (failed(verify(*module)))
    return {};
  std::string ir;
  llvm::raw_string_ostream out(ir);
  module->print(out);
  return ir;
}
std::string numericReductionShader(NumericType type) {
  const auto bits = describeNumeric(type)->bits;
  const auto buffer = "memref<?xi" + std::to_string(bits) + ">";
  const auto params = "memref<2xi32>";
  std::ostringstream source;
  source << "module attributes {gpu.container_module, spirv.target_env = "
            "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64";
  if (bits == 8)
    source << ", Int8, StorageBuffer8BitAccess";
  if (bits == 16)
    source << ", Int16, StorageBuffer16BitAccess";
  source << "], [SPV_KHR_storage_buffer_storage_class";
  if (bits == 8)
    source << ", SPV_KHR_8bit_storage";
  if (bits == 16)
    source << ", SPV_KHR_16bit_storage";
  source << "]>, #spirv.resource_limits<>>} { gpu.module @kernels { gpu.func "
            "@kalandra_numeric_v1_reduce_"
         << describeNumeric(type)->name << "(%in:" << buffer << ",%out:" << buffer
         << ",%params:" << params
         << ") kernel attributes {spirv.entry_point_abi = "
            "#spirv.entry_point_abi<workgroup_size = "
            "[64,1,1]>} {\n"
            "%block=gpu.block_id x\n%thread=gpu.thread_id "
            "x\n%width=gpu.block_dim x\n"
            "%base=arith.muli %block,%width:index\n%i=arith.addi "
            "%base,%thread:index\n"
            "%zero=arith.constant 0:index\n%one=arith.constant "
            "1:index\n%two=arith.constant 2:index\n"
            "%count=memref.load %params[%zero]:"
         << params
         << "\n%n=arith.index_cast %count:i32 to index\n"
            "%left=arith.muli %i,%two:index\n%active=arith.cmpi "
            "ult,%left,%n:index\nscf.if %active {\n"
            "%right=arith.addi %left,%one:index\n%a=memref.load %in[%left]:"
         << buffer
         << "\n"
            "%paired=arith.cmpi ult,%right,%n:index\n"
            "%result=scf.if %paired -> (i"
         << bits << ") {\n%b=memref.load %in[%right]:" << buffer
         << "\n"
            "%placeholder=arith.constant 0:i"
         << bits
         << " loc(\"reduce.bits\")\n"
            "%valid=arith.constant true "
            "loc(\"reduce.valid\")\n%yes=arith.constant true\n"
            "%bad=arith.xori %valid,%yes:i1\nscf.if %bad "
            "{\n%lane=arith.index_cast %i:index to i32\n"
            "%old=memref.atomic_rmw minu %lane,%params[%one]:(i32,"
         << params
         << ")->i32\n}\n"
            "scf.yield %placeholder:i"
         << bits << "\n} else {scf.yield %a:i" << bits
         << "}\n"
            "memref.store %result,%out[%i]:"
         << buffer << "\n}\ngpu.return\n}}}\n";
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  auto module = parseSourceString<ModuleOp>(source.str(), &context);
  if (!module)
    return {};
  arith::ConstantOp placeholder, validity;
  SmallVector<Value> values;
  module->walk([&](Operation *op) {
    if (auto name = dyn_cast<NameLoc>(op->getLoc())) {
      if (name.getName() == "reduce.bits")
        placeholder = cast<arith::ConstantOp>(op);
      if (name.getName() == "reduce.valid")
        validity = cast<arith::ConstantOp>(op);
    }
    if (auto load = dyn_cast<memref::LoadOp>(op); load && !load.getMemRefType().hasStaticShape())
      values.push_back(load.getResult());
  });
  if (!placeholder || !validity || values.size() != 2)
    return {};
  OpBuilder builder(placeholder);
  auto loc = builder.getUnknownLoc();
  Value result, valid;
  if (isInteger(type)) {
    auto widen = [&](Value v) -> Value {
      return bits == 64 ? v : Value(arith::ExtUIOp::create(builder, loc, builder.getI64Type(), v));
    };
    auto added = *emitCheckedGpuInteger(builder, loc, type, GpuIntegerOperation::Add,
                                        widen(values[0]), widen(values[1]));
    result = bits == 64 ? added.bits
                        : Value(arith::TruncIOp::create(builder, loc, builder.getIntegerType(bits),
                                                        added.bits));
    valid = added.valid;
  } else {
    auto floating = bits == 64 ? Type(builder.getF64Type()) : Type(builder.getF32Type());
    auto a = arith::BitcastOp::create(builder, loc, floating, values[0]);
    auto b = arith::BitcastOp::create(builder, loc, floating, values[1]);
    result = arith::BitcastOp::create(builder, loc, builder.getIntegerType(bits),
                                      arith::AddFOp::create(builder, loc, a, b));
    valid = arith::ConstantIntOp::create(builder, loc, 1, 1);
  }
  placeholder.getResult().replaceAllUsesWith(result);
  validity.getResult().replaceAllUsesWith(valid);
  placeholder.erase();
  validity.erase();
  if (failed(verify(*module)))
    return {};
  std::string ir;
  llvm::raw_string_ostream out(ir);
  module->print(out);
  return ir;
}
std::expected<NumericShader, Diagnostic> emitNumericShader(const TypedModule &input,
                                                           FunctionId function,
                                                           const NumericShaderOptions &options) {
  const auto &root = input.functions.at(function);
  if (auto matrix = std::get_if<Matmul>(&input.expressions.at(root.result).operation)) {
    for (const auto &statement : root.body)
      if (statement.value != root.result)
        return std::unexpected(
            Diagnostic{"G001", "typed matrix body effects require graph stages", root.declaration});
    if (root.parameters.size() != 2)
      return std::unexpected(Diagnostic{
          "G001", "typed matrix entry requires its two tensor operands", root.declaration});
    for (unsigned i = 0; i < 2; ++i) {
      auto id = i == 0 ? matrix->left : matrix->right;
      auto read = std::get_if<SymbolRead>(&input.expressions.at(id).operation);
      if (!read || read->symbol != root.parameters[i] ||
          input.symbols.at(read->symbol).type.element != root.resultType.element)
        return std::unexpected(
            Diagnostic{"G001", "typed matrix operand maps require graph stages", root.declaration});
    }
    auto ir = numericMatmulShader(root.resultType.element);
    if (ir.empty())
      return std::unexpected(
          Diagnostic{"G001", "typed matrix shader failed verification", root.declaration});
    if (root.resultType.element == NumericType::Float32 &&
        options.arithmetic == ArithmeticMode::Hardware)
      ir = "// kalandra-hardware-float32-matrix\n" + ir;
    return NumericShader{root.resultType.element, std::move(ir), {0, 1}, {}, true};
  }
  if (auto reduction = std::get_if<ReduceSum>(&input.expressions.at(root.result).operation)) {
    auto mapped = input;
    auto &fn = mapped.functions.at(function);
    fn.result = reduction->input;
    fn.resultType = mapped.expressions.at(fn.result).type;
    fn.body.erase(std::remove_if(fn.body.begin(), fn.body.end(),
                                 [&](const auto &s) { return s.value == root.result; }),
                  fn.body.end());
    auto shader = emitNumericShader(mapped, function, options);
    if (!shader)
      return shader;
    shader->reduction = numericReductionShader(shader->element);
    if (shader->reduction.empty())
      return std::unexpected(
          Diagnostic{"G001", "typed reduction shader failed verification", root.declaration});
    return shader;
  }
  if (root.resultType.builtin != BuiltinValue::Numeric &&
      !(options.boolResult && root.resultType.builtin == BuiltinValue::Bool))
    return std::unexpected(
        Diagnostic{"G001", "typed numerical maps require tensor results", root.declaration});
  if (!options.deviceScalarParameters.empty() &&
      options.deviceScalarParameters.size() != root.parameters.size())
    return std::unexpected(
        Diagnostic{"G001", "invalid device scalar parameter projection", root.declaration});
  const auto element = options.boolResult ? NumericType::UInt8 : root.resultType.element;
  const auto description = describeNumeric(element);
  if (!description)
    return std::unexpected(Diagnostic{"G001", "invalid typed GPU element", root.declaration});
  unsigned tensors = 0;
  std::uint64_t scalarWords = 0;
  std::vector<NumericType> tensorTypes, scalarTypes;
  std::vector<std::uint8_t> scalarBuffers;
  std::size_t position = 0;
  bool narrow8 = description->bits == 8, narrow16 = description->bits == 16;
  for (auto parameter : root.parameters) {
    const auto &type = input.symbols.at(parameter).type;
    const auto numeric = type.builtin == BuiltinValue::Bool ? NumericType::UInt8 : type.element;
    const auto desc = describeNumeric(numeric);
    const bool deviceScalar =
        !options.deviceScalarParameters.empty() && options.deviceScalarParameters[position];
    ++position;
    if ((type.builtin != BuiltinValue::Numeric &&
         !(type.builtin == BuiltinValue::Bool && !type.tensor)) ||
        type.array || !desc)
      return std::unexpected(
          Diagnostic{"G001", "typed GPU captures require numeric values", root.declaration});
    if (type.tensor || deviceScalar) {
      ++tensors;
      tensorTypes.push_back(numeric);
      scalarBuffers.push_back(deviceScalar);
      narrow8 |= desc->bits == 8;
      narrow16 |= desc->bits == 16;
    } else {
      scalarTypes.push_back(numeric);
      scalarWords += desc->bits == 64 ? 2 : 1;
    }
  }
  if (scalarWords > UINT32_MAX - 2)
    return std::unexpected(
        Diagnostic{"G001", "typed GPU scalar interface limit exceeded", root.declaration});
  const auto bits = description->bits;
  const auto words = 2 + scalarWords;
  const auto buffer = "memref<?xi" + std::to_string(bits) + ">";
  auto spelling = numericKernelName(element, tensorTypes, scalarTypes);
  if (std::any_of(scalarBuffers.begin(), scalarBuffers.end(), [](auto x) { return x != 0; })) {
    spelling += "_d_";
    for (auto role : scalarBuffers)
      spelling += role ? 's' : 't';
  }
  const auto parameters = "memref<" + std::to_string(words) + "xi32>";
  std::ostringstream source;
  source << "module attributes {gpu.container_module, spirv.target_env = "
            "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64";
  if (narrow8)
    source << ", Int8, StorageBuffer8BitAccess";
  if (narrow16)
    source << ", Int16, StorageBuffer16BitAccess";
  source << "], [SPV_KHR_storage_buffer_storage_class";
  if (narrow8)
    source << ", SPV_KHR_8bit_storage";
  if (narrow16)
    source << ", SPV_KHR_16bit_storage";
  source << "]>, #spirv.resource_limits<>>} { gpu.module @kernels { gpu.func @" << spelling << "(";
  for (unsigned i = 0; i < tensors; ++i)
    source << "%a" << i << ":memref<?xi" << describeNumeric(tensorTypes[i])->bits << ">,";
  source << "%out:" << buffer << ",%params:" << parameters
         << ") kernel attributes {spirv.entry_point_abi = "
            "#spirv.entry_point_abi<workgroup_size = "
            "[64, 1, 1]>} {\n"
            "%block=gpu.block_id x\n%thread=gpu.thread_id x\n%width=gpu.block_dim "
            "x\n"
            "%base=arith.muli %block,%width:index\n%i=arith.addi "
            "%base,%thread:index "
            "loc(\"numeric.lane\")\n"
            "%zero=arith.constant 0:index\n%fault=arith.constant "
         << words - 1
         << ":index\n"
            "%count=memref.load %params[%zero]:"
         << parameters
         << "\n%n=arith.index_cast %count:i32 to index\n"
            "%lane=arith.index_cast %i:index to i32\n%zero32=arith.constant "
            "0:i32\n"
            "%iszero=arith.cmpi eq,%lane,%zero32:i32\n%yes=arith.constant true\n"
            "%uniform=arith.constant true "
            "loc(\"numeric.uniform\")\n%uniformBad=arith.xori "
            "%uniform,%yes:i1\n"
            "%uniformFault=arith.andi %uniformBad,%iszero:i1\nscf.if "
            "%uniformFault {\n"
            "%previous=memref.atomic_rmw minu %zero32,%params[%fault]:(i32,"
         << parameters
         << ")->i32\n}\n"
            "%active=arith.cmpi ult,%i,%n:index\nscf.if %active {\n"
            "%placeholder=arith.constant 0:i64 loc(\"numeric.bits\")\n"
            "%ok=arith.constant true loc(\"numeric.valid\")\n";
  if (bits == 64)
    source << "memref.store %placeholder,%out[%i]:" << buffer << "\n";
  else
    source << "%narrow=arith.trunci %placeholder:i64 to i" << bits
           << "\nmemref.store %narrow,%out[%i]:" << buffer << "\n";
  source << "%bad=arith.xori %ok,%yes:i1\nscf.if %bad {\n"
            "%previous=memref.atomic_rmw minu %lane,%params[%fault]:(i32,"
         << parameters << ")->i32\n}\n}\ngpu.return\n}}}\n";
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  auto module = parseSourceString<ModuleOp>(source.str(), &context);
  if (!module)
    return std::unexpected(
        Diagnostic{"G001", "typed GPU shader skeleton failed validation", root.declaration});
  arith::ConstantOp placeholder, validity, uniform;
  Value index;
  gpu::GPUFuncOp kernel;
  module->walk([&](gpu::GPUFuncOp op) { kernel = op; });
  module->walk([&](Operation *op) {
    if (auto name = dyn_cast<NameLoc>(op->getLoc())) {
      if (name.getName() == "numeric.lane")
        index = op->getResult(0);
      if (name.getName() == "numeric.bits")
        placeholder = cast<arith::ConstantOp>(op);
      if (name.getName() == "numeric.valid")
        validity = cast<arith::ConstantOp>(op);
      if (name.getName() == "numeric.uniform")
        uniform = cast<arith::ConstantOp>(op);
    }
  });
  OpBuilder lane(placeholder), header(uniform);
  try {
    Emitter emitter(input, function, lane, header, kernel, index, options);
    auto result = emitter.rootCall(function, tensors);
    auto reads = emitter.reads();
    if ((reads.empty() && root.resultType.tensor) || reads.size() > UINT32_MAX - 2)
      return std::unexpected(Diagnostic{
          "G001", "typed GPU maps need a supported live-buffer binding layout", root.declaration});
    // Prove the emitted checked-fault predicates, rather than guessing from
    // input/output types: an f32 map may contain a checked integer conversion.
    llvm::DenseMap<Value, bool> proven;
    auto alwaysTrue = [&](Value rootValue) {
      std::vector<Value> pending{rootValue};
      while (!pending.empty()) {
        const auto value = pending.back();
        if (proven.contains(value)) {
          pending.pop_back();
          continue;
        }
        if (auto constant = value.getDefiningOp<arith::ConstantOp>()) {
          auto integer = dyn_cast<IntegerAttr>(constant.getValue());
          proven[value] = integer && integer.getType().isInteger(1) && integer.getValue().isOne();
        } else {
          Value left, right;
          if (auto conjunction = value.getDefiningOp<arith::AndIOp>()) {
            left = conjunction.getLhs();
            right = conjunction.getRhs();
          } else if (auto choice = value.getDefiningOp<arith::SelectOp>()) {
            left = choice.getTrueValue();
            right = choice.getFalseValue();
          } else {
            proven[value] = false;
            continue;
          }
          if (!proven.contains(left)) {
            pending.push_back(left);
            continue;
          }
          if (!proven.contains(right)) {
            pending.push_back(right);
            continue;
          }
          proven[value] = proven.lookup(left) && proven.lookup(right);
        }
      }
      return proven.lookup(rootValue);
    };
    const bool noArithmeticFault =
        element == NumericType::Float32 && alwaysTrue(result.valid) && alwaysTrue(result.uniform);
    placeholder.getResult().replaceAllUsesWith(result.value);
    validity.getResult().replaceAllUsesWith(result.valid);
    uniform.getResult().replaceAllUsesWith(result.uniform);
    placeholder.erase();
    validity.erase();
    uniform.erase();
    llvm::BitVector erased(tensors + 2, false);
    for (unsigned p = 0; p < tensors; ++p)
      if (!std::binary_search(reads.begin(), reads.end(), p))
        erased.set(p);
    if (failed(kernel.eraseArguments(erased)))
      return std::unexpected(
          Diagnostic{"G001", "cannot project typed GPU arguments", root.declaration});
    if (failed(verify(*module)))
      return std::unexpected(
          Diagnostic{"G001", "generated typed GPU IR failed verification", root.declaration});
    std::string ir;
    llvm::raw_string_ostream out(ir);
    module->print(out);
    const bool indirect = reads.size() > 29;
    if (indirect)
      ir = "// kalandra.numeric.indirect\n" + ir;
    return NumericShader{element,
                         std::move(ir),
                         std::move(reads),
                         {},
                         false,
                         !root.resultType.tensor,
                         std::move(tensorTypes),
                         std::move(scalarTypes),
                         std::move(scalarBuffers),
                         indirect,
                         {},
                         {},
                         noArithmeticFault};
  } catch (const Rejected &error) {
    return std::unexpected(error.diagnostic);
  }
}

bool usesScalarControl(const TypedModule &input, FunctionId function) {
  const auto &fn = input.functions.at(function);
  return std::ranges::any_of(fn.expressions,
                             [&](auto id) {
                               const auto &op = input.expressions.at(id).operation;
                               if (input.expressions.at(id).type.array)
                                 return true;
                               if (std::holds_alternative<WhileOperation>(op))
                                 return true;
                               if (std::holds_alternative<RecordValue>(op) ||
                                   std::holds_alternative<RecordFieldRead>(op) ||
                                   std::holds_alternative<MatchOperation>(op))
                                 return true;
                               const auto *read = std::get_if<SymbolRead>(&op);
                               return read && input.symbols.at(read->symbol).reassignable;
                             }) ||
         std::ranges::any_of(fn.body,
                             [&](const auto &statement) {
                               return statement.binding &&
                                      input.symbols.at(*statement.binding).reassignable;
                             }) ||
         std::ranges::any_of(fn.parameters,
                             [&](auto symbol) { return input.symbols.at(symbol).reassignable; });
}

std::expected<NumericShader, Diagnostic> emitScalarControlShader(const TypedModule &input,
                                                                 FunctionId function,
                                                                 ArithmeticMode arithmetic,
                                                                 bool nativeFloat64Conversions,
                                                                 bool aggregateEnvelope,
                                                                 std::optional<std::size_t> graphParameter,
                                                                 std::span<const DeviceGraphExportLayout> graphResultLayouts) {
  const auto &root = input.functions.at(function);
  const auto reject = [&](const std::string &message) -> std::expected<NumericShader, Diagnostic> {
    return std::unexpected(Diagnostic{"G001", message, root.declaration});
  };
  auto planned = continuation::plan(
      input,
      {.entry = function, .nativeSubgraphs = false, .valueScopes = true, .inlineLoopCalls = true, .deviceValues = true});
  if (!planned)
    return reject("GPU scalar control planning: " + planned.error());
  const auto &plan = *planned->plans.at(function);
  const auto scalar = [](const ValueType &type) {
    return !type.tensor && !type.array && !type.task && !type.fallible &&
           (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool);
  };
  const auto flatArray = [](const ValueType &type) {
    return type.array && type.tensor && type.shape.size() == 1 && !type.task && !type.fallible &&
           (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool);
  };
  bool storage = std::ranges::any_of(plan.slots, flatArray);
  const bool arrayResult = flatArray(root.resultType);
  const bool aggregateResult = deviceInlineRecordValue(root.resultType);
  const bool graphResult = aggregateResult && !graphResultLayouts.empty();
  if (aggregateResult && !aggregateEnvelope)
    return reject("GPU aggregate outputs need typed transport");
  const bool exportResult = arrayResult || aggregateResult;
  storage |= aggregateResult;
  if (graphParameter && (*graphParameter >= root.parameters.size() ||
                         (!scalar(root.resultType) && !graphResult) ||
                         !deviceInlineRecordValue(input.symbols.at(root.parameters[*graphParameter]).type)))
    return reject("GPU graph capture currently requires an inline record and scalar result");
  const auto graphSymbol = graphParameter ? std::optional{root.parameters[*graphParameter]}
                                          : std::optional<SymbolId>{};
  if ((!scalar(root.resultType) && !exportResult) || std::ranges::any_of(root.parameters, [&](auto symbol) {
        return symbol != graphSymbol && !scalar(input.symbols.at(symbol).type) &&
               !flatArray(input.symbols.at(symbol).type);
      }))
    return reject("GPU checkpointed entry requires scalar or flat array inputs and "
                  "scalar, flat array or inline aggregate results");
  auto aggregateLayouts = planDeviceAggregateLayouts(input, plan.slots);
  std::optional<RecordStoragePlan> nominalStorage;
  if (!aggregateLayouts) {
    auto nominal = planRecordStorage(input);
    if (nominal) {
      nominalStorage.emplace(std::move(*nominal));
      aggregateLayouts = planDeviceAggregateLayouts(input, plan.slots, 300000, &*nominalStorage);
    }
  }
  if (!aggregateLayouts)
    return reject(aggregateLayouts.error());
  const auto &layouts = *aggregateLayouts;
  const auto plainRecord = deviceInlineRecordValue;
  const auto boxIdentity = [](std::size_t id) {
    return runtime::DeviceHeapBlockABI::nominalLayout(static_cast<unsigned>(id), false);
  };
  std::set<std::size_t> boxTypes, arrayTypes;
  const auto localArray = [&](const ValueType &type) {
    return flatArray(type) || deviceRecordArrayValue(type);
  };
  const auto arrayIdentity = [&](const ValueType &type) -> unsigned {
    if (deviceRecordArrayValue(type))
      return runtime::DeviceHeapBlockABI::nominalLayout(static_cast<unsigned>(*type.record), true);
    return type.builtin == BuiltinValue::Bool ? 11 : unsigned(type.element) + 1;
  };
  const auto arrayStride = [&](const ValueType &type) -> std::size_t {
    return deviceRecordArrayValue(type) ? layouts.at(*type.record).words : 1;
  };
  if (graphSymbol)
    boxTypes.insert(*input.symbols.at(*graphSymbol).type.record);
  if (graphResult)
    boxTypes.insert(*root.resultType.record);
  for (const auto &type : plan.slots)
    if (deviceRecordArrayValue(type))
      arrayTypes.insert(*type.record);
  for (const auto &[id, layout] : layouts) {
    const auto &fields = input.records.at(id).fields;
    for (std::size_t field = 0; field < fields.size(); ++field) {
      if (deviceRecordArrayValue(fields[field].type))
        arrayTypes.insert(*fields[field].type.record);
      if (layout.boxed[field])
        boxTypes.insert(*fields[field].type.record);
    }
  }
  auto traceTypes = arrayTypes;
  traceTypes.insert(boxTypes.begin(), boxTypes.end());
  std::vector<DeviceTraceLayout> traceLayouts;
  std::size_t traceBudget = 100000;
  for (auto id : traceTypes) {
    DeviceTraceLayout trace{boxIdentity(id), static_cast<unsigned>(layouts.at(id).words), {}};
    struct Walk {
      std::size_t id, offset, next;
      bool guarded;
    };
    std::vector<Walk> stack{{id, 0, 0, false}};
    std::vector<DeviceReferenceGuard> guards;
    while (!stack.empty()) {
      auto &path = stack.back();
      const auto &record = input.records.at(path.id);
      const auto &layout = layouts.at(path.id);
      if (path.next == record.fields.size()) {
        if (path.guarded)
          guards.pop_back();
        stack.pop_back();
        continue;
      }
      if (!traceBudget)
        return reject("GPU device trace schema exceeds configured work budget");
      --traceBudget;
      const auto field = path.next++;
      const auto &type = record.fields[field].type;
      const bool handle = layout.boxed[field] || localArray(type);
      if (!handle && (!plainRecord(type) || layouts.at(*type.record).owned.empty()))
        continue;
      const auto offset = path.offset + layout.fields[field];
      const auto tag = layout.fieldTags[field];
      if (tag)
        guards.push_back({static_cast<unsigned>(path.offset), tag});
      if (handle) {
        if (guards.size() > traceBudget)
          return reject("GPU device trace schema exceeds configured work budget");
        traceBudget -= guards.size();
        trace.references.push_back({static_cast<unsigned>(offset), guards});
        if (tag)
          guards.pop_back();
      } else
        stack.push_back({*type.record, offset, 0, tag != 0});
    }
    if (boxTypes.contains(id) && arrayTypes.contains(id))
      traceLayouts.push_back(trace);
    if (arrayTypes.contains(id))
      trace.identity = runtime::DeviceHeapBlockABI::nominalLayout(static_cast<unsigned>(id), true);
    traceLayouts.push_back(std::move(trace));
  }
  storage |= !traceTypes.empty();
  for (const auto &[id, layout] : layouts) {
    (void)id;
    storage |= !layout.owned.empty();
  }
  const auto width = [&](const ValueType &type) -> std::size_t {
    return scalar(type) || localArray(type) ? 1 : layouts.at(*type.record).words;
  };
  std::vector<std::size_t> offsets;
  std::size_t frameWords = exportResult ? runtime::detail::ControlArrayExportABI::Cells :
                           storage ? unsigned(DeviceHeapEmitter::Cells) : 1;
  for (const auto &type : plan.slots) {
    offsets.push_back(frameWords);
    if (width(type) > (UINT32_MAX - 4) / 2 - frameWords)
      return reject("GPU aggregate control frame exceeds the device ABI");
    frameWords += width(type);
    if (frameWords > 300000)
      return reject("GPU aggregate control frame exceeds configured compilation work budget");
  }
  const auto boxStep = frameWords;
  if (!traceTypes.empty())
    frameWords += 2; // Record construction field and multiword read cursor.
  const auto graphCursor = frameWords;
  if (graphParameter)
    ++frameWords;
  const auto graphReturn = frameWords;
  enum GraphReturnCell { ReturnPhase, ReturnRoot, EnvelopeBase, EnvelopeBlock,
                         EnvelopeWord, EnvelopeOutput, GraphReturnCells };
  if (graphResult)
    frameWords += GraphReturnCells;
  DeviceAggregateProjection resultProjection;
  DeviceAggregateReturnFrame resultFrame{static_cast<unsigned>(frameWords), resultProjection};
  if (aggregateResult && !graphResult) {
    std::size_t budget = 300000;
    auto projected = projectDeviceAggregates(input, std::span(&root.resultType, 1), budget);
    if (!projected)
      return reject(projected.error());
    resultProjection = std::move(*projected);
    frameWords = resultFrame.end();
    if (frameWords > 300000)
      return reject("GPU aggregate result frame exceeds configured compilation work budget");
  }
  const auto arrayInputs = std::count_if(root.parameters.begin(), root.parameters.end(),
      [&](auto parameter) {
        return parameter == graphSymbol || flatArray(input.symbols.at(parameter).type);
      });
  if (std::size_t(arrayInputs) > (UINT32_MAX - 4) / 2 - frameWords)
    return reject("GPU array input metadata exceeds the device ABI");
  frameWords += arrayInputs; // Sealed actual backing lengths, never source slots.
  if (frameWords > (UINT32_MAX - 4) / 2)
    return reject("GPU scalar control parameter layout exceeds the device ABI");
  using C = continuation::Code;
  std::vector<std::vector<std::size_t>> liveRoots;
  if (storage) {
    std::vector<continuation::Slot> ownedSlots;
    for (std::size_t i = 0; i < plan.slots.size(); ++i) {
      const auto &type = plan.slots[i];
      if (localArray(type) || (plainRecord(type) && !layouts.at(*type.record).owned.empty()))
        ownedSlots.push_back(i);
    }
    auto roots = deviceLiveRoots(plan, ownedSlots);
    if (!roots)
      return reject(roots.error());
    liveRoots = std::move(*roots);
    // Liveness is solved for logical slots. Expand a live aggregate into all
    // its owned leaves; stale dead slots and overwritten values stay unrooted.
    // Inactive enum payload cells are zeroed at construction, including nested
    // records. This inline representation never imports host pointers.
    std::size_t rootBudget = 64 * 1024 * 1024 / sizeof(std::size_t);
    for (auto &row : liveRoots) {
      std::vector<std::size_t> expanded;
      for (auto slot : row) {
        const auto &type = plan.slots.at(slot);
        const auto count = localArray(type) ? 1 : layouts.at(*type.record).owned.size();
        if (count > rootBudget)
          return reject("GPU aggregate root expansion exceeds configured memory budget");
        rootBudget -= count;
        if (localArray(type))
          expanded.push_back(offsets.at(slot));
        else
          for (auto leaf : layouts.at(*type.record).owned)
            expanded.push_back(offsets.at(slot) + leaf);
      }
      row = std::move(expanded);
    }
  }
  std::vector<DeviceRootMap> allocationRoots;
  std::size_t boxRootBudget = 64 * 1024 * 1024 / sizeof(std::size_t);
  if (storage)
    for (std::size_t i = 0; i < plan.code.size(); ++i) {
      const auto &op = plan.code[i];
      if ((op.code == C::Kernel || op.code == C::CollectAppend || op.code == C::MapInit) &&
          localArray(plan.slots.at(op.output)))
        allocationRoots.push_back({i + 1, liveRoots[i]});
      if (op.code == C::Kernel) {
        const auto *record = std::get_if<RecordValue>(&input.expressions.at(op.source).operation);
        if (record) {
          const auto &layout = layouts.at(*plan.slots.at(op.output).record);
          if (std::ranges::any_of(record->fields, [&](const auto &field) {
                return layout.boxed.at(field.field);
              })) {
            if (liveRoots[i].size() > boxRootBudget ||
                layout.owned.size() > boxRootBudget - liveRoots[i].size())
              return reject("GPU boxed record roots exceed configured memory budget");
            boxRootBudget -= liveRoots[i].size() + layout.owned.size();
            auto roots = liveRoots[i];
            for (auto leaf : layout.owned)
              roots.push_back(offsets.at(op.output) + leaf);
            allocationRoots.push_back({i + 1, std::move(roots)});
          }
        }
      }
      if (aggregateResult && op.code == C::Return) {
        std::vector<std::size_t> resultRoots;
        for (auto leaf : layouts.at(*root.resultType.record).owned)
          resultRoots.push_back(offsets.at(plan.result) + leaf);
        if (graphResult)
          resultRoots.push_back(graphReturn + ReturnRoot);
        allocationRoots.push_back({i + 1, std::move(resultRoots)});
      }
    }
  const auto arrayOperation = [&](const auto &op) {
    if (!storage || op.code != C::Kernel)
      return false;
    const auto &node = input.expressions.at(op.source);
    return (localArray(node.type) && (std::holds_alternative<TensorLiteral>(node.operation) ||
                                   std::holds_alternative<ArraySlice>(node.operation) ||
                                   std::holds_alternative<ArrayConcat>(node.operation) ||
                                   std::holds_alternative<ArraySort>(node.operation))) ||
           std::holds_alternative<ArrayIndex>(node.operation) ||
           std::holds_alternative<ArrayBoundsCheck>(node.operation) ||
           std::holds_alternative<ArrayStore>(node.operation) ||
           (std::holds_alternative<CollectionQuery>(node.operation) && !op.args.empty() &&
            localArray(plan.slots.at(op.args[0])));
  };
  auto code = planned->kernels;
  std::vector<std::optional<FunctionId>> kernels;
  for (const auto &op : plan.code) {
    if (op.code == C::Kernel &&
        std::holds_alternative<ArrayStore>(input.expressions.at(op.source).operation) &&
        deviceRecordArrayValue(plan.slots.at(op.args.at(0))))
      return reject("GPU record array element assignment requires an exclusive typed store");
    if (op.code == C::Kernel && !arrayOperation(op) &&
        !std::holds_alternative<RecordValue>(input.expressions.at(op.source).operation)) {
      kernels.push_back(op.target);
      continue;
    }
    kernels.push_back({});
    if (op.code == C::MapInit || op.code == C::MapCheck || op.code == C::MapLoad ||
        op.code == C::MapStore || op.code == C::MapAdvance || arrayOperation(op) || op.code == C::CollectInit || op.code == C::CollectAppend ||
        op.code == C::CollectFinish || op.code == C::Label || op.code == C::Copy || op.code == C::Branch || op.code == C::Jump ||
        op.code == C::Return || op.code == C::Field || op.code == C::Case ||
        (op.code == C::Kernel &&
         std::holds_alternative<RecordValue>(input.expressions.at(op.source).operation)))
      continue;
    if (op.code != C::Literal && op.code != C::Unary && op.code != C::Binary)
      return reject(
          "GPU scalar control does not support task effects, recursive calls or owned values");
    TypedFunction fn{};
    fn.name = "control_cell" + std::to_string(kernels.size());
    fn.declaration = root.declaration;
    fn.resultType = plan.slots.at(op.output);
    auto node = input.expressions.at(op.source);
    std::vector<ValueId> args;
    for (auto slot : op.args) {
      const auto symbol = code.symbols.size();
      code.symbols.push_back({"cell" + std::to_string(symbol), plan.slots.at(slot),
                              root.declaration, code.functions.size(), true});
      fn.parameters.push_back(symbol);
      args.push_back(code.expressions.size());
      code.expressions.push_back({root.declaration, plan.slots.at(slot), SymbolRead{symbol}});
    }
    if (auto unary = std::get_if<ScalarUnary>(&node.operation))
      unary->operand = args.at(0);
    else if (auto binary = std::get_if<ScalarBinary>(&node.operation)) {
      binary->left = args.at(0);
      binary->right = args.at(1);
    }
    fn.result = code.expressions.size();
    code.expressions.push_back(std::move(node));
    kernels.back() = code.functions.size();
    code.functions.push_back(std::move(fn));
  }
  const auto type = exportResult ? NumericType::UInt64 :
      root.resultType.builtin == BuiltinValue::Bool ? NumericType::UInt8 : root.resultType.element;
  const auto bits = describeNumeric(type)->bits;
  const auto frames = std::uint32_t(frameWords);
  std::vector<NumericType> inputs;
  for (auto parameter : root.parameters) {
    const auto &t = input.symbols.at(parameter).type;
    inputs.push_back(t.array || parameter == graphSymbol ? NumericType::UInt64 :
                     t.builtin == BuiltinValue::Bool ? NumericType::UInt8 : t.element);
  }
  if (storage)
    inputs.push_back(NumericType::UInt64);
  std::vector<NumericType> states(frames, NumericType::UInt64);
  auto name = numericKernelName(type, inputs, states);
  std::vector<std::uint8_t> roles;
  for (auto parameter : root.parameters)
    roles.push_back(parameter == graphSymbol || flatArray(input.symbols.at(parameter).type) ? 2 : 1);
  if (storage)
    roles.push_back(exportResult ? runtime::DeviceArrayWireABI::ExportRole : 0);
  if (!root.parameters.empty() || exportResult) {
    name += "_d_";
    for (auto role : roles)
      name += role == 3 ? 'x' : role == 2 ? 'a' : role ? 's' : 't';
  }
  std::ostringstream source;
  const auto hasWidth = [&](unsigned width) {
    return bits == width || std::ranges::any_of(inputs, [&](auto type) {
             return describeNumeric(type)->bits == width;
           });
  };
  source << "module attributes {gpu.container_module, spirv.target_env = "
            "#spirv.target_env<#spirv.vce<v1.0, [Shader, Int64";
  if (hasWidth(8))
    source << ", Int8, StorageBuffer8BitAccess";
  if (hasWidth(16))
    source << ", Int16, StorageBuffer16BitAccess";
  source << "], [SPV_KHR_storage_buffer_storage_class";
  if (hasWidth(8))
    source << ", SPV_KHR_8bit_storage";
  if (hasWidth(16))
    source << ", SPV_KHR_16bit_storage";
  source << "]>, #spirv.resource_limits<>>} { gpu.module @kernels { gpu.func @" << name << "(";
  for (std::size_t i = 0; i < inputs.size(); ++i)
    source << "%a" << i << ":memref<?xi" << describeNumeric(inputs[i])->bits << ">,";
  source << "%out:memref<?xi" << bits << ">,%params:memref<" << 2 + 2 * frames
         << "xi32>) kernel attributes {"
            "spirv.entry_point_abi=#spirv.entry_point_abi<workgroup_size=[64,1,1]>} {"
            "%thread=gpu.thread_id x\n%zero=arith.constant 0:index\n"
            "%active=arith.cmpi eq,%thread,%zero:index\nscf.if %active {}\ngpu.return\n}}}";
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  context.getOrLoadDialect<memref::MemRefDialect>();
  auto module = parseSourceString<ModuleOp>(source.str(), &context);
  if (!module)
    return reject("GPU scalar control shader skeleton failed verification");
  gpu::GPUFuncOp kernel;
  scf::IfOp body;
  module->walk([&](gpu::GPUFuncOp op) { kernel = op; });
  module->walk([&](scf::IfOp op) { body = op; });
  OpBuilder b = OpBuilder::atBlockTerminator(&body.getThenRegion().front());
  const auto loc = b.getUnknownLoc();
  const auto constant = [&](std::uint64_t value, unsigned width = 64) -> Value {
    return arith::ConstantOp::create(
        b, loc, b.getIntegerType(width),
        b.getIntegerAttr(b.getIntegerType(width), llvm::APInt(width, value)));
  };
  const auto at = [&](std::uint64_t value) -> Value {
    return arith::ConstantIndexOp::create(b, loc, value);
  };
  const auto params = kernel.getArgument(inputs.size() + 1);
  const auto load = [&](std::size_t slot) -> Value {
    auto lo = memref::LoadOp::create(b, loc, params, ValueRange{at(1 + slot * 2)});
    auto hi = memref::LoadOp::create(b, loc, params, ValueRange{at(2 + slot * 2)});
    auto low = arith::ExtUIOp::create(b, loc, b.getI64Type(), lo);
    auto high = arith::ExtUIOp::create(b, loc, b.getI64Type(), hi);
    return arith::OrIOp::create(b, loc, low, arith::ShLIOp::create(b, loc, high, constant(32)));
  };
  const auto store = [&](std::size_t slot, Value value) {
    if (value.getType().isInteger(1))
      value = arith::ExtUIOp::create(b, loc, b.getI64Type(), value);
    auto lo = arith::TruncIOp::create(b, loc, b.getI32Type(), value);
    auto hi = arith::TruncIOp::create(b, loc, b.getI32Type(),
                                      arith::ShRUIOp::create(b, loc, value, constant(32)));
    memref::StoreOp::create(b, loc, lo, params, ValueRange{at(1 + slot * 2)});
    memref::StoreOp::create(b, loc, hi, params, ValueRange{at(2 + slot * 2)});
  };
  std::vector<DeviceArrayInput> captures;
  std::optional<std::size_t> graphLengthSlot;
  std::size_t wireOrdinal = 0;
  for (std::size_t i = 0; i < root.parameters.size(); ++i) {
    const auto &type = input.symbols.at(root.parameters[i]).type;
    if (i == graphParameter)
      graphLengthSlot = frames - arrayInputs + wireOrdinal++;
    else if (flatArray(type))
      captures.push_back({kernel.getArgument(i), load(frames - arrayInputs + wireOrdinal++),
          type.builtin == BuiltinValue::Bool ? 11 : unsigned(type.element) + 1});
  }
  std::optional<DeviceHeapEmitter> heap;
  if (storage)
    heap.emplace(b, kernel.getArgument(inputs.size() - 1), params, 2 + 2 * frames, allocationRoots,
                 traceLayouts, captures, graphParameter.has_value());
  auto yes = constant(1, 1);
  const auto initializeParameters = [&] {
    std::size_t capture = 0;
    for (std::size_t i = 0; i < plan.parameters.size(); ++i) {
      if (i == graphParameter)
        continue;
      if (flatArray(input.symbols.at(root.parameters[i]).type)) {
        store(offsets.at(plan.parameters[i]),
              constant(runtime::DeviceArrayWireABI::ExternalHandle | ++capture));
        continue;
      }
      Value value = memref::LoadOp::create(b, loc, kernel.getArgument(i), ValueRange{at(0)});
      if (describeNumeric(inputs[i])->bits != 64)
        value = arith::ExtUIOp::create(b, loc, b.getI64Type(), value);
      store(offsets.at(plan.parameters[i]), value);
    }
    store(0, constant(1)); // Zero is uninitialized; instruction PCs are one-based.
    for (std::size_t i = 0; i < captures.size(); ++i)
      heap->read(constant(runtime::DeviceArrayWireABI::ExternalHandle | (i + 1)), constant(1));
  };
  constexpr auto graphInit = runtime::detail::ControlArenaABI::Grow - 1;
  {
    auto initial = scf::IfOp::create(
        b, loc, arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, load(0), constant(0)), false);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&initial.getThenRegion().front());
    if (graphParameter) {
      // Zero is reserved for the backend's first dispatch. Subsequent chunks
      // and growth acknowledgements must use a nonzero continuation PC.
      store(0, constant(graphInit));
    } else {
      if (heap)
        heap->initialize();
      initializeParameters();
    }
  }
  auto loop = scf::WhileOp::create(b, loc, TypeRange{b.getI32Type()}, ValueRange{constant(0, 32)});
  auto *before = new Block;
  loop.getBefore().push_back(before);
  before->addArgument(b.getI32Type(), loc);
  auto *after = new Block;
  loop.getAfter().push_back(after);
  after->addArgument(b.getI32Type(), loc);
  try {
    OpBuilder::InsertionGuard restore(b);
    b.setInsertionPointToStart(before);
    auto budget = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, before->getArgument(0),
                                        constant(256, 32));
    auto running = arith::CmpIOp::create(b, loc,
        storage ? arith::CmpIPredicate::ult : arith::CmpIPredicate::ne,
        load(0), constant(storage ? UINT64_MAX - 1 : UINT64_MAX));
    scf::ConditionOp::create(b, loc, arith::AndIOp::create(b, loc, budget, running),
                             before->getArguments());
    b.setInsertionPointToStart(after);
    if (graphParameter) {
      using W = runtime::DeviceArrayWireABI;
      constexpr unsigned imageOffset = W::HeaderWords + 1;
      const auto equal = [&](Value left, Value right) -> Value {
        return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, left, right);
      };
      auto initial = scf::IfOp::create(b, loc, equal(load(0), constant(graphInit)), false);
      OpBuilder::InsertionGuard initialGuard(b);
      b.setInsertionPointToStart(&initial.getThenRegion().front());
      const auto cursor = load(graphCursor);
      const auto image = kernel.getArgument(*graphParameter);
      auto phase = scf::IfOp::create(b, loc, equal(cursor, constant(0)), true);
      b.setInsertionPointToStart(&phase.getThenRegion().front());
      auto ready = scf::IfOp::create(b, loc, heap->seed(image, load(*graphLengthSlot), imageOffset), false);
      b.setInsertionPointToStart(&ready.getThenRegion().front());
      store(graphCursor, constant(1));
      b.setInsertionPointToStart(&phase.getElseRegion().front());
      const auto parameter = plan.parameters.at(*graphParameter);
      const auto &type = plan.slots.at(parameter);
      const auto count = width(type);
      const Value handle = memref::LoadOp::create(b, loc, image, ValueRange{at(W::HeaderWords)});
      const Value imageWords = arith::SubIOp::create(b, loc, load(*graphLengthSlot), constant(imageOffset));
      const Value remaining = arith::SubIOp::create(b, loc, imageWords, handle);
      const Value inRange = arith::AndIOp::create(b, loc,
          arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, handle, imageWords),
          arith::CmpIOp::create(b, loc, arith::CmpIPredicate::uge, remaining, constant(count + 4)));
      auto bound = scf::IfOp::create(b, loc, inRange, true);
      b.setInsertionPointToStart(&bound.getThenRegion().front());
      const Value valid = arith::AndIOp::create(b, loc,
          equal(heap->read(handle, constant(1)), constant(boxIdentity(*type.record))),
          equal(heap->read(handle, constant(2)), constant(count)));
      auto checked = scf::IfOp::create(b, loc, valid, true);
      b.setInsertionPointToStart(&checked.getThenRegion().front());
      const Value word = arith::SubIOp::create(b, loc, cursor, constant(1));
      heap->storeAt(arith::AddIOp::create(b, loc, constant(offsets.at(parameter)), word),
                    heap->read(handle, arith::AddIOp::create(b, loc, constant(4), word)));
      store(graphCursor, arith::AddIOp::create(b, loc, cursor, constant(1)));
      auto done = scf::IfOp::create(b, loc, equal(cursor, constant(count)), false);
      b.setInsertionPointToStart(&done.getThenRegion().front());
      initializeParameters();
      b.setInsertionPointToStart(&checked.getElseRegion().front());
      heap->fault(99);
      b.setInsertionPointToStart(&bound.getElseRegion().front());
      heap->fault(99);
    }
    Value sourceReady = heap ? heap->service(graphResult) : yes;
    if (graphResult)
      sourceReady = arith::OrIOp::create(b, loc, sourceReady, heap->resultTraceReady());
    const auto pc = load(0);
    const NumericShaderOptions options{.arithmetic = arithmetic,
                                       .nativeFloat64Conversions = nativeFloat64Conversions};
    TypedFunction contextFunction{};
    contextFunction.declaration = root.declaration;
    const auto contextId = code.functions.size();
    code.functions.push_back(std::move(contextFunction));
    Emitter emitter(code, contextId, b, b, kernel, at(0), options);
    std::size_t aggregateWork = 300000;
    const auto chargeAggregate = [&](std::size_t words) {
      if (words > aggregateWork)
        throw Rejected{{"G001", "GPU aggregate copies exceed configured compilation work budget",
                        root.declaration}};
      aggregateWork -= words;
    };
    for (std::size_t i = 0; i < plan.code.size(); ++i) {
      const auto &op = plan.code[i];
      auto selected = scf::IfOp::create(
          b, loc, arith::AndIOp::create(b, loc, sourceReady,
              arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, pc, constant(i + 1))), false);
      OpBuilder::InsertionGuard guard(b);
      b.setInsertionPointToStart(&selected.getThenRegion().front());
      Value next = constant(i + 2);
      const auto read = [&](std::size_t slot) {
        auto raw = load(offsets.at(slot));
        const auto &t = plan.slots.at(slot);
        Value value =
            t.builtin == BuiltinValue::Bool
                ? Value(arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ne, raw, constant(0)))
                : raw;
        ShaderValue result{value, yes, yes, false, t.builtin == BuiltinValue::Bool};
        result.element = t.element;
        return result;
      };
      const auto copyWords = [&](std::size_t destination, std::size_t source, std::size_t count) {
        chargeAggregate(count);
        std::vector<Value> values;
        for (std::size_t word = 0; word < count; ++word)
          values.push_back(load(source + word));
        for (std::size_t word = 0; word < count; ++word)
          store(destination + word, values[word]);
      };
      const auto stepElement = [&](std::size_t stride, auto emit) -> Value {
        if (stride == 1) {
          emit(constant(0));
          return yes;
        }
        const auto word = load(boxStep + 1);
        emit(word);
        const Value advanced = arith::AddIOp::create(b, loc, word, constant(1));
        const Value done = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq,
                                                advanced, constant(stride));
        store(boxStep + 1, arith::SelectOp::create(b, loc, done, constant(0), advanced));
        return done;
      };
      const auto wordOffset = [&](Value index, std::size_t stride) -> Value {
        if (stride == 1)
          return index;
        return arith::MulIOp::create(b, loc, index, constant(stride));
      };
      if (op.code == C::MapInit || op.code == C::MapCheck || op.code == C::MapLoad ||
          op.code == C::MapStore || op.code == C::MapAdvance) {
        const auto &map = std::get<ArrayMap>(input.expressions.at(op.source).operation);
        const auto compare = [&](arith::CmpIPredicate pred, Value a, Value d) -> Value {
          return arith::CmpIOp::create(b, loc, pred, a, d);
        };
        const auto condition = [&](Value predicate, auto body) {
          auto branch = scf::IfOp::create(b, loc, predicate, false);
          OpBuilder::InsertionGuard restore(b);
          b.setInsertionPointToStart(&branch.getThenRegion().front());
          body();
        };
        if (op.code == C::MapAdvance) {
          store(offsets.at(op.output), arith::AddIOp::create(b, loc,
              load(offsets.at(op.args.at(0))), constant(1)));
        } else if (op.code == C::MapStore) {
          const auto handle = load(offsets.at(op.args.at(0)));
          const auto stride = arrayStride(plan.slots.at(op.args.at(0)));
          const auto base = wordOffset(load(offsets.at(op.args.at(1))), stride);
          const auto source = offsets.at(op.args.at(2));
          const auto done = stepElement(stride, [&](Value word) {
            const Value address = arith::AddIOp::create(b, loc, base, word);
            heap->write(handle, arith::AddIOp::create(b, loc, constant(4), address),
                        stride == 1 ? load(source) :
                            heap->loadAt(arith::AddIOp::create(b, loc, constant(source), word)));
          });
          if (stride != 1)
            next = arith::SelectOp::create(b, loc, done, next, pc);
        } else {
          const auto inputValue = load(offsets.at(op.args.at(0)));
          Value count = inputValue;
          if (!map.generate) {
            auto length = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()},
                compare(arith::CmpIPredicate::ne, inputValue, constant(0)), true);
            {
              OpBuilder::InsertionGuard restore(b);
              b.setInsertionPointToStart(&length.getThenRegion().front());
              const auto &type = plan.slots.at(op.args.at(0));
              condition(compare(arith::CmpIPredicate::ne, heap->read(inputValue, constant(1)),
                                constant(arrayIdentity(type))), [&] { heap->fault(99); });
              Value countWords = heap->read(inputValue, constant(2));
              const auto stride = arrayStride(type);
              if (stride != 1) {
                const Value remainder = arith::RemUIOp::create(b, loc, countWords, constant(stride));
                condition(compare(arith::CmpIPredicate::ne, remainder, constant(0)),
                          [&] { heap->fault(99); });
                countWords = arith::DivUIOp::create(b, loc, countWords, constant(stride));
              }
              scf::YieldOp::create(b, loc, ValueRange{countWords});
              b.setInsertionPointToStart(&length.getElseRegion().front());
              scf::YieldOp::create(b, loc, ValueRange{constant(0)});
            }
            count = length.getResult(0);
          }
          const auto index = load(offsets.at(op.args.at(1)));
          if (op.code == C::MapCheck) {
            store(offsets.at(op.output), arith::ExtUIOp::create(b, loc, b.getI64Type(),
                compare(arith::CmpIPredicate::ult, index, count)));
          } else if (op.code == C::MapLoad) {
            if (map.generate) {
              store(offsets.at(op.output), index);
            } else {
              const auto stride = arrayStride(plan.slots.at(op.args.at(0)));
              const auto base = wordOffset(index, stride);
              const auto destination = offsets.at(op.output);
              const auto done = stepElement(stride, [&](Value word) {
                const Value address = arith::AddIOp::create(b, loc, base, word);
                auto value = heap->read(inputValue, arith::AddIOp::create(b, loc, constant(4), address));
                if (stride == 1)
                  store(destination, value);
                else
                  heap->storeAt(arith::AddIOp::create(b, loc, constant(destination), word), value);
              });
              if (stride != 1)
                next = arith::SelectOp::create(b, loc, done, next, pc);
            }
          } else {
            store(offsets.at(op.args.at(1)), constant(0));
            if (map.generate)
              condition(compare(arith::CmpIPredicate::slt, count, constant(0)), [&] { heap->fault(1); });
            const auto &type = plan.slots.at(op.output);
            const auto stride = arrayStride(type);
            Value words = count;
            if (stride != 1) {
              constexpr auto limit = runtime::detail::ControlArenaABI::MaxWords;
              const auto fits = compare(arith::CmpIPredicate::ule, count, constant(limit / stride));
              words = arith::SelectOp::create(b, loc, fits, wordOffset(count, stride), constant(limit));
            }
            const auto empty = compare(arith::CmpIPredicate::eq, count, constant(0));
            const auto ready = heap->ready(), cursor = heap->cursor();
            const Value complete = arith::AndIOp::create(b, loc, ready,
                compare(arith::CmpIPredicate::eq, cursor, words));
            condition(empty, [&] { store(offsets.at(op.output), constant(0)); });
            condition(arith::AndIOp::create(b, loc,
                compare(arith::CmpIPredicate::eq, heap->load(DeviceHeapEmitter::Phase), constant(0)),
                compare(arith::CmpIPredicate::sgt, count, constant(0))), [&] {
              heap->begin(words, arrayIdentity(type));
            });
            condition(ready, [&] {
              condition(compare(arith::CmpIPredicate::ult, cursor, words), [&] {
                heap->write(heap->handle(), arith::AddIOp::create(b, loc, constant(4), cursor), constant(0));
                heap->advance();
              });
              condition(complete, [&] {
                store(offsets.at(op.output), heap->handle());
                heap->finish();
              });
            });
            next = arith::SelectOp::create(b, loc, arith::OrIOp::create(b, loc, complete, empty),
                                           next, constant(i + 1));
          }
        }
      } else if (op.code == C::CollectInit) {
        store(offsets.at(op.output), constant(0));
      } else if (op.code == C::CollectFinish) {
        // The builder already carries its logical length. Its spare capacity
        // stays private; publication does not expose uninitialized cells.
      } else if (op.code == C::CollectAppend) {
        const auto condition = [&](Value predicate, auto body) {
          auto branch = scf::IfOp::create(b, loc, predicate, false);
          OpBuilder::InsertionGuard restore(b);
          b.setInsertionPointToStart(&branch.getThenRegion().front());
          body();
        };
        const auto compare = [&](arith::CmpIPredicate pred, Value a, Value d) -> Value {
          return arith::CmpIOp::create(b, loc, pred, a, d);
        };
        const auto old = load(offsets.at(op.output));
        auto descriptor = scf::IfOp::create(b, loc, TypeRange{b.getI64Type(), b.getI64Type()},
            compare(arith::CmpIPredicate::ne, old, constant(0)), true);
        {
          OpBuilder::InsertionGuard restore(b);
          b.setInsertionPointToStart(&descriptor.getThenRegion().front());
          auto length = heap->read(old, constant(2));
          auto capacity = arith::SubIOp::create(b, loc, heap->read(old, constant(0)), constant(4));
          scf::YieldOp::create(b, loc, ValueRange{length, capacity});
          b.setInsertionPointToStart(&descriptor.getElseRegion().front());
          scf::YieldOp::create(b, loc, ValueRange{constant(0), constant(0)});
        }
        const auto length = descriptor.getResult(0), capacity = descriptor.getResult(1);
        const auto &type = plan.slots.at(op.output);
        const auto stride = arrayStride(type);
        const Value remaining = arith::SubIOp::create(b, loc, capacity, length);
        const auto needsGrowth = compare(arith::CmpIPredicate::ult, remaining, constant(stride));
        const auto ready = heap->ready();
        const auto cursor = heap->cursor();
        const auto copied = compare(arith::CmpIPredicate::eq, cursor, length);
        const Value copyDone = arith::AndIOp::create(b, loc, ready, copied);
        const Value appendReady = arith::OrIOp::create(b, loc, copyDone,
            arith::XOrIOp::create(b, loc, needsGrowth, yes));
        condition(needsGrowth, [&] {
          condition(compare(arith::CmpIPredicate::eq, heap->load(DeviceHeapEmitter::Phase), constant(0)), [&] {
            auto size = arith::MaxUIOp::create(b, loc, constant(std::max<std::size_t>(4, stride)),
                arith::AddIOp::create(b, loc, capacity, capacity));
            heap->begin(size, arrayIdentity(type));
          });
          const Value copying = arith::AndIOp::create(b, loc, ready,
              compare(arith::CmpIPredicate::ult, cursor, length));
          condition(copying, [&] {
            const Value offset = arith::AddIOp::create(b, loc, constant(4), cursor);
            heap->write(heap->handle(), offset, heap->read(old, offset));
            heap->advance();
          });
        });
        const Value last = stride == 1 ? yes :
            compare(arith::CmpIPredicate::eq, load(boxStep + 1), constant(stride - 1));
        condition(appendReady, [&] {
          const Value handle = arith::SelectOp::create(b, loc, needsGrowth, heap->handle(), old);
          const auto source = offsets.at(op.args.at(0));
          const auto done = stepElement(stride, [&](Value word) {
            const Value offset = arith::AddIOp::create(b, loc, length, word);
            heap->write(handle, arith::AddIOp::create(b, loc, constant(4), offset),
                        stride == 1 ? load(source) :
                            heap->loadAt(arith::AddIOp::create(b, loc, constant(source), word)));
          });
          condition(done, [&] {
            heap->write(handle, constant(2), arith::AddIOp::create(b, loc, length, constant(stride)));
            condition(needsGrowth, [&] {
              store(offsets.at(op.output), handle);
              heap->finish();
            });
          });
        });
        const Value finished = arith::AndIOp::create(b, loc, appendReady, last);
        next = arith::SelectOp::create(b, loc, finished, next, pc);
      } else if (arrayOperation(op)) {
        const auto &node = input.expressions.at(op.source);
        const auto condition = [&](Value predicate, auto body) {
          auto branch = scf::IfOp::create(b, loc, predicate, false);
          OpBuilder::InsertionGuard restore(b);
          b.setInsertionPointToStart(&branch.getThenRegion().front());
          body();
        };
        const auto compare = [&](arith::CmpIPredicate pred, Value a, Value d) -> Value {
          return arith::CmpIOp::create(b, loc, pred, a, d);
        };
        const auto wordsFor = [&](Value elements, const ValueType &type) -> Value {
          const auto stride = arrayStride(type);
          return stride == 1 ? elements :
              Value(arith::MulIOp::create(b, loc, elements, constant(stride)));
        };
        const auto lengthOf = [&](std::size_t slot) -> Value {
          const auto h = load(offsets.at(slot));
          auto branch = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()},
              compare(arith::CmpIPredicate::ne, h, constant(0)), true);
          {
            OpBuilder::InsertionGuard restore(b);
            b.setInsertionPointToStart(&branch.getThenRegion().front());
            const auto layout = heap->read(h, constant(1));
            const auto &type = plan.slots.at(slot);
            condition(compare(arith::CmpIPredicate::ne, layout, constant(arrayIdentity(type))),
                      [&] { heap->fault(99); });
            Value length = heap->read(h, constant(2));
            const auto stride = arrayStride(type);
            if (stride != 1) {
              const Value remainder = arith::RemUIOp::create(b, loc, length, constant(stride));
              condition(compare(arith::CmpIPredicate::ne, remainder, constant(0)),
                        [&] { heap->fault(99); });
              length = arith::DivUIOp::create(b, loc, length, constant(stride));
            }
            scf::YieldOp::create(b, loc, ValueRange{length});
            b.setInsertionPointToStart(&branch.getElseRegion().front());
            scf::YieldOp::create(b, loc, ValueRange{constant(0)});
          }
          return branch.getResult(0);
        };
        const auto indexOf = [&](std::size_t slot) -> Value {
          Value index = load(offsets.at(slot));
          const auto desc = describeNumeric(plan.slots.at(slot).element);
          if (desc->family == NumericFamily::SignedInteger && desc->bits < 64)
            index = arith::ExtSIOp::create(b, loc, b.getI64Type(),
                arith::TruncIOp::create(b, loc, b.getIntegerType(desc->bits), index));
          return index;
        };
        if (const auto *sort = std::get_if<ArraySort>(&node.operation)) {
          const auto count = lengthOf(op.args.at(0));
          const auto phase = heap->load(DeviceHeapEmitter::SortPhase);
          next = constant(i + 1);
          condition(compare(arith::CmpIPredicate::ne, phase, constant(0)), [&] {
            const auto &type = plan.slots.at(op.output);
            auto done = heap->sortStep(type.element, type.builtin == BuiltinValue::Bool);
            condition(done, [&] {
              store(offsets.at(op.output), heap->load(DeviceHeapEmitter::SortHandle));
            });
          });
          // The source PC remains here until sorting resets its phase. Starting
          // a sort does not execute an unbounded native helper or host algorithm.
          condition(compare(arith::CmpIPredicate::eq, phase, constant(0)), [&] {
            if (!sort->copying) {
              const auto handle = load(offsets.at(op.args.at(0)));
              heap->requireWritable(handle);
              heap->beginSort(handle, count);
            } else {
              const auto empty = compare(arith::CmpIPredicate::eq, count, constant(0));
              const auto ready = heap->ready(), cursor = heap->cursor();
              const Value complete = arith::AndIOp::create(b, loc, ready,
                  compare(arith::CmpIPredicate::eq, cursor, count));
              condition(empty, [&] { heap->beginSort(constant(0), constant(0)); });
              condition(arith::AndIOp::create(b, loc,
                  compare(arith::CmpIPredicate::eq, heap->load(DeviceHeapEmitter::Phase), constant(0)),
                  arith::XOrIOp::create(b, loc, empty, yes)), [&] {
                const auto &type = plan.slots.at(op.output);
                heap->begin(count, type.builtin == BuiltinValue::Bool ? 11 : unsigned(type.element) + 1);
              });
              condition(ready, [&] {
                condition(compare(arith::CmpIPredicate::ult, cursor, count), [&] {
                  const Value offset = arith::AddIOp::create(b, loc, constant(4), cursor);
                  heap->write(heap->handle(), offset,
                      heap->read(load(offsets.at(op.args.at(0))), offset));
                  heap->advance();
                });
                condition(complete, [&] {
                  heap->beginSort(heap->handle(), count);
                  heap->finish();
                });
              });
            }
          });
          const Value done = arith::AndIOp::create(b, loc,
              compare(arith::CmpIPredicate::ne, phase, constant(0)),
              compare(arith::CmpIPredicate::eq, heap->load(DeviceHeapEmitter::SortPhase), constant(0)));
          next = arith::SelectOp::create(b, loc, done, constant(i + 2), constant(i + 1));
        } else if (localArray(node.type)) {
          const auto stride = arrayStride(node.type);
          const bool literal = std::holds_alternative<TensorLiteral>(node.operation);
          const bool slice = std::holds_alternative<ArraySlice>(node.operation);
          const bool shape = std::holds_alternative<CollectionQuery>(node.operation);
          Value count, leftLength, start, shapeLength;
          if (literal) {
            chargeAggregate(op.args.size());
            count = constant(op.args.size());
          } else if (shape) {
            count = constant(1);
            shapeLength = lengthOf(op.args.at(0));
          } else if (slice) {
            const auto length = lengthOf(op.args.at(0));
            start = indexOf(op.args.at(1));
            const auto end = indexOf(op.args.at(2));
            auto valid = arith::AndIOp::create(b, loc,
                compare(arith::CmpIPredicate::ule, start, end),
                compare(arith::CmpIPredicate::ule, end, length));
            condition(arith::XOrIOp::create(b, loc, valid, yes), [&] { heap->fault(1); });
            const Value distance = arith::SubIOp::create(b, loc, end, start);
            count = arith::SelectOp::create(b, loc, valid, distance, constant(0));
            start = wordsFor(start, node.type);
          } else {
            leftLength = lengthOf(op.args.at(0));
            count = arith::AddIOp::create(b, loc, leftLength, lengthOf(op.args.at(1)));
            leftLength = wordsFor(leftLength, node.type);
          }
          count = wordsFor(count, node.type);
          const auto ready = heap->ready();
          const auto cursor = heap->cursor();
          const auto empty = compare(arith::CmpIPredicate::eq, count, constant(0));
          const Value complete = arith::AndIOp::create(b, loc, ready,
              compare(arith::CmpIPredicate::eq, cursor, count));
          condition(empty, [&] { store(offsets.at(op.output), constant(0)); });
          condition(arith::AndIOp::create(b, loc,
              compare(arith::CmpIPredicate::eq, heap->load(DeviceHeapEmitter::Phase), constant(0)),
              arith::XOrIOp::create(b, loc, empty, yes)), [&] {
            heap->begin(count, arrayIdentity(node.type));
          });
          condition(ready, [&] {
            condition(compare(arith::CmpIPredicate::ult, cursor, count), [&] {
              if (literal) {
                Value element = cursor, word = constant(0);
                if (stride != 1) {
                  element = arith::DivUIOp::create(b, loc, cursor, constant(stride));
                  word = arith::RemUIOp::create(b, loc, cursor, constant(stride));
                }
                for (std::size_t j = 0; j < op.args.size(); ++j)
                  condition(compare(arith::CmpIPredicate::eq, element, constant(j)), [&] {
                    const auto value = stride == 1 ? load(offsets.at(op.args[j])) :
                        heap->loadAt(arith::AddIOp::create(b, loc, constant(offsets.at(op.args[j])), word));
                    heap->write(heap->handle(), arith::AddIOp::create(b, loc, constant(4), cursor), value);
                  });
              } else {
                Value item;
                if (shape)
                  item = shapeLength;
                else if (slice)
                  item = heap->read(load(offsets.at(op.args.at(0))),
                      arith::AddIOp::create(b, loc, constant(4),
                          arith::AddIOp::create(b, loc, start, cursor)));
                else {
                  auto choose = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()},
                      compare(arith::CmpIPredicate::ult, cursor, leftLength), true);
                  {
                    OpBuilder::InsertionGuard restore(b);
                    b.setInsertionPointToStart(&choose.getThenRegion().front());
                    scf::YieldOp::create(b, loc, ValueRange{heap->read(load(offsets.at(op.args.at(0))),
                        arith::AddIOp::create(b, loc, constant(4), cursor))});
                    b.setInsertionPointToStart(&choose.getElseRegion().front());
                    scf::YieldOp::create(b, loc, ValueRange{heap->read(load(offsets.at(op.args.at(1))),
                        arith::AddIOp::create(b, loc, constant(4),
                            arith::SubIOp::create(b, loc, cursor, leftLength)))});
                  }
                  item = choose.getResult(0);
                }
                heap->write(heap->handle(), arith::AddIOp::create(b, loc, constant(4), cursor), item);
              }
              heap->advance();
            });
            condition(complete, [&] {
              store(offsets.at(op.output), heap->handle());
              heap->finish();
            });
          });
          next = arith::SelectOp::create(b, loc, arith::OrIOp::create(b, loc, complete, empty),
                                         next, constant(i + 1));
        } else {
          const auto h = load(offsets.at(op.args.at(0)));
          const auto length = lengthOf(op.args.at(0));
          if (std::holds_alternative<CollectionQuery>(node.operation)) {
            if (std::get<CollectionQuery>(node.operation).kind != CollectionQueryKind::Length)
              return reject("GPU device-array shape queries are not implemented");
            store(offsets.at(op.output), length);
          } else {
            const Value index = indexOf(op.args.at(1));
            const auto valid = compare(arith::CmpIPredicate::ult, index, length);
            condition(arith::XOrIOp::create(b, loc, valid, yes), [&] { heap->fault(1); });
            const auto &type = plan.slots.at(op.args.at(0));
            const auto stride = arrayStride(type);
            const bool paged = std::holds_alternative<ArrayIndex>(node.operation) && stride != 1;
            Value word, advanced, done;
            if (paged) {
              word = load(boxStep + 1);
              advanced = arith::AddIOp::create(b, loc, word, constant(1));
              done = compare(arith::CmpIPredicate::eq, advanced, constant(stride));
              next = arith::SelectOp::create(b, loc, done, next, pc);
            }
            condition(valid, [&] {
              if (std::holds_alternative<ArrayBoundsCheck>(node.operation))
                store(offsets.at(op.output), index);
              else if (std::holds_alternative<ArrayStore>(node.operation)) {
                heap->writeSource(h, arith::AddIOp::create(b, loc, index, constant(4)),
                            load(offsets.at(op.args.at(2))));
                store(offsets.at(op.output), constant(0));
              } else {
                const auto offset = wordsFor(index, type);
                if (!paged) {
                  store(offsets.at(op.output),
                        heap->read(h, arith::AddIOp::create(b, loc, offset, constant(4))));
                } else {
                  const Value location = arith::AddIOp::create(b, loc, offset, word);
                  heap->storeAt(arith::AddIOp::create(b, loc, constant(offsets.at(op.output)), word),
                                heap->read(h, arith::AddIOp::create(b, loc, location, constant(4))));
                  store(boxStep + 1, arith::SelectOp::create(b, loc, done, constant(0), advanced));
                }
              }
            });
          }
        }
      } else if (op.code == C::Copy)
        copyWords(offsets.at(op.output), offsets.at(op.args.at(0)),
                  width(plan.slots.at(op.output)));
      else if (op.code == C::Field) {
        const auto source = op.args.at(0);
        const auto &layout = layouts.at(*plan.slots.at(source).record);
        const auto field = offsets.at(source) + layout.fields.at(op.target);
        const auto count = width(plan.slots.at(op.output));
        if (!layout.boxed.at(op.target)) {
          copyWords(offsets.at(op.output), field, count);
        } else {
          const auto handle = load(field);
          const auto word = load(boxStep + 1);
          const auto equal = [&](Value a, Value d) -> Value {
            return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, a, d);
          };
          auto valid = arith::AndIOp::create(b, loc,
              equal(heap->read(handle, constant(1)), constant(boxIdentity(*plan.slots.at(op.output).record))),
              equal(heap->read(handle, constant(2)), constant(count)));
          auto check = scf::IfOp::create(b, loc, valid, true);
          {
            OpBuilder::InsertionGuard restore(b);
            b.setInsertionPointToStart(&check.getThenRegion().front());
            heap->storeAt(arith::AddIOp::create(b, loc, constant(offsets.at(op.output)), word),
                          heap->read(handle, arith::AddIOp::create(b, loc, constant(4), word)));
            b.setInsertionPointToStart(&check.getElseRegion().front());
            heap->fault(99);
          }
          const Value advanced = arith::AddIOp::create(b, loc, word, constant(1));
          const auto done = equal(advanced, constant(count));
          store(boxStep + 1, arith::SelectOp::create(b, loc, done, constant(0), advanced));
          next = arith::SelectOp::create(b, loc, done, next, pc);
        }
      } else if (op.code == C::Case) {
        auto tag = load(offsets.at(op.args.at(0)));
        auto matches =
            arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, tag, constant(op.target));
        if (op.jump == SIZE_MAX) {
          auto failed =
              scf::IfOp::create(b, loc, arith::XOrIOp::create(b, loc, matches, yes), false);
          OpBuilder::InsertionGuard failedGuard(b);
          b.setInsertionPointToStart(&failed.getThenRegion().front());
          memref::StoreOp::create(b, loc, constant(0, 32), params, ValueRange{at(1 + 2 * frames)});
        }
        next = arith::SelectOp::create(b, loc, matches, next,
                                       constant(op.jump == SIZE_MAX ? UINT64_MAX : op.jump + 1));
      } else if (op.code == C::Kernel &&
                 std::holds_alternative<RecordValue>(input.expressions.at(op.source).operation)) {
        const auto &record = std::get<RecordValue>(input.expressions.at(op.source).operation);
        const auto destination = offsets.at(op.output);
        const auto &layout = layouts.at(*plan.slots.at(op.output).record);
        const bool boxes = std::ranges::any_of(record.fields, [&](const auto &field) {
          return layout.boxed.at(field.field);
        });
        const auto initialize = [&] {
          std::size_t argument = 0;
          if (record.base) {
            copyWords(destination, offsets.at(op.args.at(argument++)), layout.words);
          } else {
            chargeAggregate(layout.words);
            for (std::size_t word = 0; word < layout.words; ++word)
              store(destination + word, constant(0));
            if (record.variant)
              store(destination, constant(*record.variant + 1));
          }
          for (const auto &field : record.fields) {
            const auto source = op.args.at(argument++);
            if (!layout.boxed.at(field.field))
              copyWords(destination + layout.fields.at(field.field), offsets.at(source),
                        width(plan.slots.at(source)));
          }
        };
        if (!boxes) {
          initialize();
        } else {
          const auto equal = [&](Value a, Value d) -> Value {
            return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, a, d);
          };
          const auto when = [&](Value condition, auto emit) {
            auto branch = scf::IfOp::create(b, loc, condition, false);
            OpBuilder::InsertionGuard restore(b);
            b.setInsertionPointToStart(&branch.getThenRegion().front());
            emit();
          };
          const auto step = load(boxStep);
          when(equal(step, constant(0)), [&] {
            initialize();
            store(boxStep, constant(1));
          });
          std::size_t stepNumber = 0;
          std::size_t argument = record.base ? 1 : 0;
          for (const auto &field : record.fields) {
            const auto source = op.args.at(argument++);
            if (!layout.boxed.at(field.field))
              continue;
            chargeAggregate(1);
            const auto stepId = ++stepNumber;
            const auto count = width(plan.slots.at(source));
            when(equal(step, constant(stepId)), [&] {
              const auto phase = heap->load(DeviceHeapEmitter::Phase);
              when(equal(phase, constant(0)), [&] {
                heap->begin(constant(count), boxIdentity(*plan.slots.at(source).record));
              });
              when(heap->ready(), [&] {
                const auto word = heap->cursor();
                const auto done = equal(word, constant(count));
                when(arith::XOrIOp::create(b, loc, done, yes), [&] {
                  heap->write(heap->handle(), arith::AddIOp::create(b, loc, constant(4), word),
                              heap->loadAt(arith::AddIOp::create(b, loc, constant(offsets.at(source)), word)));
                  heap->advance();
                });
                when(done, [&] {
                  store(destination + layout.fields.at(field.field), heap->handle());
                  heap->finish();
                  store(boxStep, constant(stepId + 1));
                });
              });
            });
          }
          const auto done = equal(step, constant(stepNumber + 1));
          when(done, [&] { store(boxStep, constant(0)); });
          next = arith::SelectOp::create(b, loc, done, next, pc);
        }
      } else if (op.code == C::Branch)
        next =
            arith::SelectOp::create(b, loc, read(op.args.at(0)).value, next, constant(op.jump + 1));
      else if (op.code == C::Jump)
        next = constant(op.jump + 1);
      else if (op.code == C::Return) {
        if (arrayResult)
          emitDeviceArrayReturn(*heap, b, load(offsets.at(plan.result)),
              root.resultType.builtin == BuiltinValue::Bool ? NumericType::UInt8 : root.resultType.element,
              root.resultType.builtin == BuiltinValue::Bool);
        if (graphResult) {
          using A = runtime::detail::ControlArenaABI;
          using G = runtime::DeviceGraphWireABI;
          const auto phase = load(graphReturn + ReturnPhase);
          const auto equal = [&](Value left, Value right) -> Value {
            return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, left, right);
          };
          const auto when = [&](Value predicate, auto emit) {
            auto branch = scf::IfOp::create(b, loc, predicate, false);
            OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(&branch.getThenRegion().front());
            emit();
          };
          const auto count = width(root.resultType);
          when(equal(phase, constant(0)), [&] {
            when(equal(heap->load(DeviceHeapEmitter::Phase), constant(0)), [&] {
              heap->begin(constant(count), boxIdentity(*root.resultType.record));
            });
            when(heap->ready(), [&] {
              const auto word = heap->cursor();
              const auto copied = equal(word, constant(count));
              when(arith::XOrIOp::create(b, loc, copied, yes), [&] {
                heap->write(heap->handle(), arith::AddIOp::create(b, loc, constant(4), word),
                    heap->loadAt(arith::AddIOp::create(b, loc, constant(offsets.at(plan.result)), word)));
                heap->advance();
              });
              when(copied, [&] {
                store(graphReturn + ReturnRoot, heap->handle());
                heap->finish();
                store(graphReturn + ReturnPhase, constant(1));
              });
            });
          });
          when(equal(phase, constant(1)), [&] {
            heap->beginResultTrace();
            store(graphReturn + ReturnPhase, constant(2));
          });
          when(equal(phase, constant(2)), [&] {
            when(heap->numberResultBlocks(), [&] { store(graphReturn + ReturnPhase, constant(3)); });
          });
          when(equal(phase, constant(3)), [&] {
            const Value directory = arith::MulIOp::create(b, loc, heap->resultNodeCount(),
                                                          constant(G::DirectoryWords));
            Value words = arith::AddIOp::create(b, loc, heap->resultPayloadWords(),
                arith::AddIOp::create(b, loc, directory, constant(G::HeaderWords)));
            if (graphParameter)
              words = arith::AddIOp::create(b, loc, words,
                  arith::AddIOp::create(b, loc, constant(2), heap->resultNodeCount()));
            when(heap->reserveResultEnvelope(words, graphReturn + EnvelopeBase), [&] {
              store(graphReturn + ReturnPhase, constant(4));
            });
          });
          when(equal(phase, constant(4)), [&] {
            when(heap->writeResultEnvelope(load(graphReturn + ReturnRoot), graphReturn + EnvelopeBase,
                    graphReturn + EnvelopeBlock, graphReturn + EnvelopeWord,
                    graphReturn + EnvelopeOutput, graphResultLayouts, graphParameter.has_value()), [&] {
              emitDeviceArrayReturn(*heap, b, load(graphReturn + EnvelopeBase), NumericType::UInt64, false);
              store(graphReturn + ReturnPhase, constant(5));
            });
          });
          next = arith::SelectOp::create(b, loc, equal(load(graphReturn + ReturnPhase), constant(5)),
              constant(A::Done), load(0));
        } else if (aggregateResult) {
          const auto done = emitDeviceAggregateReturn(*heap, b, input, resultFrame, offsets.at(plan.result));
          next = arith::SelectOp::create(b, loc, done, constant(UINT64_MAX), pc);
        } else
          next = constant(UINT64_MAX);
      }
      else if (op.code != C::Label) {
        std::vector<ShaderValue> arguments;
        for (auto arg : op.args)
          arguments.push_back(read(arg));
        auto value = emitter.invoke(*kernels[i], arguments);
        store(offsets.at(op.output), value.value);
        auto bad = arith::XOrIOp::create(b, loc, value.valid, yes);
        auto failure = scf::IfOp::create(b, loc, bad, false);
        {
          OpBuilder::InsertionGuard failed(b);
          b.setInsertionPointToStart(&failure.getThenRegion().front());
          memref::StoreOp::create(b, loc, constant(0, 32), params, ValueRange{at(1 + 2 * frames)});
        }
        next = arith::SelectOp::create(b, loc, bad, constant(UINT64_MAX), next);
      }
      if (storage) {
        auto flag = memref::LoadOp::create(b, loc, params, ValueRange{at(1 + 2 * frames)});
        auto valid = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, flag, constant(UINT32_MAX, 32));
        next = arith::SelectOp::create(b, loc, valid, next, constant(UINT64_MAX));
      }
      store(0, next);
    }
    scf::YieldOp::create(
        b, loc, ValueRange{arith::AddIOp::create(b, loc, after->getArgument(0), constant(1, 32))});
  } catch (const Rejected &error) {
    return std::unexpected(error.diagnostic);
  }
  Value result = load(offsets.at(plan.result));
  if (bits != 64)
    result = arith::TruncIOp::create(b, loc, b.getIntegerType(bits), result);
  memref::StoreOp::create(b, loc, result, kernel.getArgument(inputs.size()), ValueRange{at(0)});
  if (failed(verify(*module)))
    return reject("GPU scalar control shader failed verification");
  std::string ir;
  llvm::raw_string_ostream stream(ir);
  module->print(stream);
  std::vector<std::uint32_t> reads(inputs.size());
  for (std::size_t i = 0; i < reads.size(); ++i)
    reads[i] = i;
  auto resultShader = NumericShader{type,
                       std::move(ir),
                       std::move(reads),
                       {},
                       false,
                       true,
                       std::move(inputs),
                       std::move(states),
                       std::move(roles)};
  resultShader.deviceStorage = storage;
  return resultShader;
}
} // namespace kalandra
