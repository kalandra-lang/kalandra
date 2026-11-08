#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("mixed-errors.k", text)));
}
int main() {
  try {
    const std::string types = "error A:\n  Bad:\n    text:String\nerror B:\n  Missing\n"
                              "type Mixed=A | TaskError\ntype Only=TaskError | TaskError\n";
    const std::string legacy = "fn divide(x:i64)->i64:\n  return 1/x\nfn legacy(x:i64)->i64 ! TaskError:\n"
                               "  return try parallel:\n    try await spawn divide(x)\n";
    const std::string work =
        "fn work(x:i64)->i64 ! Mixed:\n  if x==0:fail A.Bad {text:\"bad\"}\n  return try legacy(x-1)\n";
    const std::string match =
        "fn main()->i64:\n  return work(1) catch fn(error):\n    return match error:\n"
        "      A.Bad {text}:length(text)\n      TaskError.Fault {kind,message}:\n"
        "        match kind:\n          FaultKind.Arithmetic:length(message)\n          _:0\n"
        "      TaskError.Cancelled:1\n      TaskError.DeadlineExceeded:2\n"
        "      TaskError.AdmissionRejected {reason}:length(reason)\n";
    for (const auto &program :
         {types + legacy + work + match, types + legacy + "fn main()->i64 ! Mixed:\n  return legacy(0)\n",
          types + legacy + "fn main()->i64 ! (TaskError | A | B):\n  return try legacy(0)\n",
          types + legacy + "fn main()->i64 ! Only:\n  return try legacy(0)\n",
          types + legacy + "fn main()->i64 ! Mixed:\n  return legacy(0) catch fn(error):fail error\n",
          types + legacy + work + "fn main()->i64 ! Mixed:\n  return work(1) catch fn(error):fail error\n",
          types + legacy +
              "fn main()->i64 ! Mixed:\n  return if true:legacy(0)\n  else:fail A.Bad {text:\"bad\"}\n",
          types + legacy +
              "fn main()->i64 ! Mixed:\n  return legacy(1) catch fn(error):\n    if true:fail error\n    "
              "fail A.Bad {text:\"bad\"}\n",
          types + legacy + work +
              "fn main()->i64 ! Mixed:\n  return try parallel:\n    try await spawn work(1)\n",
          types + legacy + work + "fn main()->i64 ! Mixed:\n  return try parallel:\n    try work(1)\n",
          types + legacy +
              "fn forward(error:Only)->i64 ! Mixed:\n  fail error\nfn main()->i64:\n  return 0\n"}) {
      auto result = analyze(program);
      if (!result.ok())
        for (const auto &d : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, d);
      require(result.ok(), "valid mixed channel rejected");
      require(bool(continuation::plan(*result.module)), "mixed continuation rejected");
#ifndef KALANDRA_MIXED_ERROR_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error().c_str());
#endif
    }
    for (const auto *body :
         {"fn main()->i64 ! A:\n  return try legacy(0)\n", "fn main()->i64 ! TaskError:\n  return try work(0)\n",
          "fn main()->i64 ! A:\n  return work(1)\n",
          "fn main()->i64:\n  return work(1) catch fn(error):\n    return match error:\n      A.Bad:1\n      "
          "TaskError.Fault:2\n",
          "fn main()->i64:\n  return work(1) catch fn(error):\n    return match error:\n      "
          "TaskError.Cancelled {reason}:1\n      _:2\n",
          "fn main()->i64:\n  return work(1) catch fn(error):\n    return match error:\n      TaskError.Fault "
          "{reason}:1\n      _:2\n",
          "fn main()->i64 ! A:\n  return try parallel:\n    try await spawn work(1)\n",
          "fn main()->i64 ! A:\n  return try parallel:\n    try work(1)\n"})
      require(!analyze(types + legacy + work + body).ok(), "invalid mixed channel admitted");
    auto checked = analyze(types + legacy + work + match);
    require(checked.ok(), "metadata fixture rejected");
    const auto &module = *checked.module;
    const auto task = std::size_t(
        std::ranges::find_if(module.records, [](const auto &r) { return r.taskErrorFamily; }) -
        module.records.begin());
    for (unsigned mode = 0; mode < 8; ++mode) {
      auto forged = module;
      auto &record = forged.records[task];
      if (mode == 0)
        record.error = false;
      if (mode == 1)
        record.variants[0].name = "Wrong";
      if (mode == 2)
        record.fields[1].type.builtin = BuiltinValue::Numeric;
      if (mode == 3)
        forged.records.push_back(record);
      if (mode == 4)
        record.taskErrorFamily = false;
      if (mode >= 5)
        for (auto &e : forged.expressions)
          if (auto *read = std::get_if<ErrorFieldRead>(&e.operation); read && read->taskSlot) {
            if (mode == 5)
              read->taskSlot = SIZE_MAX;
            if (mode == 6)
              e.type.builtin = BuiltinValue::Numeric;
            if (mode == 7)
              read->subject = SIZE_MAX;
            break;
          }
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
              "malformed mixed metadata admitted");
    }
    std::cout << "Mixed errors: TaskError aliases, subset propagation, exact fields, handlers and "
                 "rejected metadata passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
