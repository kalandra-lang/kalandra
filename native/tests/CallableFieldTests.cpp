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
        {"type Config:\n  apply:fn(i64)->i64 ! TaskError\nfn worker(value:i64)->i64:\n  return value\nfn "
         "run(value:i64)->i64 ! TaskError:\n  return try parallel:\n    try await spawn "
         "worker(value)\nfn main()->i64:\n  config=Config {apply:run}\n  return 0\n",
         "type Config:\n  apply: fn(i64)->i64\nfn double(value:i64)->i64:\n  return value*2\nfn "
         "main()->i64:\n  config=Config {apply:double}\n  return config.apply(21)\n",
         "type Config:\n  apply: fn(i64)->i64\nfn make(offset:i64)->Config:\n  return Config "
         "{apply:fn(value):return value+offset}\nfn main()->i64:\n  config=make(20)\n  return config.apply(22)\n",
         "enum Choice:\n  Empty\n  Some:\n    callback:fn(i64)->i64\nfn "
         "make(offset:i64)->Choice:\n  return Choice.Some {callback:fn(value):return offset+value}\nfn "
         "main()->i64:\n  choice=make(20)\n  return match choice:\n    Choice.Empty:0\n    Choice.Some "
         "{callback}:callback(22)\n",
         "type Config:\n  apply: fn(i64)->i64\nfn make()->Config:\n  text=\"é\"+\"!\"\n  "
         "values:[i64]=[20,20]\n  return Config "
         "{apply:fn(value):return values[0]+values[1]+length(text)+value}\nfn main()->i64:\n  "
         "config=make()\n  return config.apply(0)\n",
         "type Config:\n  apply:fn(i64)->i64\n  label:String\ntype Outer:\n  config:Config\nfn "
         "make(offset:i64)->Outer:\n  config=Config {apply:fn(value):return offset+value,label:\"é\"}\n  return "
         "Outer {config:config}\nfn main()->i64:\n  outer=make(20)\n  "
         "f=fn(value:i64):return outer.config.apply(value)\n  return f(22)\n",
         "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  offset:i64=20\n  first=Config "
         "{apply:fn(value):return offset+value}\n  second=first with {apply:fn(value):return value*2}\n  return "
         "first.apply(1)+second.apply(10)+1\n",
         "error E:\n  Bad:\n    value:i64\ntype Config:\n  apply:fn(i64)->i64 ! E\nfn "
         "make(offset:i64)->Config:\n  callback=fn(value:i64)->i64 ! E:\n    if value==0:fail "
         "E.Bad {value:offset}\n    return value\n  return Config {apply:callback}\nfn main()->i64:\n  "
         "config=make(42)\n  return config.apply(0) catch fn(error):\n    return match error:\n      E.Bad "
         "{value}:value\n",
         "type Config:\n  apply:fn(i64)->i64\nerror E:\n  Bad:\n    config:Config\nfn "
         "raise(offset:i64)->i64 ! E:\n  fail E.Bad {config:Config "
         "{apply:fn(value):return offset+value}}\nfn main()->i64:\n  return raise(20) catch fn(error):\n    return "
         "match error:\n      E.Bad {config}:config.apply(22)\n",
         "type Config:\n  apply:fn(i64)->i64\nfn make(offset:i64)->[Config]:\n  return [Config "
         "{apply:fn(value):return offset+value}]\nfn main()->i64:\n  configs=concat(make(20),make(21))\n  "
         "selected=slice(configs,0,1)\n  mapped=map(selected,fn(config):return config.apply(22))\n  "
         "return mapped[0]\n",
         "type Config:\n  apply:fn(i64)->i64\nfn worker(offset:i64)->i64:\n  config=Config "
         "{apply:fn(value):return offset+value}\n  return config.apply(22)\nfn main()->i64 ! TaskError:\n  return try "
         "parallel:\n    try await spawn worker(20)\n",
         "type Config:\n  apply:fn(i64)->i64\nfn worker(value:i64)->i64:\n  return value\nfn main()->i64 "
         "! TaskError:\n  config=Config {apply:fn(value):return value*2}\n  return try parallel:\n    value=try "
         "await spawn worker(21)\n    config.apply(value)\n",
         "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  maker=fn(offset:i64)->Config:\n   "
         " return Config {apply:fn(value):return value+offset}\n  configs=map([20],maker)\n  "
         "return configs[0].apply(22)\n",
         "type Config:\n  make:fn(i64)->fn(i64)->i64\nfn main()->i64:\n  config=Config "
         "{make:fn(offset):return fn(value:i64):return value+offset}\n  return config.make(20)(22)\n",
         "type Config:\n  apply:fn(Tensor[i64,2,2])->Tensor[i64,2,2]\nfn main()->i64:\n  "
         "offset:i64=1\n  config=Config {apply:fn(values):return values+offset}\n  return "
         "reduceSum(config.apply(tensor([[10,10],[10,8]])))\n",
         "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config "
         "{apply:fn(value):return value*2}\n  first=10 -> config.apply()\n  second=11 -> config.apply\n  "
         "return first+second\n",
         "enum Choice:\n  Empty\n  Some:\n    callback:fn(i64)->i64\nfn main()->i64:\n  "
         "choice=Choice.Empty\n  return match choice:\n    Choice.Empty:42\n    Choice.Some "
         "{callback}:callback(21)\n",
         "enum Choice:\n  Number:\n    callback:fn(i64)->i64\n  Text:\n    "
         "callback:fn(String)->i64\nfn select(flag:bool)->Choice:\n  return if flag:Choice.Number "
         "{callback:fn(value):return value*2}\n  else:Choice.Text {callback:fn(text):return length(text)+40}\nfn "
         "main()->i64:\n  return match select(false):\n    Choice.Number {callback}:callback(21)\n    "
         "Choice.Text {callback}:callback(\"é!\")\n",
         "type Config:\n  apply:fn(i64)->i64\nerror A:\n  Bad:\n    config:Config\nerror B:\n  "
         "Missing\ntype Both=B | A | B\nfn raise(offset:i64)->i64 ! Both:\n  fail A.Bad "
         "{config:Config {apply:fn(value):return offset+value}}\nfn main()->i64:\n  return raise(20) catch "
         "fn(error):\n    return match error:\n      A.Bad {config}:config.apply(22)\n      B.Missing:0\n",
         "error E:\n  Bad:\n    value:i64\nerror BuildError:\n  Disabled\ntype Config:\n  "
         "make:fn(bool)->(fn(i64)->i64 ! E) ! BuildError\nfn main()->i64:\n  offset:i64=42\n  "
         "factory=fn(blocked:bool)->(fn(i64)->i64 ! E) ! BuildError:\n    if blocked:fail "
         "BuildError.Disabled\n    return fn(value:i64)->i64 ! E:\n      if value==0:fail E.Bad "
         "{value:offset}\n      return value\n  config=Config {make:factory}\n  f=config.make(false) "
         "catch fn(error):return fn(value:i64)->i64 ! E:return value\n  return f(0) catch fn(error):\n    return match "
         "error:\n      E.Bad {value}:value\n",
         "type Config:\n  label:String\n  enabled:bool\n  apply:fn(i64)->i64\n  values:[i64]\n  "
         "second:fn(bool)->i64\nfn main()->i64:\n  offset:i64=20\n  second=fn(flag:bool)->i64:\n   "
         " return if flag:1\n    else:0\n  config=Config "
         "{label:\"é!\",enabled:true,apply:fn(value):return offset+value,values:[19],second:second}\n  return "
         "config.apply(config.values[0])+config.second(config.enabled)+length(config.label)\n",
         "type Config:\n  apply:fn(i64)->i64\nerror E:\n  Bad:\n    config:Config\nfn "
         "raise(offset:i64)->i64 ! E:\n  fail E.Bad {config:Config "
         "{apply:fn(value):return offset+value}}\nfn worker(offset:i64)->i64:\n  return raise(offset) catch "
         "fn(error):\n    return match error:\n      E.Bad {config}:config.apply(22)\nfn main()->i64 ! "
         "TaskError:\n  return try parallel:\n    try await spawn worker(20)\n",
         "type Config:\n  apply:fn(i64)->i64\nerror E:\n  Bad\nfn make()->Config ! E:\n  fail "
         "E.Bad\nfn main()->i64:\n  offset:i64=20\n  config=make() catch fn(error):return Config "
         "{apply:fn(value):return value+offset}\n  return config.apply(22)\n",
         "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  offset:i64=20\n  first=Config "
         "{apply:fn(value):return value+offset}\n  second=Config {apply:fn(value):return value*2}\n  config=if "
         "length(\"é\")==1:first\n  else:second\n  alias=config.apply\n  return alias(22)\n",
         "type Config:\n  apply:fn(String)->String\nfn make(prefix:String)->Config:\n  return Config "
         "{apply:fn(value):return prefix+value}\nfn main()->i64:\n  config=make(\"é\"+\"!\")\n  return "
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
         {"type Config:\n  apply:fn(i64)->i64\nfn worker(config:Config)->i64:\n  return "
          "config.apply(42)\nfn main()->i64 ! TaskError:\n  config=Config "
          "{apply:fn(value):return value}\n  return try parallel:\n    try await spawn worker(config)\n",
          "type Config:\n  apply:fn(i64)->i64\ntype Outer:\n  values:[Config]\nfn "
          "worker(outer:Outer)->i64:\n  return outer.values[0].apply(42)\nfn main()->i64 ! TaskError:\n  "
          "outer=Outer {values:[Config {apply:fn(value):return value}]}\n  return try parallel:\n    try await "
          "spawn worker(outer)\n",
          "type Config:\n  apply:fn(i64)->i64\nfn worker(value:i64)->Config:\n  return Config "
          "{apply:fn(input):return input+value}\nfn main()->i64 ! TaskError:\n  return try parallel:\n    "
          "config=try await spawn worker(20)\n    config.apply(22)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    for (const auto *text :
         {"type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config "
          "{apply:fn(value:f64)->f64:return value}\n  return 0\n",
          "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config "
          "{apply:fn(value:i64)->f64:return f64(value)}\n  return 0\n",
          "error E:\n  Bad\ntype Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config "
          "{apply:fn(value:i64)->i64 ! E:return value}\n  return 0\n",
          "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config "
          "{apply:fn(value):return value}\n  updated=config with {apply:fn(value:f64):return value}\n  return 0\n",
          "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config {}\n  return 0\n",
          "error E:\n  Bad\ntype Config:\n  apply:fn(i64)->i64 ! E\nfn main()->i64:\n  "
          "config=Config {apply:fn(value:i64)->i64 ! E:return value}\n  return config.apply(42)+1\n",
          "enum Choice:\n  Some:\n    callback:fn(i64)->i64\nfn main()->i64:\n  choice=Choice.Some "
          "{callback:fn(value):return value}\n  return choice.callback(42)\n",
          "enum Choice:\n  Some:\n    callback:fn(i64)->i64\nfn main()->i64:\n  choice=Choice.Some "
          "{callback:0}\n  return 0\n",
          "type Config:\n  apply:fn(Tensor[i64,2])->i64\nfn sum(values:Tensor[i64,3])->i64:\n  return "
          "reduceSum(values)\nfn main()->i64:\n  config=Config {apply:sum}\n  return 0\n",
          "type A:\n  value:i64\ntype B:\n  value:i64\ntype Config:\n  apply:fn(A)->i64\nfn "
          "accept(value:B)->i64:\n  return value.value\nfn main()->i64:\n  config=Config {apply:accept}\n "
          " return 0\n",

          "type Config:\n  apply:fn(Config)->i64\n  next:Config\nfn main()->i64:\n  return 0\n",
          "fn main()->i64:\n  values=[fn(value:i64):return value,fn(value:f64):return value]\n  return 0\n"})
      require(!analyze(text).ok(), "invalid callable field use accepted");

    auto nominal =
        analyze("type Config:\n  value:i64\n  apply:fn(Config)->i64\n"
                "fn read(config:Config)->i64:\n  return config.value\n"
                "fn main()->i64:\n  config=Config {value:42,apply:read}\n  return config.apply(config)\n");
    require(nominal.ok(), "nominal self-signature rejected as stored recursion");
    auto base = analyze("type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config "
                        "{apply:fn(value):return value*2}\n  return config.apply(21)\n");
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
    auto unused = analyze("type Config:\n  apply:fn(i64)->i64\nenum Choice:\n  Empty\n  Some:\n    "
                          "callback:fn()->i64\nfn main()->i64:\n  return 0\n");
    require(unused.ok(), "unused callable field declarations rejected");
    require(functionValueCapabilityError(*unused.module, NumericTarget::AppleGpu).has_value(),
            "unused GPU callable fields accepted");
    auto unionBase = analyze(
        "error A:\n  Bad:\n    callback:fn(i64)->i64\nerror B:\n  Missing\ntype Both=B | A\nfn "
        "raise(offset:i64)->i64 ! Both:\n  fail A.Bad {callback:fn(value):return offset+value}\nfn "
        "main()->i64:\n  return raise(20) catch fn(error):\n    return match error:\n      A.Bad "
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
        "error A:\n  Bad:\n    callback:fn(i64)->i64\nerror B:\n  Missing\ntype Both=B | A\nfn "
        "raise(offset:i64)->i64 ! Both:\n  fail A.Bad {callback:fn(value):return offset+value}\nfn "
        "main()->i64:\n  return raise(20) catch fn(error):\n    return match error:\n      A.Bad "
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
                  "budget.k", "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  return 0\n")),
              limits);
    require(!budget.ok(), "code/environment pair bypassed inline field budget");
    std::cout
        << "Callable record/enum fields, ownership boundaries and signature validation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
