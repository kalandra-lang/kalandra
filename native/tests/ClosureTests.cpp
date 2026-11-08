#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
CheckResult analyze(const char *source) {
  return check(parse(std::make_shared<SourceFile>("closures.k", source)));
}
} // namespace
int main() {
  try {
    for (const auto *source :
         {"fn worker(value:i64)->i64:\n  return value\nfn run(value:i64)->i64 ! TaskError:\n  return try "
          "parallel:\n    try await spawn worker(value)\nfn main()->i64:\n  return "
          "f=fn(value:i64):return run(value) catch fn(error):return 0\n  return f(42)\n",
          "fn makeAdder(offset:i64)->fn(i64)->i64:\n  return fn(value:i64):return value+offset\nfn "
          "apply(value:i64,f:fn(i64)->i64)->i64:\n  return f(value)\nfn main()->i64:\n  return "
          "add=makeAdder(10)\n  return apply(22,add)+add(0)\n",
          "fn apply(value:i64,f:fn(i64)->i64)->i64:\n  return f(value)\nfn main()->i64:\n  return "
          "f:fn(i64)->i64=fn(value):return value*2\n  return apply(f(20),fn(value):return value+2)\n",
          "fn make(a:i64)->fn(i64)->fn(i64)->i64:\n  return fn(b:i64):return fn(c:i64):return a+b+c\nfn main()->i64:\n  return "
          "first=make(10)\n  second=first(20)\n  return second(12)\n",
          "fn twice(value:i64)->i64:\n  return value*2\nfn make(f:fn(i64)->i64,offset:\n  return "
          "i64)->fn(i64)->i64:\n  return fn(value:i64):return f(value)+offset\nfn main()->i64:\n  return "
          "first=make(twice,10)\n  second=make(first,10)\n  return second(11)\n",
          "fn double(value:i64)->i64:\n  return value*2\nfn make(offset:i64)->fn(i64)->i64:\n  return "
          "fn(value:i64):return value+offset\nfn choose(flag:bool)->fn(i64)->i64:\n  return if flag:make(21)\n  "
          "else:double\nfn main()->i64:\n  a=choose(true)\n  b=choose(false)\n  return a(0)+b(10)+1\n",
          "type Row:\n  text:String\n  values:[i64]\nfn make(row:Row)->fn(i64)->i64:\n  return "
          "fn(value:i64):return row.values[0]+row.values[1]+length(row.text)+value\nfn main()->i64:\n  return "
          "f=make(Row {text:\"é\"+\"!\",values:[10,20]})\n  return f(10)\n",
          "fn make(values:[i64])->fn(i64)->[i64]:\n  return fn(value:i64):return concat(values,[value])\nfn "
          "main()->i64:\n  values:[i64]=[10,20]\n  f=make(values)\n  result=f(12)\n  "
          "return result[0]+result[1]+result[2]\n",
          "fn make(values:Tensor[i64,?,?])->fn(i64)->Tensor[i64,?,?]:\n  return "
          "fn(value:i64):return values+value\nfn main()->i64:\n  f=make(tensor([[10,10]]))\n  "
          "return reduceSum(f(11))\n",
          "fn make(value:String)->fn(String)->String:\n  return fn(suffix:String):return value+suffix\nfn "
          "main()->i64:\n  f=make(\"é\"+\"🙂\")\n  return length(f(\"!\"))+39\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value:i64):return value+offset\nfn main()->i64:\n  return "
          "f=make(10)\n  values=map([10,12],f)\n  return values[0]+values[1]\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value:i64):return value+offset\nfn main()->i64:\n  return "
          "f=make(10)\n  values=map([10,12],fn(value):return f(value))\n  return values[0]+values[1]\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value:i64):return value+offset\nfn "
          "worker(value:i64)->i64:\n  f=make(value)\n  return f(21)\nfn main()->i64 ! TaskError:\n  return try "
          "parallel:\n    try await spawn worker(21)\n",
          "fn worker(value:i64)->i64:\n  return value*2\nfn make(offset:i64)->fn(i64)->i64:\n  return "
          "fn(value:i64):return value+offset\nfn main()->i64 ! TaskError:\n  f=make(10)\n  return try "
          "parallel:\n    value=try await spawn worker(16)\n    f(value)\n",
          "error E:\n  Bad:\n    value:i64\nfn check(value:i64,offset:i64)->i64 ! E:\n  if "
          "value==0:fail E.Bad {value:offset}\n  return value+offset\nfn make(offset:i64)->fn(i64)->i64 ! "
          "E:\n  return fn(value):return check(value,offset)\nfn main()->i64:\n  f=make(21)\n  a=f(0) catch "
          "fn(error):\n    return match error:\n      E.Bad {value}:value\n  b=f(1) catch fn(error):return 0\n  return "
          "a+b-1\n",
          "error E:\n  Bad:\n    value:i64\nfn check(value:i64,offset:i64)->i64 ! E:\n  if "
          "value==0:fail E.Bad {value:offset}\n  return value+offset\nfn "
          "make(offset:i64)->fn(i64)->i64:\n  return fn(value:i64):return check(value,offset) catch "
          "fn(error):return offset\nfn main()->i64:\n  f=make(21)\n  return f(0)+f(1)-1\n",
          "fn make(offset:i64)->fn()->i64:\n  return fn():return offset\nfn main()->i64:\n  f=make(42)\n  return f()\n",
          "fn make(value:i64)->fn(i64)->i64:\n  return fn(value:i64):return value*2\nfn main()->i64:\n  return "
          "f=make(100)\n  return f(21)\n",
          "error E:\n  Bad\nfn make(offset:i64)->(fn(i64)->i64) ! E:\n  if offset==0:fail E.Bad\n  "
          "return fn(value):return value+offset\nfn main()->i64:\n  f=make(21) catch "
          "fn(error):return fn(value:i64):return value\n  return f(21)\n",
          "fn main()->i64:\n  value:i64=42\n  f=fn(item:i64):return value/item\n  input:[i64]=[]\n  "
          "result=map(input,f)\n  return value+length(result)\n",
          "fn make(value:i64)->fn(i64)->i64:\n  return fn(x:i64):return value+value+x\nfn main()->i64:\n  return "
          "f=make(20)\n  return f(2)\n",
          "error E:\n  Bad:\n    value:i64\nfn check(value:i64,offset:i64)->i64 ! E:\n  if "
          "value==0:fail E.Bad {value:offset}\n  return value+offset\nfn make(offset:i64)->fn(i64)->i64 ! "
          "E:\n  return fn(value):return try check(value,offset)\nfn main()->i64:\n  f=make(21)\n  a=f(0) catch "
          "fn(error):\n    return match error:\n      E.Bad {value}:value\n  b=f(1) catch fn(error):return 0\n  return "
          "a+b-1\n",
          "error E:\n  Bad\nfn make(offset:i64)->fn(i64)->i64 ! E:\n  return fn(value):return value+offset\nfn "
          "main()->i64:\n  f=make(21)\n  return f(21) catch fn(error):return 0\n",
          "type Row:\n  text:String\n  values:[i64]\nerror E:\n  Bad:\n    row:Row\nfn "
          "check(value:i64,row:Row)->i64 ! E:\n  if value==0:fail E.Bad {row:row}\n  "
          "return value+row.values[0]\nfn make(row:Row)->fn(i64)->i64 ! E:\n  return fn(value):return try "
          "check(value,row)\nfn main()->i64:\n  f=make(Row {text:\"é\"+\"!\",values:[20,20]})\n  "
          "return f(0) catch fn(error):\n    return match error:\n      E.Bad "
          "{row}:length(row.text)+row.values[0]+row.values[1]\n",
          "fn makeAdder(offset:i64)->fn(i64)->i64:\n  return fn(value:i64):return value+offset\nfn "
          "apply(value:i64,f:fn(i64)->i64)->i64:\n  return f(value)\nfn main()->i64:\n  return "
          "add=makeAdder(10)\n  return (22->add())+add(0)\n",
          "fn use(flags:[bool],words:[String],value:i64)->i64:\n  return if "
          "flags[0]:length(words[0])+value\n  else:0\nfn "
          "make(flags:[bool],words:[String])->fn(i64)->i64:\n  return "
          "fn(value:i64):return use(flags,words,value)\nfn main()->i64:\n  return "
          "f=make([true,false],[\"é\"+\"!\",\"x\"])\n  return f(40)\n",
          "fn make(offset:i64)->fn(i64)->i64:\n  return fn(value:i64):return value+offset\nfn "
          "probe(input:i64)->i64:\n  f=make(input)\n  return f(2)\nfn main()->i64:\n  return probe(40)\n",
          "type Row:\n  text:String\n  values:[i64]\nfn make(row:Row)->fn(i64)->i64:\n  return "
          "fn(value:i64):return row.values[0]+row.values[1]+length(row.text)+value\nfn "
          "probe(input:i64)->i64:\n  f=make(Row {text:\"é\"+\"!\",values:[10,20]})\n  return f(input)\nfn "
          "main()->i64:\n  return probe(10)\n",
          "type Row:\n  text:String\n  values:[i64]\nfn make(row:Row)->fn(i64)->Row:\n  return "
          "fn(value:i64):return Row {text:row.text+\"!\",values:concat(row.values,[value])}\nfn "
          "worker(value:i64)->Row:\n  f=make(Row {text:\"é\"+\"🙂\",values:[10,20]})\n  "
          "return f(value)\nfn run()->i64 ! TaskError:\n  return try parallel:\n    task=spawn worker(9)\n    "
          "first=try await task\n    second=try await task\n    "
          "first.values[0]+first.values[1]+second.values[2]+length(second.text)\nfn main()->i64 ! "
          "TaskError:\n  return try run()\n"}) {
      auto result = analyze(source);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid closure rejected");
      auto plans = continuation::plan(*result.module);
      require(bool(plans), plans ? "" : plans.error().c_str());
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
      auto native = lowerNative(*result.module);
      require(bool(native), native ? "" : native.error().c_str());
#endif
    }
    // Worker aggregate transport now supports these former rejection cases.
    for (const auto *accepted :
         {"fn make(value:i64)->fn(i64)->i64:\n  targets=[fn(item:i64):return item+value]\n  "
          "return fn(item:i64):return targets[0](item)\nfn main()->i64 ! TaskError:\n  return try parallel:\n    f=try "
          "await spawn make(21)\n    f(21)\n",
          "fn apply(value:i64,f:fn(i64)->i64)->i64:\n  return f(value)\nfn main()->i64 ! TaskError:\n  return "
          "targets=[fn(value:i64):return value*2]\n  f=fn(value:i64):return targets[0](value)\n  return try parallel:\n "
          "   try await spawn apply(21,f)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    for (const auto *source :
         {"fn main()->i64:\n  f=fn(value):return value+1\n  return f(41)\n",
          "fn main()->i64:\n  f:fn(i64)->i64=fn(a,b):return a+b\n  return 0\n",
          "fn main()->i64:\n  f:fn(i64)->i64=fn(value:f32):return value\n  return 0\n",
          "fn main()->i64:\n  f:fn(i64)->String=fn(value):return value\n  return 0\n",
          "fn main()->i64:\n  f=fn(value:i64):return f(value)\n  return 0\n",
          "fn main()->i64:\n  f=fn(value:i64,value:i64):return value\n  return 0\n",
          "type A:\n  value:i64\ntype B:\n  value:i64\nfn main()->i64:\n  return "
          "f:fn(A)->i64=fn(value:B):return value.value\n  return 0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn checked(value:i64)->i64 ! E:\n  fail E.Bad\nfn "
          "main()->i64:\n  f:fn(i64)->i64 ! Other=fn(value):return checked(value)\n  return 0\n",
          "error E:\n  Bad\nfn checked()->i64 ! E:\n  fail E.Bad\nfn main()->i64:\n  return "
          "result=checked()\n  f=fn(value:i64):return result\n  return 0\n",

          "fn worker(value:i64)->i64:\n  return value\nfn main()->i64 ! TaskError:\n  return try parallel:\n    "
          "task=spawn worker(42)\n    f=fn(value:i64):return task\n    try await task\n",
          "error E:\n  Bad\nfn checked()->i64 ! E:\n  fail E.Bad\nfn main()->i64:\n  return "
          "f=fn(value:i64):return checked()+value\n  return 0\n"})
      require(!analyze(source).ok(), "invalid closure accepted");
    auto fixture = analyze("fn make(value:i64)->fn(i64)->i64:\n  return fn(item:i64):return item+value+value\nfn "
                           "main()->i64:\n  f=make(20)\n  return f(2)\n");
    require(fixture.ok(), "capture fixture failed");
    for (unsigned mode = 0; mode < 6; ++mode) {
      auto module = *fixture.module;
      bool changed = false;
      for (ValueId id = 0; id < module.expressions.size(); ++id)
        if (auto *ref = std::get_if<FunctionReference>(&module.expressions[id].operation);
            ref && !ref->captures.empty()) {
          auto &fn = module.functions[ref->function];
          require(ref->captures.size() == 1, "duplicate lexical reads captured more than once");
          if (mode == 0)
            ref->captures.clear();
          if (mode == 1)
            ref->captures[0] = id;
          if (mode == 2)
            fn.captureCount = fn.parameters.size() + 1;
          if (mode == 3)
            module.expressions[ref->captures[0]].type.task = true;
          if (mode == 4)
            module.symbols[fn.parameters.back()].type.element = NumericType::Float32;
          if (mode == 5)
            ref->captures.push_back(ref->captures[0]);
          changed = true;
          break;
        }
      require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
              "invalid capture metadata accepted");
    }
    std::cout << "Managed closure inference, lexical captures and invalid metadata checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
