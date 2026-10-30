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
         {"fn worker(value:int)->int:\n  value\nfn run(value:int)->int ! TaskError:\n  try "
          "parallel:\n    try await spawn worker(value)\nfn main()->int:\n  "
          "f=fn(value:int):run(value) catch fn(error):0\n  f(42)\n",
          "fn makeAdder(offset:int)->fn(int)->int:\n  fn(value:int):value+offset\nfn "
          "apply(value:int,f:fn(int)->int)->int:\n  f(value)\nfn main()->int:\n  "
          "add=makeAdder(10)\n  apply(22,add)+add(0)\n",
          "fn apply(value:int,f:fn(int)->int)->int:\n  f(value)\nfn main()->int:\n  "
          "f:fn(int)->int=fn(value):value*2\n  apply(f(20),fn(value):value+2)\n",
          "fn make(a:int)->fn(int)->fn(int)->int:\n  fn(b:int):fn(c:int):a+b+c\nfn main()->int:\n  "
          "first=make(10)\n  second=first(20)\n  second(12)\n",
          "fn twice(value:int)->int:\n  value*2\nfn make(f:fn(int)->int,offset:\n  "
          "int)->fn(int)->int:\n  fn(value:int):f(value)+offset\nfn main()->int:\n  "
          "first=make(twice,10)\n  second=make(first,10)\n  second(11)\n",
          "fn double(value:int)->int:\n  value*2\nfn make(offset:int)->fn(int)->int:\n  "
          "fn(value:int):value+offset\nfn choose(flag:Bool)->fn(int)->int:\n  if flag:make(21)\n  "
          "else:double\nfn main()->int:\n  a=choose(true)\n  b=choose(false)\n  a(0)+b(10)+1\n",
          "type Row:\n  text:String\n  values:[int]\nfn make(row:Row)->fn(int)->int:\n  "
          "fn(value:int):row.values[0]+row.values[1]+length(row.text)+value\nfn main()->int:\n  "
          "f=make(Row {text:\"é\"+\"!\",values:[10,20]})\n  f(10)\n",
          "fn make(values:[int])->fn(int)->[int]:\n  fn(value:int):concat(values,[value])\nfn "
          "main()->int:\n  values:[int]=[10,20]\n  f=make(values)\n  result=f(12)\n  "
          "result[0]+result[1]+result[2]\n",
          "fn make(values:Tensor[int,?,?])->fn(int)->Tensor[int,?,?]:\n  "
          "fn(value:int):values+value\nfn main()->int:\n  f=make(tensor([[10,10]]))\n  "
          "reduceSum(f(11))\n",
          "fn make(value:String)->fn(String)->String:\n  fn(suffix:String):value+suffix\nfn "
          "main()->int:\n  f=make(\"é\"+\"🙂\")\n  length(f(\"!\"))+39\n",
          "fn make(offset:int)->fn(int)->int:\n  fn(value:int):value+offset\nfn main()->int:\n  "
          "f=make(10)\n  values=map([10,12],f)\n  values[0]+values[1]\n",
          "fn make(offset:int)->fn(int)->int:\n  fn(value:int):value+offset\nfn main()->int:\n  "
          "f=make(10)\n  values=map([10,12],fn(value):f(value))\n  values[0]+values[1]\n",
          "fn make(offset:int)->fn(int)->int:\n  fn(value:int):value+offset\nfn "
          "worker(value:int)->int:\n  f=make(value)\n  f(21)\nfn main()->int ! TaskError:\n  try "
          "parallel:\n    try await spawn worker(21)\n",
          "fn worker(value:int)->int:\n  value*2\nfn make(offset:int)->fn(int)->int:\n  "
          "fn(value:int):value+offset\nfn main()->int ! TaskError:\n  f=make(10)\n  try "
          "parallel:\n    value=try await spawn worker(16)\n    f(value)\n",
          "error E:\n  Bad:\n    value:int\nfn check(value:int,offset:int)->int ! E:\n  if "
          "value==0:fail E.Bad {value:offset}\n  value+offset\nfn make(offset:int)->fn(int)->int ! "
          "E:\n  fn(value):check(value,offset)\nfn main()->int:\n  f=make(21)\n  a=f(0) catch "
          "fn(error):\n    match error:\n      E.Bad {value}:value\n  b=f(1) catch fn(error):0\n  "
          "a+b-1\n",
          "error E:\n  Bad:\n    value:int\nfn check(value:int,offset:int)->int ! E:\n  if "
          "value==0:fail E.Bad {value:offset}\n  value+offset\nfn "
          "make(offset:int)->fn(int)->int:\n  fn(value:int):check(value,offset) catch "
          "fn(error):offset\nfn main()->int:\n  f=make(21)\n  f(0)+f(1)-1\n",
          "fn make(offset:int)->fn()->int:\n  fn():offset\nfn main()->int:\n  f=make(42)\n  f()\n",
          "fn make(value:int)->fn(int)->int:\n  fn(value:int):value*2\nfn main()->int:\n  "
          "f=make(100)\n  f(21)\n",
          "error E:\n  Bad\nfn make(offset:int)->(fn(int)->int) ! E:\n  if offset==0:fail E.Bad\n  "
          "fn(value):value+offset\nfn main()->int:\n  f=make(21) catch "
          "fn(error):fn(value:int):value\n  f(21)\n",
          "fn main()->int:\n  value:int=42\n  f=fn(item:int):value/item\n  input:[int]=[]\n  "
          "result=map(input,f)\n  value+length(result)\n",
          "fn make(value:int)->fn(int)->int:\n  fn(x:int):value+value+x\nfn main()->int:\n  "
          "f=make(20)\n  f(2)\n",
          "error E:\n  Bad:\n    value:int\nfn check(value:int,offset:int)->int ! E:\n  if "
          "value==0:fail E.Bad {value:offset}\n  value+offset\nfn make(offset:int)->fn(int)->int ! "
          "E:\n  fn(value):try check(value,offset)\nfn main()->int:\n  f=make(21)\n  a=f(0) catch "
          "fn(error):\n    match error:\n      E.Bad {value}:value\n  b=f(1) catch fn(error):0\n  "
          "a+b-1\n",
          "error E:\n  Bad\nfn make(offset:int)->fn(int)->int ! E:\n  fn(value):value+offset\nfn "
          "main()->int:\n  f=make(21)\n  f(21) catch fn(error):0\n",
          "type Row:\n  text:String\n  values:[int]\nerror E:\n  Bad:\n    row:Row\nfn "
          "check(value:int,row:Row)->int ! E:\n  if value==0:fail E.Bad {row:row}\n  "
          "value+row.values[0]\nfn make(row:Row)->fn(int)->int ! E:\n  fn(value):try "
          "check(value,row)\nfn main()->int:\n  f=make(Row {text:\"é\"+\"!\",values:[20,20]})\n  "
          "f(0) catch fn(error):\n    match error:\n      E.Bad "
          "{row}:length(row.text)+row.values[0]+row.values[1]\n",
          "fn makeAdder(offset:int)->fn(int)->int:\n  fn(value:int):value+offset\nfn "
          "apply(value:int,f:fn(int)->int)->int:\n  f(value)\nfn main()->int:\n  "
          "add=makeAdder(10)\n  (22->add())+add(0)\n",
          "fn use(flags:[Bool],words:[String],value:int)->int:\n  if "
          "flags[0]:length(words[0])+value\n  else:0\nfn "
          "make(flags:[Bool],words:[String])->fn(int)->int:\n  "
          "fn(value:int):use(flags,words,value)\nfn main()->int:\n  "
          "f=make([true,false],[\"é\"+\"!\",\"x\"])\n  f(40)\n",
          "fn make(offset:int)->fn(int)->int:\n  fn(value:int):value+offset\nfn "
          "probe(input:int)->int:\n  f=make(input)\n  f(2)\nfn main()->int:\n  probe(40)\n",
          "type Row:\n  text:String\n  values:[int]\nfn make(row:Row)->fn(int)->int:\n  "
          "fn(value:int):row.values[0]+row.values[1]+length(row.text)+value\nfn "
          "probe(input:int)->int:\n  f=make(Row {text:\"é\"+\"!\",values:[10,20]})\n  f(input)\nfn "
          "main()->int:\n  probe(10)\n",
          "type Row:\n  text:String\n  values:[int]\nfn make(row:Row)->fn(int)->Row:\n  "
          "fn(value:int):Row {text:row.text+\"!\",values:concat(row.values,[value])}\nfn "
          "worker(value:int)->Row:\n  f=make(Row {text:\"é\"+\"🙂\",values:[10,20]})\n  "
          "f(value)\nfn run()->int ! TaskError:\n  try parallel:\n    task=spawn worker(9)\n    "
          "first=try await task\n    second=try await task\n    "
          "first.values[0]+first.values[1]+second.values[2]+length(second.text)\nfn main()->int ! "
          "TaskError:\n  try run()\n"}) {
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
         {"fn make(value:int)->fn(int)->int:\n  targets=[fn(item:int):item+value]\n  "
          "fn(item:int):targets[0](item)\nfn main()->int ! TaskError:\n  try parallel:\n    f=try "
          "await spawn make(21)\n    f(21)\n",
          "fn apply(value:int,f:fn(int)->int)->int:\n  f(value)\nfn main()->int ! TaskError:\n  "
          "targets=[fn(value:int):value*2]\n  f=fn(value:int):targets[0](value)\n  try parallel:\n "
          "   try await spawn apply(21,f)\n"})
      require(analyze(accepted).ok(), "supported worker aggregate rejected");
    for (const auto *source :
         {"fn main()->int:\n  f=fn(value):value+1\n  f(41)\n",
          "fn main()->int:\n  f:fn(int)->int=fn(a,b):a+b\n  0\n",
          "fn main()->int:\n  f:fn(int)->int=fn(value:float32):value\n  0\n",
          "fn main()->int:\n  f:fn(int)->String=fn(value):value\n  0\n",
          "fn main()->int:\n  f=fn(value:int):f(value)\n  0\n",
          "fn main()->int:\n  f=fn(value:int,value:int):value\n  0\n",
          "type A:\n  value:int\ntype B:\n  value:int\nfn main()->int:\n  "
          "f:fn(A)->int=fn(value:B):value.value\n  0\n",
          "error E:\n  Bad\nerror Other:\n  Bad\nfn checked(value:int)->int ! E:\n  fail E.Bad\nfn "
          "main()->int:\n  f:fn(int)->int ! Other=fn(value):checked(value)\n  0\n",
          "error E:\n  Bad\nfn checked()->int ! E:\n  fail E.Bad\nfn main()->int:\n  "
          "result=checked()\n  f=fn(value:int):result\n  0\n",

          "fn worker(value:int)->int:\n  value\nfn main()->int ! TaskError:\n  try parallel:\n    "
          "task=spawn worker(42)\n    f=fn(value:int):task\n    try await task\n",
          "error E:\n  Bad\nfn checked()->int ! E:\n  fail E.Bad\nfn main()->int:\n  "
          "f=fn(value:int):checked()+value\n  0\n"})
      require(!analyze(source).ok(), "invalid closure accepted");
    auto fixture = analyze("fn make(value:int)->fn(int)->int:\n  fn(item:int):item+value+value\nfn "
                           "main()->int:\n  f=make(20)\n  f(2)\n");
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
