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
CheckResult analyze(std::string text) {
  return check(parse(std::make_shared<SourceFile>("function-values.k", std::move(text))));
}
const std::string functions =
    "fn double(value:i64)->i64:\n  return value*2\n"
    "fn triple(value:i64)->i64:\n  return value*3\n"
    "fn apply(value:i64,transform:fn(i64)->i64)->i64:\n  return transform(value)\n"
    "fn choose(flag:bool)->fn(i64)->i64:\n  return if flag:double\n  else:triple\n";
void positive() {
  for (const auto *body :
       {"fn coordinate(value:i64)->i64:\n  return run(value) catch fn(error):return 0\n"
        "fn run(value:i64)->i64 ! TaskError:\n  return try parallel:\n    try await spawn "
        "double(value)\n"
        "fn main()->i64:\n  f=coordinate\n  return f(1)\n",
        "fn main()->i64:\n  f=double\n  alias:fn(i64)->i64=f\n  return alias(21)\n",
        "fn main()->i64 ! TaskError:\n  f=double\n  return try parallel:\n    result=try await "
        "spawn "
        "map([1],f)\n    result[0]\n",
        "fn main()->i64:\n  return apply(14,choose(false))\n",
        "fn main()->i64:\n  return choose(true)(21)\n",
        "fn main()->i64:\n  f=choose(false)\n  return 14->f\n",
        "fn main()->i64:\n  f=choose(true)\n  values=map([1,2,3],f)\n  return values[2]\n",
        "fn main()->i64:\n  f=choose(true)\n  values=map([1,2,3],fn(value):return f(value))\n  "
        "return values[2]\n",
        "fn keep(f:fn(i64)->i64)->fn(i64)->i64:\n  return f\n"
        "fn main()->i64:\n  return keep(choose(false))(14)\n",
        "fn factory()->fn(bool)->fn(i64)->i64:\n  return choose\n"
        "fn main()->i64:\n  return factory()(false)(14)\n",
        "fn worker(value:i64)->i64:\n  f=choose(false)\n  return f(value)\n"
        "fn main()->i64 ! TaskError:\n  return try parallel:\n    try await spawn worker(14)\n",
        "fn arrays(values:[i64])->[i64]:\n  return values\nfn main()->i64:\n  "
        "f:fn([i64])->[i64]=arrays\n "
        " return f([20,22])[0]\n",
        "fn matrix(values:Tensor[f32,?,?])->Tensor[f32,?,?]:\n  return values\nfn main()->i64:\n  "
        "f=matrix\n  result=f(tensor([[1,2]]))\n  return 0\n",
        "fn texts(values:[String])->[String]:\n  return values\nfn main()->i64:\n  f=texts\n  "
        "return "
        "length(f([\"é\"])[0])\n",
        "fn flags(values:[bool])->[bool]:\n  return values\nfn main()->i64:\n  f=flags\n  return "
        "if "
        "f([true])[0]:42\n  else:0\n",
        "type Row:\n  value:i64\nfn rows(values:[Row])->[Row]:\n  return values\nfn main()->i64:\n "
        " f=rows\n  return f([Row {value:42}])[0].value\n",
        "fn arrays(values:[i64])->[i64]:\n  return values\nfn worker(values:[i64])->[i64]:\n  "
        "f=arrays\n  return "
        "f(values)\nfn main()->i64 ! TaskError:\n  return try parallel:\n    work=spawn "
        "worker([42])\n    "
        "result=try await work\n    result[0]\n",
        "fn pair(value:i64):\n  return [value,value]\nfn three(value:i64):\n  return "
        "[value,value,value]\nfn select(flag:bool)->fn(i64)->[i64]:\n  return if flag:pair\n  "
        "else:three\nfn main()->i64:\n  f:fn(i64)->[i64]=select(false)\n  result=f(14)\n  return "
        "result[0]+result[1]+result[2]\n",
        "fn keep(values:Tensor[i64])->Tensor[i64]:\n  return values\nfn scalar(value:i64):\n  "
        "return "
        "reshape(tensor([value]),[])\nfn main()->i64:\n  f=keep\n  g=scalar\n  return f(g(42))[]\n",
        "fn answer()->i64:\n  return 42\nfn main()->i64:\n  f=answer\n  return f()\n"}) {
    auto checked = analyze(functions + body);
    if (!checked.ok())
      for (const auto &diagnostic : checked.diagnostics)
        std::cerr << renderDiagnostic(*checked.source, diagnostic);
    require(checked.ok(), "valid function value rejected");
    require(dumpTypedIR(*checked.module).find("function.") != std::string::npos,
            "function values disappeared from typed IR");
    require(bool(continuation::plan(*checked.module)), "function continuation planning failed");
#ifndef KALANDRA_FUNCTION_FRONTEND_ONLY
    auto lowered = lowerNative(*checked.module);
    require(bool(lowered), lowered ? "" : lowered.error().c_str());
    require(lowered->llvm.find(".unchecked") == std::string::npos,
            "function pointer retained an unchecked entry");
#endif
  }
}
void mutationContracts() {
  const std::string declarations = "fn inspect(mut values:[i64])->i64:\n  return length(values)\n"
                                   "fn read(values:[i64])->i64:\n  return length(values)\n";
  for (const auto *body : {
           "fn main()->i64:\n  f:fn(mut [i64])->i64=inspect\n  return f([42])\n",
           "fn main()->i64:\n  f:fn(mut [i64])->i64=fn(mut values):return length(values)\n  return "
           "f([42])\n",
           "fn factory()->fn(mut [i64])->i64:\n  return inspect\nfn main()->i64:\n  return "
           "factory()([42])\n",
           "type Editor:\n  edit:fn(mut [i64])->i64\nfn main()->i64:\n  editor=Editor "
           "{edit:inspect}\n  return editor.edit([42])\n",
           "fn main()->i64:\n  callbacks:[fn(mut [i64])->i64]=[inspect]\n  return "
           "callbacks[0]([42])\n",
           "fn capture(mut values:[i64])->fn()->i64:\n  return fn():return length(values)\n"
           "fn main()->i64:\n  return capture([42])()\n",
           "fn flags(mut values:[bool])->i64:\n  return length(values)\n"
           "fn main()->i64:\n  return flags([true])\n",
       }) {
    auto checked = analyze(declarations + body);
    require(checked.ok(), "valid mutating array contract rejected");
    require(!functionValueCapabilityError(*checked.module, NumericTarget::Cpu),
            "valid mutating function metadata rejected");
    for (const auto &function : checked.module->functions)
      for (std::size_t i = function.parameters.size() - function.captureCount;
           i < function.parameters.size(); ++i)
        require(!checked.module->symbols[function.parameters[i]].mutableAccess,
                "capture inherited caller write permission");
  }
  for (const auto *body : {
           "fn main()->i64:\n  f:fn([i64])->i64=inspect\n  return 0\n",
           "fn main()->i64:\n  f:fn(mut [i64])->i64=read\n  return 0\n",
           "fn main()->i64:\n  callbacks=[inspect,read]\n  return 0\n",
           "fn main()->i64:\n  f:fn(mut [i64])->i64=fn(values):return length(values)\n  return 0\n",
           "fn bad(mut value:i64)->i64:\n  return value\nfn main()->i64:\n  return 0\n",
           "fn bad(mut value:[String])->i64:\n  return length(value)\nfn main()->i64:\n  return "
           "0\n",
           "fn bad(mut value:Tensor[i64,2])->i64:\n  return 0\nfn main()->i64:\n  return 0\n",
           "fn main()->i64:\n  f:fn(mut i64)->i64=read\n  return 0\n",
           "fn main()->i64:\n  f:fn(mut [String])->i64=read\n  return 0\n",
       })
    require(!analyze(declarations + body).ok(),
            "mut contract erased or unsupported storage admitted");

  auto checked = analyze(declarations + "fn main()->i64:\n  f=inspect\n  return 0\n");
  require(checked.ok(), "mut signature forgery fixture rejected");
  bool forged = false;
  for (auto &expression : checked.module->expressions)
    if (std::holds_alternative<FunctionReference>(expression.operation) &&
        expression.type.callable && expression.type.callable->mutates(0)) {
      auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
      signature->mutableParameters.clear();
      expression.type.callable = std::move(signature);
      forged = true;
      break;
    }
  require(forged && functionValueCapabilityError(*checked.module, NumericTarget::Cpu).has_value(),
          "forged reference erased target mutation contract");
}
void negative() {
  for (const auto *body :
       {"fn main()->i64:\n  f:fn(f64)->f64=double\n  return 0\n",
        "fn main()->i64:\n  f=double\n  return f()\n",
        "fn main()->i64:\n  f=double\n  return f(\"bad\")\n",
        "fn main()->i64:\n  f=double\n  return f(1,2)\n",
        "fn bad(value:f64)->f64:\n  return value\n"
        "fn main()->i64:\n  f=if true:double\n  else:bad\n  return 0\n",
        "fn main()->i64:\n  f=fn(value):return value+1\n  return f(1)\n",
        "type Holder:\n  callback:fn(i64)->i64\nfn main()->i64:\n  holder=Holder {callback:0}\n  "
        "return 0\n",
        "fn main()->i64:\n  values=[double,fn(value:f64):return value]\n  return 0\n",

        "fn arrays(values:[i64])->[i64]:\n  return values\nfn main()->i64:\n  "
        "f:fn(Tensor[i64,?])->[i64]=arrays\n  return 0\n",
        "fn matrix(values:Tensor[i64,2])->Tensor[i64,2]:\n  return values\nfn main()->i64:\n  "
        "f:fn(Tensor[i64,?])->Tensor[i64,2]=matrix\n  return 0\n",
        "fn matrix(values:Tensor[i64,2])->Tensor[i64,2]:\n  return values\nfn main()->i64:\n  "
        "f=matrix\n  "
        "result=f(tensor([1,2,3]))\n  return 0\n",
        "fn matrix(values:Tensor[i64,?,?])->Tensor[i64,?,?]:\n  return values\nfn main()->i64:\n  "
        "f=matrix\n  result=f(tensor([1,2]))\n  return 0\n",
        "fn texts(values:[String])->[String]:\n  return values\nfn main()->i64:\n  f=texts\n  "
        "result=f([1])\n  return 0\n",
        "type A:\n  value:i64\ntype B:\n  value:i64\nfn arrays(values:[A])->[A]:\n  return "
        "values\nfn "
        "main()->i64:\n  f:fn([B])->[B]=arrays\n  return 0\n",
        "type A:\n  value:i64\ntype B:\n  value:i64\n"
        "fn read(value:A)->i64:\n  return value.value\n"
        "fn main()->i64:\n  f:fn(B)->i64=read\n  return 0\n"}) {
    auto checked = analyze(functions + body);
    require(!checked.ok(), "invalid signature, capture or execution boundary accepted");
  }
}
void forged() {
  auto checked = analyze(functions + "fn main()->i64:\n  f=double\n  return f(21)\n");
  require(checked.ok(), "forged IR fixture rejected");
  for (unsigned mode = 0; mode < 10; ++mode) {
    auto module = *checked.module;
    bool changed = false;
    for (ValueId id = 0; id < module.expressions.size(); ++id) {
      auto &expression = module.expressions[id];
      if (auto *ref = std::get_if<FunctionReference>(&expression.operation); ref && mode < 5) {
        if (mode == 0)
          ref->function = SIZE_MAX;
        if (mode == 1)
          expression.type.callable.reset();
        if (mode == 2) {
          auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
          signature->parameters[0].element = NumericType::Float64;
          expression.type.callable = signature;
        }
        if (mode == 3)
          expression.type.task = true;
        if (mode == 4)
          expression.type.builtin = BuiltinValue::Numeric;
        changed = true;
        break;
      }
      if (auto *call = std::get_if<FunctionInvoke>(&expression.operation); call && mode >= 5) {
        if (mode == 5)
          call->callee = id;
        if (mode == 6)
          call->callee = SIZE_MAX;
        if (mode == 7)
          call->arguments[0] = SIZE_MAX;
        if (mode == 8)
          call->arguments.clear();
        if (mode == 9)
          expression.type.element = NumericType::Float64;
        changed = true;
        break;
      }
    }
    require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
            "forged callable IR accepted");
  }
}
void collectionShapes() {
  auto checked = analyze(
      "fn varying(value:i64)->Tensor[i64,?]:\n  return if value==0:tensor([1])\n"
      "  else:tensor([2,3])\nfn main()->i64:\n  f=varying\n  a=f(0)\n  b=f(1)\n  return 0\n");
  require(checked.ok(), "dynamic callable result fixture rejected");
  std::vector<ExtentId> extents;
  for (const auto &expression : checked.module->expressions)
    if (std::holds_alternative<FunctionInvoke>(expression.operation)) {
      require(expression.type.shape.size() == 1 && !expression.type.shape[0].constant,
              "indirect result invented a fixed extent");
      extents.push_back(expression.type.shape[0].identity);
    }
  require(extents.size() == 2 && extents[0] && extents[1] && extents[0] != extents[1],
          "indirect results falsely share a dynamic shape identity");
  auto arrays = analyze("fn copy(values:[i64])->[i64]:\n  return values\n"
                        "fn main()->i64:\n  f=copy\n  result=f([20,22])\n  return result[0]\n");
  require(arrays.ok(), "collection IR fixture rejected");
  for (unsigned mode = 0; mode < 7; ++mode) {
    auto module = *arrays.module;
    bool changed = false;
    for (auto &expression : module.expressions) {
      if (auto *call = std::get_if<FunctionInvoke>(&expression.operation); call && mode < 4) {
        if (mode == 0)
          expression.type.array = false;
        if (mode == 1)
          expression.type.element = NumericType::UInt64;
        if (mode == 2)
          expression.type.shape.push_back({1, 0});
        if (mode == 3)
          module.expressions[call->arguments[0]].type.array = false;
        changed = true;
        break;
      }
      if (std::holds_alternative<FunctionReference>(expression.operation) && mode >= 4) {
        auto signature = std::make_shared<FunctionSignature>(*expression.type.callable);
        if (mode == 4)
          signature->parameters[0].shape[0].constant = 3;
        if (mode == 5)
          signature->result.tensor = false;
        if (mode == 6)
          signature->parameters[0].shape.resize(129);
        expression.type.callable = signature;
        changed = true;
        break;
      }
    }
    require(changed && numericCapabilityError(module, NumericTarget::Cpu).has_value(),
            "forged collection signature accepted");
  }
}

} // namespace
int main() {
  try {
    positive();
    mutationContracts();
    negative();
    forged();
    collectionShapes();
    std::cout << "Function-value signatures, calls, boundaries and IR checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
