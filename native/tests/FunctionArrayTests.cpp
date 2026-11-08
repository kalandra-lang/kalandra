#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
CheckResult analyze(const char *text) {
  return check(parse(std::make_shared<SourceFile>("function-arrays.k", text)));
}
} // namespace
int main() {
  try {
    for (
        const auto *text :
        {"fn worker(value:i64)->i64:\n  return value\nfn coordinate(value:i64)->i64 ! TaskError:\n  return try "
         "parallel:\n    try await spawn worker(value)\nfn main()->i64:\n  "
         "callbacks=[coordinate]\n  return 0\n",
         "fn double(value:i64)->i64:\n  return value*2\nfn triple(value:i64)->i64:\n  return value*3\nfn "
         "main()->i64:\n  callbacks=[double,triple]\n  return callbacks[0](12)+callbacks[1](6)\n",
         "fn make(offset:i64)->[fn(i64)->i64]:\n  return "
         "[fn(value):return value+offset,fn(value):return value*offset]\nfn main()->i64:\n  callbacks=make(20)\n "
         " return callbacks[0](2)+callbacks[1](1)\n",
         "fn make()->[fn(i64)->i64]:\n  text=\"é\"+\"!\"\n  values:[i64]=[20,20]\n  "
         "return [fn(value):return values[0]+values[1]+length(text)+value]\nfn main()->i64:\n  "
         "callbacks=make()\n  return callbacks[0](0)\n",
         "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[]\n  return length(callbacks)+42\n",
         "fn make(offset:i64)->[fn(i64)->i64]:\n  return [fn(value):return value+offset]\nfn main()->i64:\n  "
         "callbacks=concat(make(10),make(20))\n  selected=slice(callbacks,1,2)\n  "
         "return selected[0](22)\n",
         "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value*2,fn(value):return value*3]\n  "
         "values=map(callbacks,fn(callback):return callback(8))\n  return values[0]+values[1]+2\n",
         "fn main()->i64:\n  callbacks=map([20,21],fn(offset):return fn(value:i64):return value+offset)\n  return "
         "callbacks[0](1)+callbacks[1](0)\n",
         "fn main()->i64:\n  maker=fn(offset:i64)->fn(i64)->i64:return fn(value:i64):return value+offset\n  "
         "callbacks=map([20,21],maker)\n  evaluate=fn(callback:fn(i64)->i64)->i64:return callback(0)\n  "
         "values=map(callbacks,evaluate)\n  return values[0]+values[1]+1\n",
         "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value):return value+offset\nfn "
         "evaluate(callback:fn(i64)->i64)->i64:\n  return callback(0)\nfn main()->i64:\n  "
         "callbacks=map([20,21],make)\n  values=map(callbacks,evaluate)\n  return values[0]+values[1]+1\n",
         "type Config:\n  callbacks:[fn(i64)->i64]\nfn make(offset:i64)->Config:\n  return Config "
         "{callbacks:[fn(value):return value+offset]}\nfn main()->i64:\n  config=make(20)\n  return "
         "config.callbacks[0](22)\n",
         "enum Choice:\n  Empty\n  Some:\n    callbacks:[fn(i64)->i64]\nfn main()->i64:\n  "
         "offset:i64=20\n  choice=Choice.Some {callbacks:[fn(value):return value+offset]}\n  return match "
         "choice:\n    Choice.Empty:0\n    Choice.Some {callbacks}:callbacks[0](22)\n",
         "error E:\n  Bad:\n    value:i64\nfn make(offset:i64)->[fn(i64)->i64 ! E]:\n  "
         "validate=fn(value:i64)->i64 ! E:\n    if value==0:fail E.Bad {value:offset}\n    return value\n "
         " return [validate]\nfn main()->i64:\n  callbacks=make(42)\n  return callbacks[0](0) catch fn(error):\n "
         "   return match error:\n      E.Bad {value}:value\n",
         "error E:\n  Bad:\n    callbacks:[fn(i64)->i64]\nfn raise(offset:i64)->i64 ! E:\n  fail "
         "E.Bad {callbacks:[fn(value):return value+offset]}\nfn main()->i64:\n  return raise(20) catch "
         "fn(error):\n    return match error:\n      E.Bad {callbacks}:callbacks[0](22)\n",
         "fn evaluate(callbacks:[fn(i64)->i64])->i64:\n  return callbacks[0](22)\nfn "
         "make(offset:i64)->[fn(i64)->i64]:\n  return [fn(value):return value+offset]\nfn main()->i64:\n  "
         "factory=make\n  apply=evaluate\n  return apply(factory(20))\n",
         "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value*2]\n  "
         "run=fn(value:i64):return callbacks[0](value)\n  return run(21)\n",
         "fn main()->i64:\n  first:[fn(i64)->i64]=[fn(value):return value*2]\n  "
         "second:[fn(i64)->i64]=[fn(value):return value,fn(value):return value*3]\n  callbacks=if true:first\n  "
         "else:second\n  return callbacks[0](21)\n",
         "fn worker(offset:i64)->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value+offset]\n  return "
         "callbacks[0](22)\nfn main()->i64 ! TaskError:\n  return try parallel:\n    try await spawn "
         "worker(20)\n",
         "fn worker(value:i64)->i64:\n  return value\nfn main()->i64 ! TaskError:\n  "
         "callbacks:[fn(i64)->i64]=[fn(value):return value*2]\n  return try parallel:\n    value=try await spawn "
         "worker(21)\n    callbacks[0](value)\n",
         "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value*2]\n  first=10 -> "
         "callbacks[0]\n  second=11 -> callbacks[0]()\n  return first+second\n",
         "fn main()->i64:\n  values:[i64]=[]\n  maker=fn(offset:i64)->fn(i64)->i64:\n    "
         "bad=1/offset\n    return fn(value:i64):return value+bad\n  callbacks=map(values,maker)\n  "
         "return length(callbacks)+42\n",
         "error E:\n  Bad\nfn make(offset:i64)->[fn(i64)->i64] ! E:\n  if offset==0:fail E.Bad\n  "
         "return [fn(value):return value+offset]\nfn main()->i64:\n  factory=make\n  callbacks=factory(0) catch "
         "fn(error):return [fn(value:i64):return value*2]\n  return callbacks[0](21)\n",
         "error E:\n  Bad:\n    callbacks:[fn(i64)->i64]\nfn raise(offset:i64)->i64 ! E:\n  fail "
         "E.Bad {callbacks:[fn(value):return value+offset]}\nfn worker(offset:i64)->i64:\n  return raise(offset) "
         "catch fn(error):\n    return match error:\n      E.Bad {callbacks}:callbacks[0](22)\nfn "
         "main()->i64 ! TaskError:\n  return try parallel:\n    try await spawn worker(20)\n",
         "fn worker(index:i64)->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return 1/value]\n  return "
         "callbacks[index](0)\nfn attempt()->i64 ! TaskError:\n  return try parallel:\n    try await "
         "spawn worker(1)\nfn main()->i64:\n  return attempt() catch fn(error):\n    return match error:\n      "
         "TaskError.Fault {kind}:\n        match kind:\n          FaultKind.Bounds:42\n          "
         "_:0\n      _:0\n",
         "error E:\n  Bad:\n    value:i64\nfn main()->i64:\n  offset:i64=21\n  "
         "failer=fn(value:i64)->i64 ! E:fail E.Bad {value:offset}\n  callbacks:[fn(i64)->i64 ! "
         "E]=[failer,failer]\n  evaluate=fn(callback:fn(i64)->i64 ! E)->i64:\n    return callback(0) "
         "catch fn(error):\n      return match error:\n        E.Bad {value}:value\n  "
         "values=map(callbacks,evaluate)\n  return values[0]+values[1]\n"}) {
      auto result = analyze(text);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid function array rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "valid function array metadata rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), plan ? "" : plan.error().c_str());
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
      auto native = lowerNative(*result.module);
      require(bool(native), native ? "" : native.error().c_str());
#endif
    }
    // Worker aggregate transport now supports these former rejection cases.
    for (const auto *accepted :
         {"fn worker(callbacks:[fn(i64)->i64])->i64:\n  return callbacks[0](42)\nfn main()->i64 ! "
          "TaskError:\n  callbacks:[fn(i64)->i64]=[fn(value):return value]\n  return try parallel:\n    try "
          "await spawn worker(callbacks)\n",
          "fn worker(offset:i64)->[fn(i64)->i64]:\n  return [fn(value):return value+offset]\nfn main()->i64 ! "
          "TaskError:\n  return try parallel:\n    callbacks=try await spawn worker(20)\n    "
          "callbacks[0](22)\n",
          "type Config:\n  callbacks:[fn(i64)->i64]\nfn worker(config:Config)->i64:\n  return "
          "config.callbacks[0](42)\nfn main()->i64 ! TaskError:\n  config=Config "
          "{callbacks:[fn(value):return value]}\n  return try parallel:\n    try await spawn worker(config)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    for (const auto *text :
         {"fn main()->i64:\n  callbacks=[fn(value):return value]\n  return 0\n",
          "fn main()->i64:\n  callbacks=[]\n  return 0\n",
          "fn main()->i64:\n  callbacks=[fn(value:i64):return value,fn(value:f64):return value]\n  return 0\n",
          "fn main()->i64:\n  callbacks=[fn(value:i64):return value,fn(value:i64)->f64:return f64(value)]\n  "
          "return 0\n",
          "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value:f64):return value]\n  return 0\n",
          "error A:\n  Bad\nerror B:\n  Bad\nfn main()->i64:\n  first=fn(value:i64)->i64 ! "
          "A:return value\n  second=fn(value:i64)->i64 ! B:return value\n  callbacks=[first,second]\n  return 0\n",
          "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value]\n  return callbacks(42)\n",
          "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value]\n  return "
          "callbacks[0](\"bad\")\n",
          "fn main()->i64:\n  first=[fn(value:i64):return value]\n  second=[fn(value:f64):return value]\n  "
          "callbacks=concat(first,second)\n  return 0\n",
          "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value]\n  return callbacks[1](42)\n",
          "error E:\n  Bad\nfn main()->i64:\n  callback=fn(value:i64)->i64 ! E:return value\n  "
          "callbacks=[callback]\n  return callbacks[0](42)+1\n",
          "error E:\n  Bad\nfn main()->i64:\n  callback=fn(value:i64)->i64 ! E:return value\n  "
          "callbacks=[callback]\n  evaluate=fn(fnValue:fn(i64)->i64 ! E)->i64 ! E:return try fnValue(0)\n "
          " values=map(callbacks,evaluate)\n  return 0\n",

          "fn main()->i64:\n  callbacks:[[fn(i64)->i64]]=[]\n  0\n",
          "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value]\n  "
          "value=tensor(callbacks)\n  return 0\n",
          "type A:\n  value:i64\ntype B:\n  value:i64\nfn main()->i64:\n  "
          "callbacks:[fn(A)->i64]=[fn(value:B):return value.value]\n  return 0\n"})
      require(!analyze(text).ok(), "invalid function array accepted");
    auto separate = analyze(
        "error InputError:\n  Bad:\n    value:i64\nerror BuildError:\n  Disabled\nfn "
        "make(offset:i64)->[fn(i64)->i64 ! InputError] ! BuildError:\n  if offset==0:fail "
        "BuildError.Disabled\n  callback=fn(value:i64)->i64 ! InputError:\n    if value==0:fail "
        "InputError.Bad {value:offset}\n    return value\n  return [callback]\nfn main()->i64:\n  factory=make\n "
        " callbacks=factory(42) catch fn(error):return [fn(value:i64)->i64 ! InputError:return value]\n  return "
        "callbacks[0](0) catch fn(error):\n    return match error:\n      InputError.Bad {value}:value\n");
    require(separate.ok(), "array factory and element channels were conflated");
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
    auto separateNative = lowerNative(*separate.module);
    require(bool(separateNative), separateNative ? "" : separateNative.error().c_str());
#endif
    auto base = analyze(
        "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value):return value*2]\n  return callbacks[0](21)\n");
    require(base.ok(), "forged function array fixture rejected");
    for (unsigned mode = 0; mode < 10; ++mode) {
      auto module = *base.module;
      bool changed = false;
      for (std::size_t id = 0; id < module.expressions.size(); ++id) {
        auto &expression = module.expressions[id];
        if (isFunctionArray(expression.type) && mode < 6) {
          if (mode == 0)
            expression.type.callable.reset();
          if (mode == 1) {
            auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
            signature->parameters[0].element = NumericType::Float64;
            expression.type.callable = signature;
          }
          if (mode == 2)
            expression.type.shape.push_back({1, 0});
          if (mode == 3)
            expression.type.shape[0].constant = UINT64_MAX;
          if (mode == 4)
            expression.type.tensor = false;
          if (mode == 5)
            expression.type.device = 0;
          changed = true;
          break;
        }
        if (auto *read = std::get_if<ArrayIndex>(&expression.operation); read && mode >= 6) {
          if (mode == 6) {
            auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
            signature->result.element = NumericType::Float64;
            expression.type.callable = signature;
          }
          if (mode == 7)
            read->input = id;
          if (mode == 8)
            expression.type.array = true;
          if (mode == 9)
            expression.type.callable.reset();
          changed = true;
          break;
        }
      }
      require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
              "forged function array metadata accepted");
    }
    auto factory = analyze("fn make()->[fn(i64)->i64]:\n  return [fn(value):return value]\nfn main()->i64:\n  "
                           "factory=make\n  callbacks=factory()\n  return callbacks[0](42)\n");
    require(factory.ok(), "array factory metadata fixture rejected");
    auto forgedFactory = *factory.module;
    bool changedFactory = false;
    for (auto &expression : forgedFactory.expressions)
      if (isFunctionArray(expression.type) &&
          std::holds_alternative<FunctionInvoke>(expression.operation)) {
        expression.type.shape[0].constant = 10;
        changedFactory = true;
        break;
      }
    require(changedFactory && numericCapabilityError(forgedFactory, NumericTarget::Cpu).has_value(),
            "indirect factory invented a fixed array length");
    auto mapped =
        analyze("fn main()->i64:\n  callbacks=map([20],fn(offset):return fn(value:i64):return value+offset)\n  return "
                "callbacks[0](22)\n");
    require(mapped.ok(), "function array map metadata fixture rejected");
    auto forgedMap = *mapped.module;
    bool changedMap = false;
    for (auto &expression : forgedMap.expressions)
      if (isFunctionArray(expression.type) &&
          std::holds_alternative<ArrayMap>(expression.operation)) {
        auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
        signature->result.element = NumericType::Float64;
        expression.type.callable = signature;
        changedMap = true;
        break;
      }
    require(changedMap && numericCapabilityError(forgedMap, NumericTarget::Cpu).has_value(),
            "mapped function array changed its element signature");
    auto headers = parse(std::make_shared<SourceFile>(
        "headers.k",
        "fn inspect(callbacks:[fn(i64)->i64 ! CallError])->[fn()->fn(i64)->i64]:\n  return 0\n"));
    require(headers.ok(), "function array signatures failed to parse");
    const auto &parameter = headers.module.functions[0].parameters[0].type;
    require(parameter.array && parameter.function && parameter.function->errorTypes.size() == 1,
            "function array lost nested invocation channel");
    require(headers.module.source->slice(parameter.span) == "[fn(int)->int ! CallError]",
            "function array span lost delimiters");
    require(headers.module.functions[0].result->array &&
                headers.module.functions[0].result->function &&
                headers.module.functions[0].result->function->result.function,
            "returned function array lost nested signature");
    SyntaxLimits syntaxLimits;
    syntaxLimits.maxDepth = 8;
    std::string nested = "i64";
    for (unsigned i = 0; i < 32; ++i)
      nested = "[fn()->" + nested + "]";
    require(
        !parse(std::make_shared<SourceFile>("depth.k", "fn inspect(value:" + nested + "):\n  0\n"),
               syntaxLimits)
             .ok(),
        "function array bypassed syntax-depth budget");
    SemanticLimits semanticLimits;
    semanticLimits.maxValues = 6;
    auto budget = check(parse(std::make_shared<SourceFile>(
                            "budget.k", "fn identity(value:i64)->i64:\n  return value\nfn main()->i64:\n  "
                                        "callbacks=[identity,identity,identity,identity]\n  return 0\n")),
                        semanticLimits);
    require(!budget.ok() &&
                std::ranges::any_of(budget.diagnostics,
                                    [](const auto &diagnostic) {
                                      return diagnostic.code == "S024" &&
                                             diagnostic.message.find("function array literal") !=
                                                 std::string::npos;
                                    }),
            "function array literal bypassed inline storage budget");
    require(numericCapabilityError(*base.module, NumericTarget::AppleGpu).has_value(),
            "GPU function array accepted");
    std::cout
        << "Function arrays, nested signatures, checked indexing and captured ownership passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
