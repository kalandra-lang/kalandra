#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("error-narrowing.k", text)));
}
int main() {
  try {
    const std::string types = "error A:\n  Bad:\n    text:String\n  Missing\n"
                              "error B:\n  Other\nerror C:\n  Last\n"
                              "type Both=A | B\ntype All=Both | C\n"
                              "type Mixed=Both | TaskError\n";
    const std::string work = "fn work()->int ! Both:\n  fail B.Other\n";
    for (const auto *body :
         {"fn only()->int ! A:\n  work() catch fn(error):\n    match error:\n"
          "      A.Bad:fail error\n      A.Missing:fail error\n      B.Other:0\n",
          "fn only()->int ! B:\n  work() catch fn(error):\n    match error:\n"
          "      A.Bad:0\n      A.Missing:1\n      _:fail error\n",
          "fn forward(error:A)->int ! A:\n  fail error\n"
          "fn only()->int ! A:\n  work() catch fn(error):\n    match error:\n"
          "      A.Bad:forward(error)\n      _:0\n",
          "fn forward(error:Mixed)->int ! A:\n  match error:\n    A.Bad:fail error\n    _:0\n",
          "fn forward(error:Mixed)->int ! TaskError:\n  match error:\n    A.Bad:0\n"
          "    A.Missing:1\n    B.Other:2\n    _:fail error\n",
          "fn forward(error:Mixed)->int ! TaskError:\n  match error:\n"
          "    TaskError.Fault:fail error\n    _:0\n",
          "fn forward(error:All)->int ! (B | C):\n  match error:\n"
          "    A.Bad:0\n    A.Missing:1\n    _:fail error\n",
          "fn forward(error:All)->int ! B:\n  match error:\n"
          "    A.Bad:0\n    A.Missing:1\n    _:\n      match error:\n        C.Last:2\n"
          "        _:fail error\n",
          "fn consume(value:A)->int:\n  1\nfn score(value:Both)->int:\n  match value:\n"
          "    A.Bad:consume(value)\n    _:0\n",
          // A pattern binding with the subject's name follows ordinary shadowing.
          "fn forward(error:Both)->int:\n  match error:\n"
          "    A.Bad {text:error}:length(error)\n    _:0\n"}) {
      auto checked = analyze(types + work + body + "fn main()->int:\n  0\n");
      if (!checked.ok())
        for (const auto &d : checked.diagnostics)
          std::cerr << renderDiagnostic(*checked.source, d);
      require(checked.ok(), "valid branch narrowing rejected");
      require(bool(continuation::plan(*checked.module)), "narrowing plan failed");
#ifndef KALANDRA_ERROR_NARROW_FRONTEND_ONLY
      auto lowered = lowerNative(*checked.module);
      require(bool(lowered), lowered ? "" : lowered.error().c_str());
#endif
    }
    for (const auto *body :
         {// One A variant remains, so the wildcard still includes A and B.
          "fn only()->int ! B:\n  work() catch fn(error):\n    match error:\n"
          "      A.Bad:0\n      _:fail error\n",
          "fn only()->int ! A:\n  work() catch fn(error):\n    match error:\n"
          "      A.Bad:0\n      B.Other:fail error\n      _:0\n",
          // Snapshots and the binding outside the branch remain the original union.
          "fn only()->int ! A:\n  work() catch fn(error):\n    copy=error\n    match error:\n"
          "      A.Bad:fail copy\n      _:0\n",
          "fn only(error:Both)->int ! A:\n  ignored=match error:\n    A.Bad:0\n    _:1\n"
          "  fail error\n",
          "fn forward(error:A)->int:\n  1\nfn only(error:Both)->int:\n  match error:\n"
          "    A.Bad:1\n    _:forward(error)\n",
          "fn only(error:Mixed)->int ! A:\n  match error:\n"
          "    TaskError.Fault:fail error\n    _:0\n",
          // Returning an incoming expected result on one path cannot discharge it elsewhere.
          "fn only()->int ! Both:\n  pending=work()\n  work() catch fn(error):\n"
          "    match error:\n      A.Bad:pending\n      _:0\n"})
      require(!analyze(types + work + body + "fn main()->int:\n  0\n").ok(),
              "narrowing escaped its proven branch");
    auto checked = analyze(types + work +
                           "fn only()->int ! A:\n  work() catch fn(error):\n"
                           "    match error:\n      A.Bad:fail error\n      _:0\n"
                           "fn main()->int:\n  0\n");
    require(checked.ok(), "narrowing metadata fixture rejected");
    for (unsigned mode = 0; mode < 8; ++mode) {
      auto forged = *checked.module;
      bool mutated = false;
      for (std::size_t id = 0; id < forged.expressions.size(); ++id)
        if (auto *narrow = std::get_if<ErrorNarrow>(&forged.expressions[id].operation)) {
          auto &result = forged.expressions[id].type;
          if (mode == 0)
            narrow->input = SIZE_MAX;
          if (mode == 1)
            narrow->input = id;
          if (mode == 2)
            result = forged.expressions[narrow->input].type;
          if (mode == 3) {
            result.builtin = BuiltinValue::Numeric;
            result.record.reset();
          }
          if (mode == 4)
            result.record = SIZE_MAX;
          if (mode == 5)
            result.task = true;
          if (mode == 6)
            result.fallible = true;
          if (mode == 7)
            result.record = 2; // C is outside Both.
          mutated = true;
          break;
        }
      require(mutated, "fixture has no checked projection");
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)) &&
                  !continuation::plan(forged),
              "malformed narrowing reached lowering");
    }
    std::cout << "Error narrowing: selected families, residual wildcards, lexical scope and "
                 "checked projection metadata passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
