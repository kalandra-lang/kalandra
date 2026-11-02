#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("locals.k", source)));
}
int main() {
  try {
    for (auto source :
         {"fn main()->int:\n  a=10\n  a=a*2\n  a\n", "fn main()->Bool:\n  a=true\n  a=false\n  a\n",
          "fn step(a:int)->int:\n  a=a+1\n  a\nfn main()->int:\n  step(2)\n",
          "fn main()->int:\n  a=0\n  while a<20: a=a+1\n  a\n",
          "fn main()->int:\n  a=0\n  if true:\n    a=4\n  a\n",
          "fn main()->int:\n  a=0\n  if true:\n    a=4\n  else:\n    a=5\n  a\n",
          "fn main()->int:\n  a=0\n  while a<5:\n    if a<3:\n      a=a+1\n    else:\n      "
          "a=a+2\n  a\n",
          "fn main()->int:\n  a=10\n  f=fn(x:int):a+x\n  a=20\n  f(1)+a\n",
          "fn main()->int:\n  a=1\n  a=0\n  if a==0: 7\n  else: 1/a\n",
          "fn id(x:int)->int:\n  x\nfn main()->int ! TaskError:\n  a=10\n  try parallel:\n    "
          "pending=spawn id(a)\n    a=20\n    prior=try await pending\n    a+prior\n",
          "fn id(x:int)->int:\n  x\nfn main()->int ! TaskError:\n  a=0\n  while a<10:\n    try "
          "parallel:\n      pending=spawn id(a)\n      a=try await pending+1\n      0\n  a\n",
          "fn main()->int:\n  values=generate(8,fn(i):\n    a=0\n    while a<i:\n      a=a+1\n    "
          "a\n  )\n  values[7]\n"}) {
      auto result = analyze(source);
      std::string errors;
      for (const auto &d : result.diagnostics)
        errors += d.message + "\n";
      require(result.ok(), std::string(source) + errors);
      require(bool(continuation::plan(*result.module)), "mutable continuation planning failed");
#ifndef KALANDRA_REBIND_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), "mutable native lowering failed: " + (lowered ? "" : lowered.error()));
#endif
    }
    for (auto source :
         {"fn main()->int:\n  a=1\n  a=false\n  a\n", "fn main()->int:\n  a=1\n  a:float=2\n  a\n",
          "fn main()->[int]:\n  a=[1]\n  a=[2]\n  a\n",
          "fn main()->int:\n  a=0\n  while 1:\n    a=a+1\n  a\n",
          "fn main()->int:\n  a=0\n  while a<4:\n    local=1\n    a=a+1\n  local\n",
          "fn main():\n  a=0\n  while a<4: a=a+1\n",
          "fn main()->int:\n  a=0\n  value=while a<4: a=a+1\n  value\n",
          "fn main()->int:\n  a=0\n  value=if true:\n    a=1\n  else:\n    a=2\n  0\n",
          "fn id(x:int)->int:\n  x\nfn main()->int ! TaskError:\n  try parallel:\n    "
          "pending=spawn id(1)\n    while false:\n      try await pending\n    0\n",
          "fn id(x:int)->int:\n  x\nfn main()->int ! TaskError:\n  try parallel:\n    i=0\n    "
          "while i<4:\n      pending=spawn id(i)\n      i=i+1\n    0\n"})
      require(!analyze(source).ok(),
              std::string("unsafe/invalid local control admitted: ") + source);
    auto failure = analyze("error Stop:\n  Done\nfn work()->int ! Stop:\n  a=0\n  while a<5:\n    "
                           "if a==3:\n      fail Stop.Done\n    else:\n      a=a+1\n  a\nfn "
                           "main()->int:\n  work() catch fn(error):42\n");
    std::string failureDetails;
    for (const auto &d : failure.diagnostics)
      failureDetails += d.message + "\n";
    require(failure.ok(),
            "explicit failure in statement branches lost its return context: " + failureDetails);
    require(bool(continuation::plan(*failure.module)), "loop failure planning failed");
    auto siblings = analyze("fn choose(flag:Bool)->int:\n  if flag:\n    local=1\n    local\n  "
                            "else:\n    local=2\n    local\nfn main()->int:\n  choose(true)\n");
    require(siblings.ok(), "sibling-local declarations rejected");
    for (const auto &symbol : siblings.module->symbols)
      require(!symbol.reassignable,
              "disjoint declarations unnecessarily changed immutable lowering");
    auto typed = analyze("fn main()->int:\n  a=0\n  while a<4: a=a+1\n  a\n");
    require(typed.ok(), "mutable metadata fixture rejected");
    require(numericCapabilityError(*typed.module, NumericTarget::AppleGpu).has_value(),
            "CPU-only local mutation admitted to a GPU kernel");
    require(typed.module->symbols[0].reassignable, "write scan did not identify scalar state");
    require(dumpTypedIR(*typed.module).find("while.statement") != std::string::npos,
            "loop not recorded in IR");
#ifndef KALANDRA_REBIND_FRONTEND_ONLY
    auto forged = *typed.module;
    forged.symbols[0].type.tensor = forged.symbols[0].type.array = true;
    forged.symbols[0].type.shape = {{{1}, 0}};
    require(!lowerNative(forged), "owned storage admitted to scalar cells");
#endif
    std::cout << "Local scalars: rebinding, statement branches, loops, snapshots, task obligations "
                 "and rejection gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
