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
    const std::string functions = "fn first()->i64 ! A:\n  fail A.Missing\n"
                                  "fn second()->i64 ! B:\n  fail B.Bad {value:2}\n";
    for (const auto *body :
         {"fn main()->i64 ! Both:\n  return try first()\n",
          "enum Choice:\n  First\n  Second\nfn main()->i64 ! Both:\n  pending=first()\n"
          "  return match Choice.First:\n    Choice.First:pending\n    Choice.Second:pending\n",
          "fn main()->i64 ! Both:\n  if true:fail A.Missing\n  return try second()\n",
          "fn main()->i64 ! (B | A | A):\n  return first()\n",
          "fn main()->i64 ! Both:\n  return if true:first()\n  else:second()\n",
          "fn main()->i64 ! All:\n  if true:\n    fail C.Last\n  return try first()\n",
          "fn joined()->i64 ! Both:\n  return try first()\nfn main()->i64 ! Reordered:\n  return joined()\n",
          "fn joined()->i64 ! Both:\n  return if true:first()\n  else:second()\n"
          "fn main()->i64:\n  return joined() catch fn(error):\n    return match error:\n"
          "      A.Missing:1\n      A.Bad {text,values}:length(text)+values[0]\n"
          "      B.Bad {value}:value\n",
          "fn main()->i64 ! Both:\n  return first() catch fn(error):\n"
          "    if true:\n      fail B.Bad {value:2}\n    fail A.Missing\n",
          "fn main()->i64 ! Both:\n  return first() catch fn(error):\n"
          "    x=try first()\n    y=try second()\n    return x+y\n",
          "fn joined()->i64 ! Both:\n  return first()\nfn main()->i64 ! All:\n"
          "  return joined() catch fn(error):fail error\n"}) {
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
         {"type Loop=Loop\nfn main()->i64:\n  return 0\n",
          "enum Choice:\n  First\n  Second\nfn main()->i64 ! Both:\n  pending=first()\n"
          "  return match Choice.First:\n    Choice.First:0\n    Choice.Second:pending\n",
          "type One=Two\ntype Two=One\nfn main()->i64:\n  return 0\n",
          "type Wrong=i64\nfn main()->i64:\n  return 0\n",
          "enum Ordinary:\n  Bad\ntype Wrong=A | Ordinary\nfn main()->i64:\n  return 0\n",
          "type Wrong=A | Missing\nfn main()->i64:\n  return 0\n",
          "type Both=A | B\nfn main()->i64:\n  return 0\n", "type A=B\nfn main()->i64:\n  return 0\n",
          "fn Both()->i64:\n  return 0\nfn main()->i64:\n  return 0\n", "fn main()->i64 ! A:\n  return try second()\n",
          "fn joined()->i64 ! Both:\n  return first()\nfn main()->i64 ! A:\n  return joined()\n",
          "fn joined()->i64 ! Both:\n  return first()\nfn main()->i64 ! A:\n  return try joined()\n",
          "fn joined()->i64 ! Both:\n  return first()\nfn main()->i64:\n  return joined() catch fn(error):\n"
          "    return match error:\n      A.Missing:0\n      A.Bad:1\n",
          "fn joined()->i64 ! Both:\n  return first()\nfn main()->i64:\n  return joined() catch fn(error):\n"
          "    return match error:\n      A.Missing:0\n      A.Missing:1\n      _:2\n",
          "fn joined()->i64 ! Both:\n  return first()\nfn main()->i64:\n  return joined() catch fn(error):\n"
          "    return match error:\n      Both.Missing:0\n      _:1\n",
          "fn child()->i64 ! Both:\n  return first()\nfn main()->i64 ! TaskError:\n"
          "  return try parallel:\n    try await spawn child()\n"})
      require(!analyze(types + functions + body).ok(), "invalid union admitted");
    auto result = analyze(types + functions + "fn main()->i64 ! Both:\n  return first()\n");
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
