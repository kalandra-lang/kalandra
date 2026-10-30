#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static FunctionId function(const TypedModule &module, const std::string &name) {
  for (FunctionId id = 0; id < module.functions.size(); ++id)
    if (module.functions[id].name == name)
      return id;
  throw std::runtime_error("missing test function: " + name);
}
static TypedModule checked(const std::string &source) {
  auto result = check(parse(std::make_shared<SourceFile>("execution-flow.k", source)));
  if (!result.ok()) {
    for (const auto &error : result.diagnostics)
      std::cerr << renderDiagnostic(*result.source, error);
    throw std::runtime_error("execution-flow fixture rejected");
  }
  return std::move(*result.module);
}
static const std::string foundation = R"k(fn double(value:int)->int:
  value*2
fn other(value:int)->int:
  value+1
fn floating(value:float)->float:
  value
fn floatFactory()->fn(float)->float:
  floating
fn run(value:int)->int:
  execute(value) catch fn(error):0
fn execute(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
)k";
int main() {
  try {
    unsigned cases = 0;
    for (const auto &program :
         {"fn main()->int:\n  f=double\n  g=f\n  g(21)\n",
          "fn apply(f:fn(int)->int,value:int)->int:\n  f(value)\n"
          "fn main()->int:\n  apply(double,21)\n",
          "fn factory()->fn(int)->int:\n  double\n"
          "fn main()->int:\n  f=factory()\n  f(21)\n",
          "fn select(flag:Bool)->fn(int)->int:\n  if flag:double\n  else:other\n"
          "fn main()->int:\n  f=select(true)\n  f(21)\n",
          "type Box:\n  f:fn(int)->int\n"
          "fn main()->int:\n  box=Box {f:double}\n  box.f(21)\n",
          "fn main()->int:\n  callbacks=[double,other]\n  callbacks[0](21)\n",
          "fn factory(f:fn(int)->int)->fn(int)->int:\n  fn(value:int):f(value)\n"
          "fn main()->int:\n  f=factory(double)\n  f(21)\n",
          "fn apply(f:fn(int)->int,value:int)->int:\n  if value==0:f(21)\n"
          "  else:apply(f,value-1)\n"
          "fn main()->int:\n  apply(double,2)\n",
          "fn main()->int:\n  callbacks=[double,other]\n"
          "  values=slice(callbacks,0,1)\n  values[0](21)\n",
          "fn factory()->[fn(int)->int]:\n  [double,other]\n"
          "fn main()->int:\n  callbacks=factory()\n  callbacks[0](21)\n"}) {
      auto module = checked(foundation + program);
      auto baseline = functionInvocationFlow(module);
      require(bool(baseline), "valid invocation flow exhausted budget");
      auto effects = continuation::coordinatingFunctions(module);
      const auto root = function(module, "main");
      const auto target = function(module, "double");
      const auto coordinator = function(module, "run");
      require(!effects[root] && effects[coordinator], "synchronous invocation misclassified");
      require(!functionInvocationFlow(module, 0) && !functionInvocationFlow(module, 1),
              "flow budget exhaustion returned a partial proof");
      // Substitute a same-signature coordinating target into otherwise checked
      // IR. Execution stays gated, but analysis must follow the complete graph.
      unsigned replaced = 0;
      for (auto &value : module.expressions)
        if (auto *reference = std::get_if<FunctionReference>(&value.operation);
            reference && reference->function == target) {
          reference->function = coordinator;
          ++replaced;
        }
      require(replaced != 0, "fixture contained no target reference");
      auto flow = functionInvocationFlow(module);
      require(bool(flow), "coordinating invocation flow failed");
      bool found = false;
      for (ValueId id = 0; id < module.expressions.size(); ++id)
        if (std::holds_alternative<FunctionInvoke>(module.expressions[id].operation)) {
          require(!flow->unknown[id], "known function flow lost its provenance");
          require(std::is_sorted(flow->targets[id].begin(), flow->targets[id].end()),
                  "invocation target order is unstable");
          for (auto fn : flow->targets[id]) {
            require(fn != function(module, "floating"), "incompatible callable target leaked");
            found |= fn == coordinator;
          }
        }
      require(found, "coordinating target disappeared through indirect flow");
      effects = continuation::coordinatingFunctions(module);
      require(effects[root] && continuation::resumableFunctions(module)[root],
              "indirect coordination did not reach callers");
      for (const auto *name : {"factory", "select"})
        for (FunctionId id = 0; id < module.functions.size(); ++id)
          if (module.functions[id].name == name)
            require(!effects[id], "creating or returning a callable executed its body");
      require(!functionValueCapabilityError(module, NumericTarget::Cpu) &&
                  continuation::plan(module).has_value(),
              "proven resumable callable graph did not lower");
      ++cases;
    }
    auto handled = checked(R"k(error E:
  Bad:
    value:int
fn main()->int:
  offset:int=21
  failer=fn(value:int)->int ! E:fail E.Bad {value:offset}
  callbacks:[fn(int)->int ! E]=[failer,failer]
  evaluate=fn(callback:fn(int)->int ! E)->int:
    callback(0) catch fn(error):
      match error:
        E.Bad {value}:value
  values=map(callbacks,evaluate)
  values[0]+values[1]
)k");
    require(bool(continuation::plan(handled)),
            "handled indirect errors escaped the synchronous calling convention");
    for (auto effect : continuation::coordinatingFunctions(handled))
      require(!effect, "expected-error handling became task coordination");
    auto unknown = checked(foundation + "fn unused(f:fn(int)->int)->int:\n  f(21)\n"
                                        "fn main()->int:\n  f=double\n  f(21)\n");
    const auto flow = functionInvocationFlow(unknown);
    require(bool(flow), "unknown parameter flow failed");
    bool seen = false;
    for (ValueId id = 0; id < unknown.expressions.size(); ++id)
      if (std::holds_alternative<FunctionInvoke>(unknown.expressions[id].operation))
        if (flow->unknown[id]) {
          require(!flow->reachable[id], "uncalled generic callback became reachable");
          require(std::ranges::find(flow->targets[id], function(unknown, "double")) !=
                      flow->targets[id].end(),
                  "unknown source erased compatible known targets");
          require(std::ranges::find(flow->targets[id], function(unknown, "floating")) ==
                      flow->targets[id].end(),
                  "unknown source admitted an incompatible signature");
          seen = true;
        }
    require(seen, "unbound callback lost its explicit unknown-source marker");
    require(!continuation::coordinatingFunctions(unknown)[function(unknown, "main")],
            "unreachable unknown callback polluted unrelated functions");
    std::cout << "Execution flow: " << cases
              << " indirect-call graphs, unknown sources, budgets and dispatch gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
