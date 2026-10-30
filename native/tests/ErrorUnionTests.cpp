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
  return check(parse(std::make_shared<SourceFile>("error-unions.k", text)));
}
int main() {
  try {
    const std::string types = "error A:\n  Missing\n  Bad:\n    text:String\n    values:[int]\n"
                              "error B:\n  Bad:\n    value:int\nerror C:\n  Last\n"
                              "type Both=A | B | A\ntype Reordered=B | A\ntype All=C | Both\n";
    const std::string functions = "fn first()->int ! A:\n  fail A.Missing\n"
                                  "fn second()->int ! B:\n  fail B.Bad {value:2}\n";
    for (const auto *body :
         {"fn main()->int ! Both:\n  try first()\n",
          "enum Choice:\n  First\n  Second\nfn main()->int ! Both:\n  pending=first()\n"
          "  match Choice.First:\n    Choice.First:pending\n    Choice.Second:pending\n",
          "fn main()->int ! Both:\n  if true:fail A.Missing\n  try second()\n",
          "fn main()->int ! (B | A | A):\n  first()\n",
          "fn main()->int ! Both:\n  if true:first()\n  else:second()\n",
          "fn main()->int ! All:\n  if true:\n    fail C.Last\n  try first()\n",
          "fn joined()->int ! Both:\n  try first()\nfn main()->int ! Reordered:\n  joined()\n",
          "fn joined()->int ! Both:\n  if true:first()\n  else:second()\n"
          "fn main()->int:\n  joined() catch fn(error):\n    match error:\n"
          "      A.Missing:1\n      A.Bad {text,values}:length(text)+values[0]\n"
          "      B.Bad {value}:value\n",
          "fn main()->int ! Both:\n  first() catch fn(error):\n"
          "    if true:\n      fail B.Bad {value:2}\n    fail A.Missing\n",
          "fn main()->int ! Both:\n  first() catch fn(error):\n"
          "    x=try first()\n    y=try second()\n    x+y\n",
          "fn joined()->int ! Both:\n  first()\nfn main()->int ! All:\n"
          "  joined() catch fn(error):fail error\n"}) {
      auto result = analyze(types + functions + body);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid union rejected");
      require(bool(continuation::plan(*result.module)), "union continuation rejected");
#ifndef KALANDRA_ERROR_UNION_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error().c_str());
#endif
    }
    for (const auto *body :
         {"type Loop=Loop\nfn main()->int:\n  0\n",
          "enum Choice:\n  First\n  Second\nfn main()->int ! Both:\n  pending=first()\n"
          "  match Choice.First:\n    Choice.First:0\n    Choice.Second:pending\n",
          "type One=Two\ntype Two=One\nfn main()->int:\n  0\n",
          "type Wrong=int\nfn main()->int:\n  0\n",
          "enum Ordinary:\n  Bad\ntype Wrong=A | Ordinary\nfn main()->int:\n  0\n",
          "type Wrong=A | Missing\nfn main()->int:\n  0\n",
          "type Both=A | B\nfn main()->int:\n  0\n", "type A=B\nfn main()->int:\n  0\n",
          "fn Both()->int:\n  0\nfn main()->int:\n  0\n", "fn main()->int ! A:\n  try second()\n",
          "fn joined()->int ! Both:\n  first()\nfn main()->int ! A:\n  joined()\n",
          "fn joined()->int ! Both:\n  first()\nfn main()->int ! A:\n  try joined()\n",
          "fn joined()->int ! Both:\n  first()\nfn main()->int:\n  joined() catch fn(error):\n"
          "    match error:\n      A.Missing:0\n      A.Bad:1\n",
          "fn joined()->int ! Both:\n  first()\nfn main()->int:\n  joined() catch fn(error):\n"
          "    match error:\n      A.Missing:0\n      A.Missing:1\n      _:2\n",
          "fn joined()->int ! Both:\n  first()\nfn main()->int:\n  joined() catch fn(error):\n"
          "    match error:\n      Both.Missing:0\n      _:1\n",
          "fn child()->int ! Both:\n  first()\nfn main()->int ! TaskError:\n"
          "  try parallel:\n    try await spawn child()\n"})
      require(!analyze(types + functions + body).ok(), "invalid union admitted");
    auto result = analyze(types + functions + "fn main()->int ! Both:\n  first()\n");
    require(result.ok(), "metadata fixture rejected");
    const auto id = *result.module->functions.back().resultType.errorRecord;
    require(result.module->records[id].errorMembers == std::vector<std::size_t>{0, 1},
            "alias normalization did not collapse duplicates");
    for (unsigned mode = 0; mode < 6; ++mode) {
      auto forged = *result.module;
      auto &record = forged.records[id];
      if (mode == 0)
        record.errorMembers = {0, 0};
      if (mode == 1)
        record.errorMembers = {0, SIZE_MAX};
      if (mode == 2)
        record.errorOrigins[0] = {1, 0};
      if (mode == 3)
        record.errorOrigins.pop_back();
      if (mode == 4)
        record.fields[1].type.element = NumericType::UInt64;
      if (mode == 5)
        forged.expressions[forged.functions.back().result].type.errorRecord = SIZE_MAX;
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "malformed union metadata admitted");
    }
    std::cout
        << "Error unions: canonical aliases, subset propagation, exhaustive qualified matching, "
           "handler composition, invalid aliases and metadata checks passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
