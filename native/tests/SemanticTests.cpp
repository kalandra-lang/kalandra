#include "kalandra/Semantics.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace kalandra;
namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
CheckResult analyze(std::string text, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("semantic.k", std::move(text))), limits);
}
void accepted(const CheckResult &result) {
  if (!result.ok()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << renderDiagnostic(*result.source, diagnostic);
    throw std::runtime_error("expected valid typed module");
  }
}
void rejected(std::string text, std::string_view code, SemanticLimits limits = {}) {
  auto result = analyze(std::move(text), limits);
  require(!result.ok() && !result.module, "invalid module published typed IR");
  require(std::ranges::any_of(result.diagnostics, [code](const auto &d) { return d.code == code; }),
          "missing diagnostic: " + std::string(code));
}
const TypedExpr &returned(const TypedModule &module, FunctionId id) {
  return module.expressions[module.functions[id].result];
}
void validIR(const TypedModule &module) {
  std::vector<FunctionId> owners(module.expressions.size(), module.functions.size());
  for (FunctionId f = 0; f < module.functions.size(); ++f) {
    for (auto value : module.functions[f].expressions) {
      require(value < module.expressions.size(), "bad value ID");
      require(owners[value] == module.functions.size(), "value has two owners");
      owners[value] = f;
    }
  }
  for (ValueId id = 0; id < module.expressions.size(); ++id) {
    const auto &expression = module.expressions[id];
    require(owners[id] < module.functions.size(), "unowned typed value");
    const auto operand = [&](ValueId child) {
      require(child < id && owners[child] == owners[id], "invalid value dependency/ownership");
    };
    for (const auto &dimension : expression.type.shape)
      require(dimension.constant.has_value() != (dimension.identity != 0),
              "invalid extent representation");
    std::visit(
        [&](const auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, SymbolRead>) {
            require(op.symbol < module.symbols.size() &&
                        module.symbols[op.symbol].owner == owners[id],
                    "bad symbol resolution");
          } else if constexpr (std::is_same_v<T, ScalarUnary> || std::is_same_v<T, TensorUnary>)
            operand(op.operand);
          else if constexpr (std::is_same_v<T, ScalarBinary> || std::is_same_v<T, ArrayConcat> ||
                             std::is_same_v<T, TensorBinary> || std::is_same_v<T, Matmul>) {
            operand(op.left);
            operand(op.right);
          } else if constexpr (std::is_same_v<T, TensorLiteral>) {
            for (auto value : op.elements)
              operand(value);
          } else if constexpr (std::is_same_v<T, FunctionCall>) {
            require(op.function < module.functions.size(), "unresolved function call");
            require(op.arguments.size() == module.functions[op.function].parameters.size(),
                    "invalid call arity");
            for (auto value : op.arguments)
              operand(value);
          } else if constexpr (std::is_same_v<T, TaskOperation>)
            operand(op.operand);
          else if constexpr (std::is_same_v<T, ParallelOperation>) {
            operand(op.result);
            for (const auto &statement : op.body)
              operand(statement.value);
          } else if constexpr (std::is_same_v<T, CatchOperation>) {
            operand(op.operand);
            operand(op.result);
            require(op.parameter < module.symbols.size(), "bad handler parameter");
            for (const auto &statement : op.body)
              operand(statement.value);
          } else if constexpr (std::is_same_v<T, MatchOperation>) {
            operand(op.subject);
            for (const auto &arm : op.cases) {
              operand(arm.result);
              for (const auto &statement : arm.body)
                operand(statement.value);
            }
          } else if constexpr (std::is_same_v<T, ErrorFieldRead>)
            operand(op.subject);
          else if constexpr (std::is_same_v<T, FailOperation>)
            operand(op.error);
          else if constexpr (std::is_same_v<T, ArrayIndex>) {
            operand(op.input);
            operand(op.index);
          } else if constexpr (std::is_same_v<T, ArraySlice>) {
            operand(op.input);
            operand(op.start);
            operand(op.end);
          } else if constexpr (std::is_same_v<T, TensorReshape>) {
            operand(op.input);
            for (auto dimension : op.dimensions)
              operand(dimension);
          } else if constexpr (std::is_same_v<T, TensorIndex>) {
            operand(op.input);
            for (auto index : op.indices)
              operand(index);
          } else if constexpr (std::is_same_v<T, ReduceSum> || std::is_same_v<T, CollectionQuery> ||
                               std::is_same_v<T, ShapeCheck> || std::is_same_v<T, ArrayToTensor> ||
                               std::is_same_v<T, TensorToArray>)
            operand(op.input);
        },
        expression.operation);
  }
}
void taskRecovery() {
  const std::string helper = "fn f() -> float32 ! TaskError:\n  1\n";
  const std::string prefix =
      helper + "fn main() -> float32:\n  f() catch fn(error):\n    match error:\n";
  auto result =
      analyze(prefix + "      TaskError.Fault { kind: cause, message: detail }:\n"
                       "        match cause:\n"
                       "          FaultKind.Arithmetic: 1\n          FaultKind.Bounds: 2\n"
                       "          FaultKind.Shape: 3\n          FaultKind.Allocation: 4\n          "
                       "FaultKind.Native: 5\n"
                       "      TaskError.AdmissionRejected { reason }: 6\n"
                       "      TaskError.Cancelled: 7\n      TaskError.DeadlineExceeded: 8\n");
  accepted(result);
  validIR(*result.module);
  const auto ir = dumpTypedIR(*result.module);
  require(ir.find("result.catch") != std::string::npos &&
              ir.find("FaultKind") != std::string::npos && ir.find("String") != std::string::npos,
          "typed errors missing from IR");
  auto forwarding = analyze(helper + "fn main() -> float32:\n  result = f() catch fn(error): fail "
                                     "error\n  result catch fn(error): 2\n");
  accepted(forwarding);
  validIR(*forwarding.module);
  rejected(prefix + "      TaskError.Fault: 1\n", "S044");
  rejected(prefix + "      _: 1\n      TaskError.Fault: 2\n", "S041");
  rejected(prefix + "      TaskError.Fault: 1\n      TaskError.Fault: 2\n      _: 3\n", "S041");
  rejected(prefix + "      TaskError.Unknown: 1\n      _: 2\n", "S040");
  rejected(prefix + "      FaultKind.Shape: 1\n      _: 2\n", "S040");
  rejected(prefix + "      TaskError.Fault { reason }: 1\n      _: 2\n", "S042");
  rejected(prefix + "      TaskError.Fault { kind, kind }: 1\n      _: 2\n", "S042");
  rejected(prefix + "      TaskError.Fault { kind: x, message: x }: 1\n      _: 2\n", "S042");
  rejected(prefix + "      TaskError.Fault { kind }: kind + 1\n      _: 2\n", "S010");
  rejected(prefix + "      TaskError.Fault { message }: message\n      _: 2\n", "S010");
  rejected(helper + "fn main() -> float32:\n  f() catch fn(error): error + 1\n", "S010");
  rejected(helper + "fn main() -> float32:\n  f() catch fn(error): fail error\n", "S036");
  rejected("fn main() -> float32:\n  1 catch fn(error): 2\n", "S039");
  rejected("fn main() -> float32:\n  match 1:\n    _: 2\n", "S040");
  rejected(helper + "fn main() -> float32:\n  f() catch fn(error):\n    unused = f()\n    1\n",
           "S036");
  rejected(helper + "fn main() -> float32:\n  result = f() catch fn(error): 1\n  error\n", "S016");
  rejected(
      prefix +
          "      TaskError.Fault { kind }:\n        kind: float32 = 1\n        2\n      _: 3\n",
      "S008");
  rejected(helper +
               "fn main() -> float32:\n  f() catch fn(error):\n    error: float32 = 1\n    2\n",
           "S008");
  auto shapes = analyze(
      "fn echo(a: Tensor[float32, ?]) -> Tensor[float32, ?]:\n  a\n"
      "fn choose(a: Tensor[float32, ?], b: Tensor[float32, ?]) -> Tensor[float32, ?] ! TaskError:\n"
      "  result = parallel:\n    work = spawn echo(a)\n    try await work\n"
      "  result catch fn(error): b\n");
  accepted(shapes);
  validIR(*shapes.module);
  const auto &chosen = shapes.module->functions[1].resultType.shape[0];
  require(!chosen.constant &&
              chosen.identity != shapes.module->symbols[shapes.module->functions[1].parameters[0]]
                                     .type.shape[0]
                                     .identity &&
              chosen.identity != shapes.module->symbols[shapes.module->functions[1].parameters[1]]
                                     .type.shape[0]
                                     .identity,
          "catch inherited one branch's shape");
  // One conditional path cannot silently discharge a result from outside it.
  rejected(helper + "fn main() -> float32:\n  other = f()\n  f() catch fn(error): other catch "
                    "fn(nested): 1\n",
           "S036");
}
void taskTypesAndOwnership() {
  const std::string helper = "fn twice(x: float32) -> float32:\n  x * 2\n";
  auto result = analyze(
      helper + "fn main() -> float32 ! TaskError:\n  try parallel:\n    first = spawn twice(2)\n   "
               " alias = first\n    x = try await alias\n    y = try await first\n    x + y\n");
  accepted(result);
  validIR(*result.module);
  require(result.module->functions.back().resultType.fallible, "TaskError return lost");
  require(dumpTypedIR(*result.module).find("task.spawn") != std::string::npos, "task IR lost");
  rejected(helper + "fn main() -> float32:\n  spawn twice(2)\n", "S037");
  rejected(
      helper +
          "fn main() -> float32 ! TaskError:\n  try parallel:\n    first = spawn twice(2)\n    0\n",
      "S038");
  rejected(
      helper +
          "fn main() -> float32 ! TaskError:\n  parallel:\n    first = spawn twice(2)\n    first\n",
      "S035");
  rejected(helper + "fn main() -> float32 ! TaskError:\n  try parallel:\n    first = spawn "
                    "twice(2)\n    twice(first)\n",
           "S035");
  rejected(helper + "fn main() -> float32:\n  try parallel:\n    try await spawn twice(2)\n",
           "S036");
  rejected(helper + "fn main() -> float32 ! TaskError:\n  try parallel:\n    first = spawn "
                    "twice(2)\n    result = await first\n    0\n",
           "S036");
  rejected("fn main() -> float32 ! TaskError:\n  await 1\n", "S037");
  auto inlined =
      analyze(helper +
              "fn main() -> float32 ! TaskError:\n  try parallel:\n    try await spawn twice(2)\n");
  accepted(inlined);
  const auto &ops = inlined.module->expressions;
  const auto *tryOp =
      std::get_if<TaskOperation>(&ops[inlined.module->functions.back().result].operation);
  require(tryOp && tryOp->prefix == TaskPrefix::Try, "outer try missing");
}
void literalContexts() {
  const std::vector<std::pair<std::string, std::uint32_t>> cases{
      {"1", 0x3f800000},
      {"1.000000059604644775390625", 0x3f800000},
      {"1.000000178813934326171875", 0x3f800002},
      {"1.401298464324817e-45", 1},
      {"1e-9999", 0},
      {"3.4028234663852886e38", 0x7f7fffff}};
  for (const auto &[literal, bits] : cases) {
    auto result = analyze("fn main() -> float32:\n  " + literal + "\n");
    accepted(result);
    require(std::get<Float32Literal>(returned(*result.module, 0).operation).bits == bits,
            "wrong literal rounding");
  }
  auto negative = analyze("fn main() -> float32:\n  -0\n");
  accepted(negative);
  require(std::holds_alternative<ScalarUnary>(returned(*negative.module, 0).operation),
          "negative zero sign was discarded");
  rejected("fn main() -> float32:\n  3.5e38\n", "S015");
  rejected("fn main() -> float32:\n  x = 1\n  x\n", "S010");
  auto defaultFloat = analyze("fn main():\n  1.0\n");
  accepted(defaultFloat);
  require(defaultFloat.module->functions[0].resultType.element == NumericType::Float64,
          "default float must be float64");
  auto context = analyze("fn main(x: float32) -> float32:\n  y = 1 + x\n  y * 2\n");
  accepted(context);
  validIR(*context.module);
}
void resolutionAndCalls() {
  auto result = analyze("fn first(x: float32):\n  later(x)\nfn later(x: float32):\n  x\nfn main() "
                        "-> float32:\n  2 -> first -> later()\n");
  accepted(result);
  validIR(*result.module);
  require(std::get<FunctionCall>(returned(*result.module, 0).operation).function == 1,
          "forward call unresolved");
  const auto &outer = std::get<FunctionCall>(returned(*result.module, 2).operation);
  require(outer.function == 1 && outer.arguments.size() == 1, "pipeline target/arity wrong");
  const auto &inner = result.module->expressions[outer.arguments[0]];
  require(std::get<FunctionCall>(inner.operation).function == 0, "pipeline lost first stage");
  const auto calls = std::ranges::count_if(result.module->functions[2].expressions, [&](auto id) {
    return std::holds_alternative<FunctionCall>(result.module->expressions[id].operation);
  });
  require(calls == 2, "pipeline duplicated invocation");
  accepted(analyze("fn recur(x: float32) -> float32:\n  recur(x)\n"));
  rejected("fn recur(x: float32):\n  recur(x)\n", "S006");
  rejected("fn first(x: float32):\n  second(x)\nfn second(x: float32):\n  first(x)\n", "S006");
  accepted(analyze("fn main(x: float32) -> float32:\n  y = x\n  y\n"));
}
void shapeFactsAndGuards() {
  auto result = analyze("fn project(a: Tensor[float32, ?, 3], b: Tensor[float32, 3, 2]) -> "
                        "Tensor[float32, ?, 2]:\n  matmul(a, b)\nfn main() -> Tensor[float32, 2, "
                        "2]:\n  a: Tensor[float32,2,3] = tensor([[1,2,3],[4,5,6]])\n  b: "
                        "Tensor[float32,3,2] = tensor([[1,2],[3,4],[5,6]])\n  project(a,b)\n");
  accepted(result);
  validIR(*result.module);
  const auto &module = *result.module;
  const auto &project = module.functions[0];
  const auto &formal = module.symbols[project.parameters[0]].type;
  require(project.resultType.shape[0].identity == formal.shape[0].identity &&
              project.resultType.shape[1].constant == 2,
          "result row relationship lost");
  require(module.functions[1].resultType.shape[0].constant == 2 &&
              module.functions[1].resultType.shape[1].constant == 2,
          "call did not specialize shape facts");
  const auto &op = std::get<Matmul>(returned(module, 0).operation);
  require(!op.checkInnerExtent && op.checkAllocationSize && op.separatelyRoundedProducts &&
              op.order == ReductionOrder::BalancedAdjacent,
          "matmul numerical/guard contract lost");
  auto dynamic = analyze("fn product(a: Tensor[float32,?,?], b: Tensor[float32,?,?]) -> "
                         "Tensor[float32,?,?]:\n  matmul(a,b)\n");
  accepted(dynamic);
  require(std::get<Matmul>(returned(*dynamic.module, 0).operation).checkInnerExtent,
          "unrelated dynamic '?' treated as equal");
  auto guard = analyze("fn fixed(a: Tensor[float32,2,3]) -> Tensor[float32,2,3]:\n  a\nfn "
                       "refine(a: Tensor[float32,?,3]) -> Tensor[float32,2,3]:\n  fixed(a)\n");
  accepted(guard);
  validIR(*guard.module);
  const auto &call = std::get<FunctionCall>(returned(*guard.module, 1).operation);
  const auto &check = std::get<ShapeCheck>(guard.module->expressions[call.arguments[0]].operation);
  require(check.requirements.size() == 1 && check.requirements[0].axis == 0 &&
              check.requirements[0].extent == 2,
          "missing dynamic-to-static shape check");
  const auto &inputType = guard.module->symbols[guard.module->functions[1].parameters[0]].type;
  require(!inputType.shape[0].constant, "shape refinement changed source binding");
  rejected("fn mm(a: Tensor[float32,?,?],b: Tensor[float32,?,?]) -> Tensor[float32,?,?]:\n  "
           "matmul(a,b)\nfn wrap(a: Tensor[float32,?,?],b: Tensor[float32,?,?]) -> "
           "Tensor[float32,?,?]:\n  mm(a,b)\nfn bad(a: Tensor[float32,2,3],b: Tensor[float32,4,2]) "
           "-> Tensor[float32,2,2]:\n  wrap(a,b)\n",
           "S033");
  auto fresh = analyze("fn recur(a: Tensor[float32,?,?]) -> Tensor[float32,?,?]:\n  recur(a)\nfn "
                       "caller(a: Tensor[float32,?,?]) -> float32:\n  x = recur(a)\n  y = "
                       "recur(a)\n  reduceSum(matmul(x,y))\n");
  accepted(fresh);
  const auto &caller = fresh.module->functions[1];
  const auto &x = fresh.module->symbols[*caller.body[0].binding].type;
  const auto &y = fresh.module->symbols[*caller.body[1].binding].type;
  require(x.shape[0].identity != y.shape[0].identity,
          "distinct call results accidentally share existential extents");
}
void tensorLiterals() {
  auto zero = analyze("fn main() -> Tensor[float32,2,0]:\n  tensor([[],[]])\n");
  accepted(zero);
  require(std::get<TensorLiteral>(returned(*zero.module, 0).operation).elements.empty(),
          "empty tensor has data");
  auto rankZero = analyze("fn main() -> Tensor[float32]:\n  tensor(2)\n");
  accepted(rankZero);
  require(rankZero.module->functions[0].resultType.tensor &&
              rankZero.module->functions[0].resultType.shape.empty(),
          "rank-zero tensor lost");
  rejected("fn main() -> Tensor[float32,0,3]:\n  tensor([])\n", "S010");
  auto scalarLeaf = analyze("fn main(v: float32) -> Tensor[float32,1,2]:\n  tensor([[v, 2]])\n");
  accepted(scalarLeaf);
  auto empty =
      analyze("fn f(a: Tensor[float32,9223372036854775807,0]) -> float32:\n  reduceSum(a)\n");
  accepted(empty);
  rejected("fn main() -> Tensor[float32,2,2]:\n  tensor([[1],[2,3]])\n", "S023");
  rejected("fn main() -> Tensor[float32,2,2]:\n  tensor([[1,2,3],[4,5,6]])\n", "S011");
  rejected("fn main() -> Tensor[float32,0,?]:\n  tensor([])\n", "S010");
  accepted(analyze("fn main():\n  tensor([[1]])\n"));
}
void elementwiseArithmetic() {
  auto unary = analyze("fn negative(a: Tensor[float32,?,?]) -> Tensor[float32,?,?]:\n  -a\n"
                       "fn positive(a: Tensor[float32,2,3]) -> Tensor[float32,2,3]:\n  +a\n");
  accepted(unary);
  validIR(*unary.module);
  require(std::holds_alternative<TensorUnary>(returned(*unary.module, 0).operation),
          "tensor negation lost its typed operation");
  require(std::holds_alternative<SymbolRead>(returned(*unary.module, 1).operation),
          "tensor unary plus must remain an identity");
  require(unary.module->functions[0].preconditions.empty() &&
              dumpTypedIR(*unary.module).find("elementwise neg.f32 exact_sign_bit") !=
                  std::string::npos,
          "tensor negation must preserve shape without adding equality obligations");
  auto result = analyze(
      "fn add(a: Tensor[float32,?], b: Tensor[float32,?]) -> Tensor[float32,?]:\n  a+b\n"
      "fn lift(a: Tensor[float32,?], scale: float32) -> Tensor[float32,?]:\n  (a*scale)+0.5\n");
  accepted(result);
  validIR(*result.module);
  require(std::holds_alternative<TensorBinary>(returned(*result.module, 0).operation),
          "elementwise typed operation lost");
  require(result.module->functions[0].preconditions.size() == 1,
          "dynamic shape equality not recorded");
  require(dumpTypedIR(*result.module).find("elementwise add.f32 strict") != std::string::npos,
          "elementwise strict policy missing");
  accepted(analyze("fn lift(a: Tensor[float32,2,3]) -> Tensor[float32,2,3]:\n  0.5+a*2\n"));
  auto division = analyze(
      "fn quotient(a: Tensor[float32,?,?], b: Tensor[float32,?,?]) -> Tensor[float32,?,?]:\n  a/b\n"
      "fn reverse(a: Tensor[float32,?]) -> Tensor[float32,?]:\n  1/a\n");
  accepted(division);
  validIR(*division.module);
  require(std::get<TensorBinary>(returned(*division.module, 0).operation).operation ==
                  TokenKind::Slash &&
              division.module->functions[0].preconditions.size() == 2,
          "tensor division lost its operation or axis constraints");
  rejected("fn f(a: Tensor[float32,2], b: Tensor[float32,3]) -> Tensor[float32,2]:\n  a+b\n",
           "S011");
  rejected("fn f(a: Tensor[float32,2], b: Tensor[float32,1,2]) -> Tensor[float32,2]:\n  a*b\n",
           "S017");
  rejected("fn add(a: Tensor[float32,?], b: Tensor[float32,?]) -> Tensor[float32,?]:\n  a+b\n"
           "fn bad(a: Tensor[float32,2], b: Tensor[float32,3]) -> Tensor[float32,2]:\n  add(a,b)\n",
           "S033");
  rejected("fn f(a: Tensor[float32,2]) -> Tensor[float32,2]:\n  a%2\n", "S019");
}
void diagnostics() {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"fn f(x:Tensor[float16,1]) -> int:\n  1\n", "S003"},
      {"fn f(a: Tensor[float16,2,3]) -> float32:\n  1\n", "S003"},
      {"fn f() -> float32:\n  1\nfn f() -> float32:\n  2\n", "S004"},
      {"fn f(x: float32,x: float32) -> float32:\n  x\n", "S005"},
      {"fn f(x: float32) -> float32:\n  x = 1\n  x\n", "S008"},
      {"fn f() -> float32:\n  x: float32 = 1\n  x: float32 = 2\n  x\n", "S008"},
      {"fn f() -> float32:\n  x: float32 = x\n  x\n", "S016"},
      {"fn f() -> float32:\n  missing\n", "S016"},
      {"fn f() -> float32:\n  missing(1)\n", "S031"},
      {"fn f(x: float32) -> float32:\n  x()\n", "S030"},
      {"fn f(x: float32) -> float32:\n  f()\n", "S027"},
      {"fn f() -> float32:\n  [1,2]\n", "S021"},
      {"fn f(x: float32) -> float32:\n  x % 2\n", "S019"},
      {"fn f(x: float32) -> float32:\n  reduceSum(x)\n", "S028"},
      {"fn f(x: Tensor[float32,2]) -> float32:\n  matmul(x,x)\n", "S028"},
      {"fn f(a: Tensor[float32,2,3],b: Tensor[float32,4,2]) -> Tensor[float32,2,2]:\n  "
       "matmul(a,b)\n",
       "S029"},
      {"fn f(x: Tensor[float32,2,3]) -> float32:\n  x\n", "S010"},
      {"fn f(x: Tensor[float32,2,3]) -> Tensor[float32,3,2]:\n  x\n", "S011"},
      {"fn f(x: Tensor[float32,2,3]) -> Tensor[float32,6]:\n  x\n", "S010"},
      {"fn f(x: Tensor[float32,2,3]) -> Tensor[float32,2,3]:\n  x % x\n", "S019"},
      {"fn f(a: Tensor[float32,9223372036854775808,0]) -> float32:\n  1\n", "S001"},
      {"fn f(a: Tensor[float32,4294967296,4294967296]) -> float32:\n  1\n", "S002"},
      {"fn f(a: Tensor[float32,2305843009213693952]) -> float32:\n  1\n", "S002"},
  };
  for (const auto &[text, code] : cases)
    rejected(text, code);
  auto malformed = analyze("fn f(x):\n  x\n");
  require(!malformed.ok() && !malformed.module && malformed.diagnostics[0].code == "P002",
          "syntax diagnostics were lost");
  auto multiple = analyze("fn f() -> float32:\n  missing\nfn g() -> float32:\n  other\n");
  require(!multiple.ok() && multiple.diagnostics.size() == 2,
          "did not report independent function errors");
  require(multiple.source->location(multiple.diagnostics[0].span.begin) ==
              std::pair<std::size_t, std::size_t>{2, 3},
          "semantic location wrong");
}
void limitsAndRobustness() {
  SemanticLimits limits;
  limits.maxValues = 1;
  rejected("fn f(x:float32) -> float32:\n  x + 2\n", "S009", limits);
  limits = {};
  limits.maxTensorElements = 1;
  rejected("fn f() -> Tensor[float32,2]:\n  tensor([1,2])\n", "S024", limits);
  limits.maxTensorElements = 0;
  rejected("fn f() -> Tensor[float32]:\n  tensor(1)\n", "S024", limits);
  limits = {};
  limits.maxLiteralBytes = 3;
  rejected("fn f() -> float32:\n  1.25\n", "S032", limits);
  limits = {};
  limits.maxDepth = 3;
  rejected("fn f() -> float32:\n  1 + 2 + 3 + 4 + 5\n", "S012", limits);
  rejected("fn f() -> float32:\n  g()\nfn g() -> float32:\n  h()\nfn h() -> float32:\n  i()\nfn "
           "i() -> float32:\n  1\n",
           "S007", limits);
  limits = {};
  limits.maxShapeRequirements = 0;
  rejected(
      "fn f(a:Tensor[float32,?,?],b:Tensor[float32,?,?]) -> Tensor[float32,?,?]:\n  matmul(a,b)\n",
      "S034", limits);
  limits = {};
  limits.maxDiagnostics = 1;
  auto one = analyze("fn f() -> float32:\n  missing\nfn g() -> float32:\n  other\n", limits);
  require(!one.ok() && one.diagnostics.size() == 1, "semantic diagnostic budget ignored");
  limits.maxDiagnostics = 0;
  require(!analyze("fn f() -> float32:\n  missing\n", limits).ok(),
          "zero budget accepted invalid source");
  std::uint32_t state = 0x53454d41;
  constexpr std::string_view parts[] = {"x",           "1",    "unknown",     "x + 2",
                                        "tensor([x])", "f(x)", "reduceSum(x)"};
  for (int i = 0; i < 500; ++i) {
    state = state * 1664525u + 1013904223u;
    auto result = analyze("fn f(x: float32) -> float32:\n  " +
                          std::string(parts[state % std::size(parts)]) + "\n");
    if (result.ok())
      validIR(*result.module);
    else
      require(!result.module && !result.diagnostics.empty(),
              "failed checker published IR or lost error");
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    taskRecovery();
    taskTypesAndOwnership();
    literalContexts();
    resolutionAndCalls();
    shapeFactsAndGuards();
    tensorLiterals();
    elementwiseArithmetic();
    diagnostics();
    limitsAndRobustness();
    require(argc == 2, "expected canonical fixture");
    auto source = loadSource(argv[1]);
    require(source.has_value(), "cannot load canonical fixture");
    auto result = check(parse(*source));
    accepted(result);
    validIR(*result.module);
    require(result.module->functions.size() == 4 && result.module->expressions.size() == 26,
            "canonical typed module differs");
    const auto ir = dumpTypedIR(*result.module);
    require(ir.find("matmul strict_f32 balanced_adjacent separately_rounded_products") !=
                    std::string::npos &&
                ir.find("reduceSum strict_f32 balanced_adjacent row_major") != std::string::npos,
            "numerical policy missing from IR");
    std::cout << "PASS resolution, literal rounding, calls, pipelines, tensor shapes, guards, "
                 "diagnostics, limits, and canonical typed IR\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
