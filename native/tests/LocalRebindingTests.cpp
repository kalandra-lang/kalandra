#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#ifndef KALANDRA_REBIND_FRONTEND_ONLY
#include "../lib/NativeStack.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/SourceMgr.h"
#endif
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &source) {
  if (source.find("yield ") != std::string::npos) {
    const SourceOverlay overlay{"locals.k", source};
    auto loaded = loadModules(overlay.path, {}, {}, {}, {&overlay, 1});
    require(bool(loaded), "cannot load generator binding fixture");
    return check(std::move(*loaded));
  }
  return check(parse(std::make_shared<SourceFile>("locals.k", source)));
}
int main() {
  try {
    for (auto source :
         {"fn main()->[i64]:\n  return array(for i in range(3):\n    for j in range(i):\n      if "
          "j%2==0:\n     "
          "   yield i+j\n  )\n",
          "fn main()->[f32]:\n  return array(for i in range(0):\n    yield 1\n  )\n",
          "fn main()->i64:\n  i=0\n  result=while i<4:\n    i=i+1\n    if i==2:\n      continue\n  "
          "  break i\n  else:\n    0\n  return result\n",
          "fn main()->i64:\n  total=0\n  for value in range(5,0,-1):\n    if value==3:\n      "
          "break\n    total=total+value\n  return total\n",
          "fn main()->String:\n  return for text in [\"a\",\"b\"]:\n    break text\n  else:\n    "
          "\"empty\"\n",
          "fn main()->i64:\n  a=10\n  a=a*2\n  return a\n",
          "fn main()->bool:\n  a=true\n  a=false\n  return a\n",
          "fn main()->[i64]:\n  a=[1]\n  a=[2]\n  return a\n",
          "fn main()->[bool]:\n  a=[true]\n  a=[false,true]\n  return a\n",
          "fn step(a:i64)->i64:\n  a=a+1\n  return a\nfn main()->i64:\n  return step(2)\n",
          "fn main()->i64:\n  a=0\n  while a<20: a=a+1\n  return a\n",
          "fn main()->i64:\n  a=0\n  if true:\n    a=4\n  return a\n",
          "fn main()->i64:\n  a=0\n  if true:\n    a=4\n  else:\n    a=5\n  return a\n",
          "fn main()->i64:\n  a=0\n  while a<5:\n    if a<3:\n      a=a+1\n    else:\n      "
          "a=a+2\n  return a\n",
          "fn main()->i64:\n  a=10\n  f=fn(x:i64):return a+x\n  a=20\n  return f(1)+a\n",
          "fn main()->i64:\n  a=1\n  a=0\n  return if a==0: 7\n  else: 1/a\n",
          "fn id(x:i64)->i64:\n  return x\nfn main()->i64 ! TaskError:\n  a=10\n  return try "
          "parallel:\n    "
          "pending=spawn id(a)\n    a=20\n    prior=try await pending\n    a+prior\n",
          "fn id(x:i64)->i64:\n  return x\nfn main()->i64 ! TaskError:\n  a=0\n  while a<10:\n    "
          "try "
          "parallel:\n      pending=spawn id(a)\n      a=try await pending+1\n      0\n  return "
          "a\n",
          "fn main()->i64:\n  values=generate(8,fn(i):\n    a=0\n    while a<i:\n      a=a+1\n    "
          "return a\n  )\n  return values[7]\n"}) {
      auto result = analyze(source);
      std::string errors;
      for (const auto &d : result.diagnostics)
        errors += d.message + "\n";
      require(result.ok(), std::string(source) + errors);
      require(bool(continuation::plan(*result.module, {.valueScopes = true})), "mutable continuation planning failed");
#ifndef KALANDRA_REBIND_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), "mutable native lowering failed: " + (lowered ? "" : lowered.error()));
#endif
    }
#ifndef KALANDRA_REBIND_FRONTEND_ONLY
    // A wide record parameter can reach a collector without an intervening
    // literal/index kernel. Audit the copy adapter's actual conservative frame
    // budget instead of relying on those unrelated kernels to reject it.
    std::string wide = "type Wide:\n";
    for (unsigned field = 0; field < 600; ++field)
      wide += "  field" + std::to_string(field) + ":i64\n";
    wide += "fn collect(value:Wide)->[Wide]:\n  return array(for index in range(1):\n    yield "
            "value\n  )\nfn "
            "main()->i64:\n  return 0\n";
    auto wideTyped = analyze(wide);
    require(wideTyped.ok(), "wide collector parameter rejected by the frontend");
    auto wideIr = lowerNative(*wideTyped.module);
    require(bool(wideIr),
            "wide collector lost native admission: " + (wideIr ? "" : wideIr.error()));
    llvm::LLVMContext wideContext;
    llvm::SMDiagnostic diagnostic;
    auto wideModule = llvm::parseAssemblyString(wideIr->llvm, diagnostic, wideContext);
    require(bool(wideModule), "cannot read wide collector LLVM");
    bool sawElement = false;
    for (const auto &function : *wideModule)
      if (function.getName().starts_with("collection.payload.copy.")) {
        require(!nativeStackBudgetError(function, wideModule->getDataLayout()),
                "collection copy adapter bypassed the native stack budget");
      }
    // Locate the actual 600-field item through its layout descriptor. Private
    // adapter names encode structural schemas and are not a stable ABI.
    for (const auto &global : wideModule->globals()) {
      if (!global.getName().starts_with("collection.payload.layout."))
        continue;
      auto *layout = llvm::dyn_cast_or_null<llvm::ConstantStruct>(global.getInitializer());
      if (!layout || layout->getNumOperands() != 5)
        continue;
      auto *bytes = llvm::dyn_cast<llvm::ConstantInt>(layout->getOperand(1));
      auto *copy = llvm::dyn_cast<llvm::Function>(layout->getOperand(3)->stripPointerCasts());
      if (bytes && bytes->getZExtValue() == 600 * sizeof(std::int64_t) && copy &&
          copy->getName().starts_with("collection.payload.copy.") && !copy->isDeclaration())
        sawElement = true;
    }
    require(sawElement, "wide collector element adapter was not emitted");
#endif
    for (auto source :
         {"fn main()->i64:\n  break\n  return 0\n",
          "fn main()->i64:\n  continue\n  return 0\n",
          "fn main()->i64:\n  for i in range(3):\n    continue i\n  0\n",
          "fn main()->i64:\n  return while true:\n    break true\n  else:\n    0\n",
          "fn main()->i64:\n  a=1\n  a=false\n  return a\n", "fn main()->i64:\n  a=1\n  a:f64=2\n  return a\n",
          "fn main()->[String]:\n  a=[\"one\"]\n  a=[\"two\"]\n  return a\n",
          "fn main()->[i64]:\n  a=[1]\n  a=[true]\n  return a\n",
          "fn main()->i64:\n  a=0\n  while 1:\n    a=a+1\n  return a\n",
          "fn main()->i64:\n  a=0\n  while a<4:\n    local=1\n    a=a+1\n  return local\n",
          "fn main():\n  a=0\n  return while a<4: a=a+1\n",
          "fn main()->i64:\n  a=0\n  value=while a<4: a=a+1\n  value\n",
          "fn main()->i64:\n  a=0\n  value=if true:\n    a=1\n  else:\n    a=2\n  0\n",
          "fn id(x:i64)->i64:\n  return x\nfn main()->i64 ! TaskError:\n  return try parallel:\n    "
          "pending=spawn id(1)\n    while false:\n      try await pending\n    0\n",
          "fn id(x:i64)->i64:\n  return x\nfn main()->i64 ! TaskError:\n  return try parallel:\n    i=0\n    "
          "while i<4:\n      pending=spawn id(i)\n      i=i+1\n    0\n"})
      require(!analyze(source).ok(),
              std::string("unsafe/invalid local control admitted: ") + source);
    auto failure = analyze("error Stop:\n  Done\nfn work()->i64 ! Stop:\n  a=0\n  while a<5:\n    "
                           "if a==3:\n      fail Stop.Done\n    else:\n      a=a+1\n  return a\nfn "
                           "main()->i64:\n  return work() catch fn(error):return 42\n");
    std::string failureDetails;
    for (const auto &d : failure.diagnostics)
      failureDetails += d.message + "\n";
    require(failure.ok(),
            "explicit failure in statement branches lost its return context: " + failureDetails);
    require(bool(continuation::plan(*failure.module)), "loop failure planning failed");
    auto siblings = analyze("fn choose(flag:bool)->i64:\n  return if flag:\n    local=1\n    local\n  "
                            "else:\n    local=2\n    local\nfn main()->i64:\n  return choose(true)\n");
    require(siblings.ok(), "sibling-local declarations rejected");
    for (const auto &symbol : siblings.module->symbols)
      require(!symbol.reassignable,
              "disjoint declarations unnecessarily changed immutable lowering");
    auto typed = analyze("fn main()->i64:\n  a=0\n  while a<4: a=a+1\n  return a\n");
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
