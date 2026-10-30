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
  return check(parse(std::make_shared<SourceFile>("callable-fields.k", text)));
}
} // namespace
int main() {
  try {
    for (
        const auto *text :
        {"type Config:\n  apply:fn(int)->int ! TaskError\nfn worker(value:int)->int:\n  value\nfn "
         "run(value:int)->int ! TaskError:\n  try parallel:\n    try await spawn "
         "worker(value)\nfn main()->int:\n  config=Config {apply:run}\n  0\n",
         "type Config:\n  apply: fn(int)->int\nfn double(value:int)->int:\n  value*2\nfn "
         "main()->int:\n  config=Config {apply:double}\n  config.apply(21)\n",
         "type Config:\n  apply: fn(int)->int\nfn make(offset:int)->Config:\n  Config "
         "{apply:fn(value):value+offset}\nfn main()->int:\n  config=make(20)\n  config.apply(22)\n",
         "enum Choice:\n  Empty\n  Some:\n    callback:fn(int)->int\nfn "
         "make(offset:int)->Choice:\n  Choice.Some {callback:fn(value):offset+value}\nfn "
         "main()->int:\n  choice=make(20)\n  match choice:\n    Choice.Empty:0\n    Choice.Some "
         "{callback}:callback(22)\n",
         "type Config:\n  apply: fn(int)->int\nfn make()->Config:\n  text=\"é\"+\"!\"\n  "
         "values:[int]=[20,20]\n  Config "
         "{apply:fn(value):values[0]+values[1]+length(text)+value}\nfn main()->int:\n  "
         "config=make()\n  config.apply(0)\n",
         "type Config:\n  apply:fn(int)->int\n  label:String\ntype Outer:\n  config:Config\nfn "
         "make(offset:int)->Outer:\n  config=Config {apply:fn(value):offset+value,label:\"é\"}\n  "
         "Outer {config:config}\nfn main()->int:\n  outer=make(20)\n  "
         "f=fn(value:int):outer.config.apply(value)\n  f(22)\n",
         "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  offset:int=20\n  first=Config "
         "{apply:fn(value):offset+value}\n  second=first with {apply:fn(value):value*2}\n  "
         "first.apply(1)+second.apply(10)+1\n",
         "error E:\n  Bad:\n    value:int\ntype Config:\n  apply:fn(int)->int ! E\nfn "
         "make(offset:int)->Config:\n  callback=fn(value:int)->int ! E:\n    if value==0:fail "
         "E.Bad {value:offset}\n    value\n  Config {apply:callback}\nfn main()->int:\n  "
         "config=make(42)\n  config.apply(0) catch fn(error):\n    match error:\n      E.Bad "
         "{value}:value\n",
         "type Config:\n  apply:fn(int)->int\nerror E:\n  Bad:\n    config:Config\nfn "
         "raise(offset:int)->int ! E:\n  fail E.Bad {config:Config "
         "{apply:fn(value):offset+value}}\nfn main()->int:\n  raise(20) catch fn(error):\n    "
         "match error:\n      E.Bad {config}:config.apply(22)\n",
         "type Config:\n  apply:fn(int)->int\nfn make(offset:int)->[Config]:\n  [Config "
         "{apply:fn(value):offset+value}]\nfn main()->int:\n  configs=concat(make(20),make(21))\n  "
         "selected=slice(configs,0,1)\n  mapped=map(selected,fn(config):config.apply(22))\n  "
         "mapped[0]\n",
         "type Config:\n  apply:fn(int)->int\nfn worker(offset:int)->int:\n  config=Config "
         "{apply:fn(value):offset+value}\n  config.apply(22)\nfn main()->int ! TaskError:\n  try "
         "parallel:\n    try await spawn worker(20)\n",
         "type Config:\n  apply:fn(int)->int\nfn worker(value:int)->int:\n  value\nfn main()->int "
         "! TaskError:\n  config=Config {apply:fn(value):value*2}\n  try parallel:\n    value=try "
         "await spawn worker(21)\n    config.apply(value)\n",
         "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  maker=fn(offset:int)->Config:\n   "
         " Config {apply:fn(value):value+offset}\n  configs=map([20],maker)\n  "
         "configs[0].apply(22)\n",
         "type Config:\n  make:fn(int)->fn(int)->int\nfn main()->int:\n  config=Config "
         "{make:fn(offset):fn(value:int):value+offset}\n  config.make(20)(22)\n",
         "type Config:\n  apply:fn(Tensor[int,2,2])->Tensor[int,2,2]\nfn main()->int:\n  "
         "offset:int=1\n  config=Config {apply:fn(values):values+offset}\n  "
         "reduceSum(config.apply(tensor([[10,10],[10,8]])))\n",
         "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config "
         "{apply:fn(value):value*2}\n  first=10 -> config.apply()\n  second=11 -> config.apply\n  "
         "first+second\n",
         "enum Choice:\n  Empty\n  Some:\n    callback:fn(int)->int\nfn main()->int:\n  "
         "choice=Choice.Empty\n  match choice:\n    Choice.Empty:42\n    Choice.Some "
         "{callback}:callback(21)\n",
         "enum Choice:\n  Number:\n    callback:fn(int)->int\n  Text:\n    "
         "callback:fn(String)->int\nfn select(flag:Bool)->Choice:\n  if flag:Choice.Number "
         "{callback:fn(value):value*2}\n  else:Choice.Text {callback:fn(text):length(text)+40}\nfn "
         "main()->int:\n  match select(false):\n    Choice.Number {callback}:callback(21)\n    "
         "Choice.Text {callback}:callback(\"é!\")\n",
         "type Config:\n  apply:fn(int)->int\nerror A:\n  Bad:\n    config:Config\nerror B:\n  "
         "Missing\ntype Both=B | A | B\nfn raise(offset:int)->int ! Both:\n  fail A.Bad "
         "{config:Config {apply:fn(value):offset+value}}\nfn main()->int:\n  raise(20) catch "
         "fn(error):\n    match error:\n      A.Bad {config}:config.apply(22)\n      B.Missing:0\n",
         "error E:\n  Bad:\n    value:int\nerror BuildError:\n  Disabled\ntype Config:\n  "
         "make:fn(Bool)->(fn(int)->int ! E) ! BuildError\nfn main()->int:\n  offset:int=42\n  "
         "factory=fn(blocked:Bool)->(fn(int)->int ! E) ! BuildError:\n    if blocked:fail "
         "BuildError.Disabled\n    fn(value:int)->int ! E:\n      if value==0:fail E.Bad "
         "{value:offset}\n      value\n  config=Config {make:factory}\n  f=config.make(false) "
         "catch fn(error):fn(value:int)->int ! E:value\n  f(0) catch fn(error):\n    match "
         "error:\n      E.Bad {value}:value\n",
         "type Config:\n  label:String\n  enabled:Bool\n  apply:fn(int)->int\n  values:[int]\n  "
         "second:fn(Bool)->int\nfn main()->int:\n  offset:int=20\n  second=fn(flag:Bool)->int:\n   "
         " if flag:1\n    else:0\n  config=Config "
         "{label:\"é!\",enabled:true,apply:fn(value):offset+value,values:[19],second:second}\n  "
         "config.apply(config.values[0])+config.second(config.enabled)+length(config.label)\n",
         "type Config:\n  apply:fn(int)->int\nerror E:\n  Bad:\n    config:Config\nfn "
         "raise(offset:int)->int ! E:\n  fail E.Bad {config:Config "
         "{apply:fn(value):offset+value}}\nfn worker(offset:int)->int:\n  raise(offset) catch "
         "fn(error):\n    match error:\n      E.Bad {config}:config.apply(22)\nfn main()->int ! "
         "TaskError:\n  try parallel:\n    try await spawn worker(20)\n",
         "type Config:\n  apply:fn(int)->int\nerror E:\n  Bad\nfn make()->Config ! E:\n  fail "
         "E.Bad\nfn main()->int:\n  offset:int=20\n  config=make() catch fn(error):Config "
         "{apply:fn(value):value+offset}\n  config.apply(22)\n",
         "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  offset:int=20\n  first=Config "
         "{apply:fn(value):value+offset}\n  second=Config {apply:fn(value):value*2}\n  config=if "
         "length(\"é\")==1:first\n  else:second\n  alias=config.apply\n  alias(22)\n",
         "type Config:\n  apply:fn(String)->String\nfn make(prefix:String)->Config:\n  Config "
         "{apply:fn(value):prefix+value}\nfn main()->int:\n  config=make(\"é\"+\"!\")\n  "
         "length(config.apply(\"🙂\"))+39\n"}) {
      auto result = analyze(text);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid callable field program rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "valid field metadata rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), plan ? "" : plan.error().c_str());
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
      auto native = lowerNative(*result.module);
      require(bool(native), native ? "" : native.error().c_str());
#endif
    }
    // Worker aggregate transport now supports these former rejection cases.
    for (const auto *accepted :
         {"type Config:\n  apply:fn(int)->int\nfn worker(config:Config)->int:\n  "
          "config.apply(42)\nfn main()->int ! TaskError:\n  config=Config "
          "{apply:fn(value):value}\n  try parallel:\n    try await spawn worker(config)\n",
          "type Config:\n  apply:fn(int)->int\ntype Outer:\n  values:[Config]\nfn "
          "worker(outer:Outer)->int:\n  outer.values[0].apply(42)\nfn main()->int ! TaskError:\n  "
          "outer=Outer {values:[Config {apply:fn(value):value}]}\n  try parallel:\n    try await "
          "spawn worker(outer)\n",
          "type Config:\n  apply:fn(int)->int\nfn worker(value:int)->Config:\n  Config "
          "{apply:fn(input):input+value}\nfn main()->int ! TaskError:\n  try parallel:\n    "
          "config=try await spawn worker(20)\n    config.apply(22)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    for (const auto *text :
         {"type Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config "
          "{apply:fn(value:float)->float:value}\n  0\n",
          "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config "
          "{apply:fn(value:int)->float:float(value)}\n  0\n",
          "error E:\n  Bad\ntype Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config "
          "{apply:fn(value:int)->int ! E:value}\n  0\n",
          "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config "
          "{apply:fn(value):value}\n  updated=config with {apply:fn(value:float):value}\n  0\n",
          "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config {}\n  0\n",
          "error E:\n  Bad\ntype Config:\n  apply:fn(int)->int ! E\nfn main()->int:\n  "
          "config=Config {apply:fn(value:int)->int ! E:value}\n  config.apply(42)+1\n",
          "enum Choice:\n  Some:\n    callback:fn(int)->int\nfn main()->int:\n  choice=Choice.Some "
          "{callback:fn(value):value}\n  choice.callback(42)\n",
          "enum Choice:\n  Some:\n    callback:fn(int)->int\nfn main()->int:\n  choice=Choice.Some "
          "{callback:0}\n  0\n",
          "type Config:\n  apply:fn(Tensor[int,2])->int\nfn sum(values:Tensor[int,3])->int:\n  "
          "reduceSum(values)\nfn main()->int:\n  config=Config {apply:sum}\n  0\n",
          "type A:\n  value:int\ntype B:\n  value:int\ntype Config:\n  apply:fn(A)->int\nfn "
          "accept(value:B)->int:\n  value.value\nfn main()->int:\n  config=Config {apply:accept}\n "
          " 0\n",

          "type Config:\n  apply:fn(Config)->int\n  next:Config\nfn main()->int:\n  0\n",
          "fn main()->int:\n  values=[fn(value:int):value,fn(value:float):value]\n  0\n"})
      require(!analyze(text).ok(), "invalid callable field use accepted");

    auto nominal =
        analyze("type Config:\n  value:int\n  apply:fn(Config)->int\n"
                "fn read(config:Config)->int:\n  config.value\n"
                "fn main()->int:\n  config=Config {value:42,apply:read}\n  config.apply(config)\n");
    require(nominal.ok(), "nominal self-signature rejected as stored recursion");
    auto base = analyze("type Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config "
                        "{apply:fn(value):value*2}\n  config.apply(21)\n");
    require(base.ok(), "forged metadata fixture rejected");
    for (unsigned mode = 0; mode < 8; ++mode) {
      auto module = *base.module;
      bool changed = false;
      for (std::size_t id = 0; id < module.expressions.size(); ++id) {
        auto &expression = module.expressions[id];
        if (auto *read = std::get_if<RecordFieldRead>(&expression.operation);
            read && expression.type.builtin == BuiltinValue::Function && mode < 4) {
          if (mode == 0) {
            auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
            signature->parameters[0].element = NumericType::Float64;
            expression.type.callable = signature;
          }
          if (mode == 1)
            read->field = SIZE_MAX;
          if (mode == 2)
            read->input = id;
          if (mode == 3)
            expression.type.callable.reset();
          changed = true;
          break;
        }
        if (auto *value = std::get_if<RecordValue>(&expression.operation); value && mode == 4) {
          value->fields[0].value = module.functions.back().result;
          changed = true;
          break;
        }
      }
      if (mode >= 5) {
        auto &field = module.records[0].fields[0].type;
        if (mode == 5)
          field.callable.reset();
        if (mode == 6)
          field.tensor = true;
        if (mode == 7) {
          auto signature = std::make_shared<FunctionSignature>(*field.callable);
          signature->result.element = NumericType::Float64;
          field.callable = signature;
        }
        changed = true;
      }
      require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
              "forged callable field metadata accepted");
    }
    auto unused = analyze("type Config:\n  apply:fn(int)->int\nenum Choice:\n  Empty\n  Some:\n    "
                          "callback:fn()->int\nfn main()->int:\n  0\n");
    require(unused.ok(), "unused callable field declarations rejected");
    require(functionValueCapabilityError(*unused.module, NumericTarget::AppleGpu).has_value(),
            "unused GPU callable fields accepted");
    auto unionBase = analyze(
        "error A:\n  Bad:\n    callback:fn(int)->int\nerror B:\n  Missing\ntype Both=B | A\nfn "
        "raise(offset:int)->int ! Both:\n  fail A.Bad {callback:fn(value):offset+value}\nfn "
        "main()->int:\n  raise(20) catch fn(error):\n    match error:\n      A.Bad "
        "{callback}:callback(22)\n      B.Missing:0\n");
    require(unionBase.ok(), "callable error union fixture rejected");
    auto forgedUnion = *unionBase.module;
    bool forged = false;
    for (auto &record : forgedUnion.records)
      if (!record.errorMembers.empty())
        for (auto &field : record.fields)
          if (field.type.builtin == BuiltinValue::Function) {
            auto signature = std::make_shared<FunctionSignature>(*field.type.callable);
            signature->result.element = NumericType::Float64;
            field.type.callable = signature;
            forged = true;
          }
    require(forged && numericCapabilityError(forgedUnion, NumericTarget::Cpu).has_value(),
            "canonical union changed its callable payload signature");
    auto direct = analyze(
        "error A:\n  Bad:\n    callback:fn(int)->int\nerror B:\n  Missing\ntype Both=B | A\nfn "
        "raise(offset:int)->int ! Both:\n  fail A.Bad {callback:fn(value):offset+value}\nfn "
        "main()->int:\n  raise(20) catch fn(error):\n    match error:\n      A.Bad "
        "{callback}:callback(22)\n      B.Missing:0\n");
    require(direct.ok(), "direct callable error field rejected");
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
    auto directNative = lowerNative(*direct.module);
    require(bool(directNative), directNative ? "" : directNative.error().c_str());
#endif
    SemanticLimits limits;
    limits.maxValues = 1;
    auto budget =
        check(parse(std::make_shared<SourceFile>(
                  "budget.k", "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  0\n")),
              limits);
    require(!budget.ok(), "code/environment pair bypassed inline field budget");
    std::cout
        << "Callable record/enum fields, ownership boundaries and signature validation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
