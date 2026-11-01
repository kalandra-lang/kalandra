#include "NumericShader.h"
#include "MatmulShader.h"
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
struct Expr {
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
  Value materialize(Expr &expr) {
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
  Expr literal(std::uint64_t value, bool boolean = false) {
    return {c(header, value, boolean ? 1 : 64), yes, yes, false, boolean};
  }
  Expr binary(Expr a, Expr d, TokenKind op, SourceSpan span) {
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
  Expr comparison(Expr a, Expr d, ComparisonKind op, SourceSpan span) {
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
  Expr invoke(FunctionId function, const std::vector<Expr> &args) {
    const auto &fn = input.functions.at(function);
    if (++work > 300000 || active.size() >= 128 || !active.insert(function).second)
      reject(fn.declaration, "recursive or oversized typed GPU helper expansion");
    std::map<SymbolId, Expr> symbols;
    std::map<ValueId, Expr> values;
    for (std::size_t i = 0; i < args.size(); ++i)
      symbols.emplace(fn.parameters.at(i), args[i]);
    std::function<Expr(ValueId)> emit = [&](ValueId id) -> Expr {
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
      Expr out;
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
        std::vector<Expr> inputs;
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
  Expr rootCall(FunctionId id, unsigned tensorCount) {
    std::vector<Expr> args;
    unsigned tensors = 0, scalarWords = 0, position = 0, realTensors = 0;
    auto parameters = kernel.getArgument(tensorCount + 1);
    for (auto parameter : root.parameters) {
      const auto &type = input.symbols.at(parameter).type;
      const bool deviceScalar =
          !options.deviceScalarParameters.empty() && options.deviceScalarParameters[position];
      ++position;
      if ((type.builtin != BuiltinValue::Numeric &&
           !(type.builtin == BuiltinValue::Bool && deviceScalar)) ||
          type.array || !describeNumeric(type.element))
        reject(root.declaration, "typed GPU captures require one exact numerical type");
      if (type.tensor) {
        ++realTensors;
        args.push_back({{}, yes, yes, true, false, tensors++, type.shape});
      } else if (deviceScalar) {
        args.push_back({{}, yes, yes, false, type.builtin == BuiltinValue::Bool, tensors++, {}});
      } else {
        const auto bits = describeNumeric(type.element)->bits;
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
        args.push_back({value, yes, yes});
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
  static const Expr &argsForTensor(const std::vector<Expr> &args, unsigned parameter) {
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
         !(type.builtin == BuiltinValue::Bool && deviceScalar)) ||
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
      scalarTypes.push_back(type.element);
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
} // namespace kalandra
