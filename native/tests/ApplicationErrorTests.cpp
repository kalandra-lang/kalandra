#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("application-errors.k", text)));
}
int main() {
  try {
    const std::string decl =
        "error InputError:\n  Empty\n  Bad:\n    count:int\n    reason:String\n"
        "enum Ordinary:\n  Empty\nerror OtherError:\n  Bad\n";
    auto inferred = analyze(decl + "fn checked(x:i64) ! InputError:\n  return x+1\n"
                                   "fn main()->i64 ! InputError:\n  return try checked(1)\n");
    require(inferred.ok() && inferred.module->functions[0].resultType.errorRecord == 0,
            "inferred success type lost its explicit application channel");
    for (const auto *body :
         {"fn main()->i64 ! InputError:\n  if true:\n    42\n  return 1\n",
          "fn checked(x:i64)->i64 ! InputError:\n  if x==0:\n    fail InputError.Empty\n  return x+1\n"
          "fn main()->i64:\n  return checked(0) catch fn(error):\n    return match error:\n      "
          "InputError.Empty:42\n      InputError.Bad {count}:count\n",
          "fn checked(x:i64)->i64 ! InputError:\n  return if x==0:fail InputError.Bad "
          "{count:x,reason:\"bad\"}\n  else:x\n"
          "fn relay(x:i64)->i64 ! InputError:\n  n=try checked(x)\n  return n+1\nfn main()->i64:\n  return "
          "relay(0) catch fn(error):return 0\n",
          "fn original()->i64 ! InputError:\n  fail InputError.Empty\n"
          "fn convert()->i64 ! OtherError:\n  return original() catch fn(error):fail OtherError.Bad\n"
          "fn main()->i64:\n  return convert() catch fn(error):return 42\n",
          "fn explain(error:InputError)->String:\n  return match error:\n    InputError.Empty:\"empty\"\n "
          "   InputError.Bad {reason}:reason\n"
          "fn main()->String:\n  return explain(InputError.Bad {count:1,reason:\"bad\"})\n",
          "fn local(x:i64)->i64 ! InputError:\n  if x==0:\n    reason=\"bad\"+\" input\"\n    fail "
          "InputError.Bad {count:x,reason:reason}\n  return x\n"
          "fn worker()->i64:\n  return local(0) catch fn(error):return 42\n"
          "fn main()->i64 ! TaskError:\n  return try parallel:\n    try await spawn worker()\n"}) {
      auto result = analyze(decl + body);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid application error rejected");
      require(result.module->records[0].error && !result.module->records[1].error,
              "error declaration identity lost");
      require(bool(continuation::plan(*result.module)), "application-error plan rejected");
#ifndef KALANDRA_APPLICATION_ERROR_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (const auto *body :
         {"fn main()->i64 ! Ordinary:\n  return 1\n",
          "fn main()->i64 ! InputError:\n  fail Ordinary.Empty\n",
          "fn main()->i64 ! InputError:\n  fail OtherError.Bad\n",
          "fn bad()->i64 ! InputError:\n  fail InputError.Empty\nfn main()->i64:\n  return try bad()\n",
          "fn bad()->i64 ! InputError:\n  fail InputError.Empty\nfn main()->i64 ! OtherError:\n  "
          "return try bad()\n",
          "fn bad()->i64 ! InputError:\n  fail InputError.Empty\nfn main()->i64:\n  "
          "ignored=bad()\n  return 42\n",
          "fn bad()->i64 ! InputError:\n  fail InputError.Empty\nfn main()->i64:\n  return bad() catch "
          "fn(error):\n    return match error:\n      InputError.Bad:0\n",
          "fn bad()->i64 ! InputError:\n  fail InputError.Empty\nfn main()->i64 ! TaskError:\n  return "
          "try parallel:\n    try await spawn bad()\n",
          "fn bad()->i64 ! InputError:\n  fail InputError.Empty\nfn main()->i64 ! TaskError:\n  return "
          "try parallel:\n    try bad()\n",
          "fn main()->i64 ! InputError:\n  result=if true:\n    fail InputError.Empty\n  return 1\n",
          "fn main()->i64 ! InputError:\n  return if true:\n    fail InputError.Empty\n",
          "fn main()->i64 ! InputError:\n  if 1:\n    fail InputError.Empty\n  return 1\n",
          "fn main()->i64 ! InputError | TaskError:\n  1\n"})
      require(!analyze(decl + body).ok(),
              "invalid/unsupported error program admitted: " + std::string(body));
    auto valid = analyze(decl + "fn get()->i64 ! InputError:\n  fail InputError.Empty\n"
                                "fn main()->i64:\n  return get() catch fn(error):return 42\n");
    require(valid.ok(), "metadata fixture rejected");
    for (unsigned mode = 0; mode < 4; ++mode) {
      auto forged = *valid.module;
      if (mode == 0)
        forged.records[0].error = false;
      if (mode == 1)
        forged.functions[0].resultType.errorRecord = 999;
      if (mode == 2)
        for (auto &e : forged.expressions)
          if (auto *h = std::get_if<CatchOperation>(&e.operation))
            forged.symbols[h->parameter].type.record = 1;
      if (mode == 3)
        for (auto &e : forged.expressions)
          if (std::holds_alternative<FailOperation>(e.operation))
            e.type.errorRecord = 2;
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "invalid error metadata admitted");
    }
    auto guarded = analyze(decl + "fn checked(x:bool)->i64 ! InputError:\n  if x:\n"
                                  "    fail InputError.Empty\n  return 42\nfn main()->i64:\n  return 0\n");
    require(guarded.ok(), "guard metadata fixture rejected");
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto forged = *guarded.module;
      for (auto &e : forged.expressions)
        if (auto *guard = std::get_if<GuardOperation>(&e.operation)) {
          if (mode == 0)
            guard->condition = SIZE_MAX;
          if (mode == 1)
            forged.expressions[guard->condition].type.task = true;
          if (mode == 2)
            guard->body.back().value = SIZE_MAX;
        }
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "malformed failure guard admitted");
    }
    std::cout << "Application errors: declared families, typed fail/try/catch, guards, "
                 "rethrow/replacement, ordinary values and explicit union/worker gates passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
