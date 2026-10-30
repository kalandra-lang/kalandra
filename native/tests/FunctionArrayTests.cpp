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
        {"fn worker(value:int)->int:\n  value\nfn coordinate(value:int)->int ! TaskError:\n  try "
         "parallel:\n    try await spawn worker(value)\nfn main()->int:\n  "
         "callbacks=[coordinate]\n  0\n",
         "fn double(value:int)->int:\n  value*2\nfn triple(value:int)->int:\n  value*3\nfn "
         "main()->int:\n  callbacks=[double,triple]\n  callbacks[0](12)+callbacks[1](6)\n",
         "fn make(offset:int)->[fn(int)->int]:\n  "
         "[fn(value):value+offset,fn(value):value*offset]\nfn main()->int:\n  callbacks=make(20)\n "
         " callbacks[0](2)+callbacks[1](1)\n",
         "fn make()->[fn(int)->int]:\n  text=\"é\"+\"!\"\n  values:[int]=[20,20]\n  "
         "[fn(value):values[0]+values[1]+length(text)+value]\nfn main()->int:\n  "
         "callbacks=make()\n  callbacks[0](0)\n",
         "fn main()->int:\n  callbacks:[fn(int)->int]=[]\n  length(callbacks)+42\n",
         "fn make(offset:int)->[fn(int)->int]:\n  [fn(value):value+offset]\nfn main()->int:\n  "
         "callbacks=concat(make(10),make(20))\n  selected=slice(callbacks,1,2)\n  "
         "selected[0](22)\n",
         "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value*2,fn(value):value*3]\n  "
         "values=map(callbacks,fn(callback):callback(8))\n  values[0]+values[1]+2\n",
         "fn main()->int:\n  callbacks=map([20,21],fn(offset):fn(value:int):value+offset)\n  "
         "callbacks[0](1)+callbacks[1](0)\n",
         "fn main()->int:\n  maker=fn(offset:int)->fn(int)->int:fn(value:int):value+offset\n  "
         "callbacks=map([20,21],maker)\n  evaluate=fn(callback:fn(int)->int)->int:callback(0)\n  "
         "values=map(callbacks,evaluate)\n  values[0]+values[1]+1\n",
         "fn make(offset:int)->fn(int)->int:\n  fn(value):value+offset\nfn "
         "evaluate(callback:fn(int)->int)->int:\n  callback(0)\nfn main()->int:\n  "
         "callbacks=map([20,21],make)\n  values=map(callbacks,evaluate)\n  values[0]+values[1]+1\n",
         "type Config:\n  callbacks:[fn(int)->int]\nfn make(offset:int)->Config:\n  Config "
         "{callbacks:[fn(value):value+offset]}\nfn main()->int:\n  config=make(20)\n  "
         "config.callbacks[0](22)\n",
         "enum Choice:\n  Empty\n  Some:\n    callbacks:[fn(int)->int]\nfn main()->int:\n  "
         "offset:int=20\n  choice=Choice.Some {callbacks:[fn(value):value+offset]}\n  match "
         "choice:\n    Choice.Empty:0\n    Choice.Some {callbacks}:callbacks[0](22)\n",
         "error E:\n  Bad:\n    value:int\nfn make(offset:int)->[fn(int)->int ! E]:\n  "
         "validate=fn(value:int)->int ! E:\n    if value==0:fail E.Bad {value:offset}\n    value\n "
         " [validate]\nfn main()->int:\n  callbacks=make(42)\n  callbacks[0](0) catch fn(error):\n "
         "   match error:\n      E.Bad {value}:value\n",
         "error E:\n  Bad:\n    callbacks:[fn(int)->int]\nfn raise(offset:int)->int ! E:\n  fail "
         "E.Bad {callbacks:[fn(value):value+offset]}\nfn main()->int:\n  raise(20) catch "
         "fn(error):\n    match error:\n      E.Bad {callbacks}:callbacks[0](22)\n",
         "fn evaluate(callbacks:[fn(int)->int])->int:\n  callbacks[0](22)\nfn "
         "make(offset:int)->[fn(int)->int]:\n  [fn(value):value+offset]\nfn main()->int:\n  "
         "factory=make\n  apply=evaluate\n  apply(factory(20))\n",
         "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value*2]\n  "
         "run=fn(value:int):callbacks[0](value)\n  run(21)\n",
         "fn main()->int:\n  first:[fn(int)->int]=[fn(value):value*2]\n  "
         "second:[fn(int)->int]=[fn(value):value,fn(value):value*3]\n  callbacks=if true:first\n  "
         "else:second\n  callbacks[0](21)\n",
         "fn worker(offset:int)->int:\n  callbacks:[fn(int)->int]=[fn(value):value+offset]\n  "
         "callbacks[0](22)\nfn main()->int ! TaskError:\n  try parallel:\n    try await spawn "
         "worker(20)\n",
         "fn worker(value:int)->int:\n  value\nfn main()->int ! TaskError:\n  "
         "callbacks:[fn(int)->int]=[fn(value):value*2]\n  try parallel:\n    value=try await spawn "
         "worker(21)\n    callbacks[0](value)\n",
         "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value*2]\n  first=10 -> "
         "callbacks[0]\n  second=11 -> callbacks[0]()\n  first+second\n",
         "fn main()->int:\n  values:[int]=[]\n  maker=fn(offset:int)->fn(int)->int:\n    "
         "bad=1/offset\n    fn(value:int):value+bad\n  callbacks=map(values,maker)\n  "
         "length(callbacks)+42\n",
         "error E:\n  Bad\nfn make(offset:int)->[fn(int)->int] ! E:\n  if offset==0:fail E.Bad\n  "
         "[fn(value):value+offset]\nfn main()->int:\n  factory=make\n  callbacks=factory(0) catch "
         "fn(error):[fn(value:int):value*2]\n  callbacks[0](21)\n",
         "error E:\n  Bad:\n    callbacks:[fn(int)->int]\nfn raise(offset:int)->int ! E:\n  fail "
         "E.Bad {callbacks:[fn(value):value+offset]}\nfn worker(offset:int)->int:\n  raise(offset) "
         "catch fn(error):\n    match error:\n      E.Bad {callbacks}:callbacks[0](22)\nfn "
         "main()->int ! TaskError:\n  try parallel:\n    try await spawn worker(20)\n",
         "fn worker(index:int)->int:\n  callbacks:[fn(int)->int]=[fn(value):1/value]\n  "
         "callbacks[index](0)\nfn attempt()->int ! TaskError:\n  try parallel:\n    try await "
         "spawn worker(1)\nfn main()->int:\n  attempt() catch fn(error):\n    match error:\n      "
         "TaskError.Fault {kind}:\n        match kind:\n          FaultKind.Bounds:42\n          "
         "_:0\n      _:0\n",
         "error E:\n  Bad:\n    value:int\nfn main()->int:\n  offset:int=21\n  "
         "failer=fn(value:int)->int ! E:fail E.Bad {value:offset}\n  callbacks:[fn(int)->int ! "
         "E]=[failer,failer]\n  evaluate=fn(callback:fn(int)->int ! E)->int:\n    callback(0) "
         "catch fn(error):\n      match error:\n        E.Bad {value}:value\n  "
         "values=map(callbacks,evaluate)\n  values[0]+values[1]\n"}) {
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
         {"fn worker(callbacks:[fn(int)->int])->int:\n  callbacks[0](42)\nfn main()->int ! "
          "TaskError:\n  callbacks:[fn(int)->int]=[fn(value):value]\n  try parallel:\n    try "
          "await spawn worker(callbacks)\n",
          "fn worker(offset:int)->[fn(int)->int]:\n  [fn(value):value+offset]\nfn main()->int ! "
          "TaskError:\n  try parallel:\n    callbacks=try await spawn worker(20)\n    "
          "callbacks[0](22)\n",
          "type Config:\n  callbacks:[fn(int)->int]\nfn worker(config:Config)->int:\n  "
          "config.callbacks[0](42)\nfn main()->int ! TaskError:\n  config=Config "
          "{callbacks:[fn(value):value]}\n  try parallel:\n    try await spawn worker(config)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    for (const auto *text :
         {"fn main()->int:\n  callbacks=[fn(value):value]\n  0\n",
          "fn main()->int:\n  callbacks=[]\n  0\n",
          "fn main()->int:\n  callbacks=[fn(value:int):value,fn(value:float):value]\n  0\n",
          "fn main()->int:\n  callbacks=[fn(value:int):value,fn(value:int)->float:float(value)]\n  "
          "0\n",
          "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value:float):value]\n  0\n",
          "error A:\n  Bad\nerror B:\n  Bad\nfn main()->int:\n  first=fn(value:int)->int ! "
          "A:value\n  second=fn(value:int)->int ! B:value\n  callbacks=[first,second]\n  0\n",
          "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value]\n  callbacks(42)\n",
          "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value]\n  "
          "callbacks[0](\"bad\")\n",
          "fn main()->int:\n  first=[fn(value:int):value]\n  second=[fn(value:float):value]\n  "
          "callbacks=concat(first,second)\n  0\n",
          "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value]\n  callbacks[1](42)\n",
          "error E:\n  Bad\nfn main()->int:\n  callback=fn(value:int)->int ! E:value\n  "
          "callbacks=[callback]\n  callbacks[0](42)+1\n",
          "error E:\n  Bad\nfn main()->int:\n  callback=fn(value:int)->int ! E:value\n  "
          "callbacks=[callback]\n  evaluate=fn(fnValue:fn(int)->int ! E)->int ! E:try fnValue(0)\n "
          " values=map(callbacks,evaluate)\n  0\n",

          "fn main()->int:\n  callbacks:[[fn(int)->int]]=[]\n  0\n",
          "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value]\n  "
          "value=tensor(callbacks)\n  0\n",
          "type A:\n  value:int\ntype B:\n  value:int\nfn main()->int:\n  "
          "callbacks:[fn(A)->int]=[fn(value:B):value.value]\n  0\n"})
      require(!analyze(text).ok(), "invalid function array accepted");
    auto separate = analyze(
        "error InputError:\n  Bad:\n    value:int\nerror BuildError:\n  Disabled\nfn "
        "make(offset:int)->[fn(int)->int ! InputError] ! BuildError:\n  if offset==0:fail "
        "BuildError.Disabled\n  callback=fn(value:int)->int ! InputError:\n    if value==0:fail "
        "InputError.Bad {value:offset}\n    value\n  [callback]\nfn main()->int:\n  factory=make\n "
        " callbacks=factory(42) catch fn(error):[fn(value:int)->int ! InputError:value]\n  "
        "callbacks[0](0) catch fn(error):\n    match error:\n      InputError.Bad {value}:value\n");
    require(separate.ok(), "array factory and element channels were conflated");
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
    auto separateNative = lowerNative(*separate.module);
    require(bool(separateNative), separateNative ? "" : separateNative.error().c_str());
#endif
    auto base = analyze(
        "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value):value*2]\n  callbacks[0](21)\n");
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
    auto factory = analyze("fn make()->[fn(int)->int]:\n  [fn(value):value]\nfn main()->int:\n  "
                           "factory=make\n  callbacks=factory()\n  callbacks[0](42)\n");
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
        analyze("fn main()->int:\n  callbacks=map([20],fn(offset):fn(value:int):value+offset)\n  "
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
        "fn inspect(callbacks:[fn(int)->int ! CallError])->[fn()->fn(int)->int]:\n  0\n"));
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
    std::string nested = "int";
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
                            "budget.k", "fn identity(value:int)->int:\n  value\nfn main()->int:\n  "
                                        "callbacks=[identity,identity,identity,identity]\n  0\n")),
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
