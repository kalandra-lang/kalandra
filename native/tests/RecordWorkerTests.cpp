#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("record-workers.k", text)));
}
int main() {
  try {
    for (auto element : {"int32", "uint32", "int64", "uint64", "float32", "float64"}) {
      const std::string type(element);
      const auto source =
          "type Point:\n  x:" + type +
          "\n  valid:Bool\n"
          "fn shift(p:Point)->Point:\n  p with {x:p.x+1}\n"
          "fn nested(p:Point)->Point ! TaskError:\n  try parallel:\n"
          "    work=spawn shift(p)\n    first=try await work\n    again=try await work\n    again\n"
          "fn main()->" +
          type +
          " ! TaskError:\n  p=try parallel:\n"
          "    work=spawn nested(Point {x:1,valid:true})\n    try await work\n  p.x\n";
      auto result = analyze(source);
      require(result.ok(), "typed record worker rejected: " + type);
      auto planned = continuation::plan(*result.module);
      require(bool(planned), planned ? "" : planned.error());
      require(planned->kernels.records.size() == 1, "record table lost in kernel module");
      auto dump = dumpTypedIR(*result.module);
      require(dump.find("task<Point>") != std::string::npos, "record task identity lost in dump");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
      require(lowered->llvm.find("value.layout.record.0") != std::string::npos,
              "record result layout lost");
#endif
    }
    const std::string declaration = "type Point:\n  x:int\n  valid:Bool\n";
    for (auto body :
         {"fn make(x:int)->Point:\n  Point {x:10/x,valid:true}\nfn attempt()->Point ! TaskError:\n "
          " try parallel:\n    try await spawn make(0)\nfn main():\n  p=attempt() catch fn(error): "
          "Point {x:7,valid:false}\n  p.x\n",
          "fn make(x:int)->Point:\n  Point {x:10/x,valid:true}\nfn attempt()->Point ! TaskError:\n "
          " try parallel:\n    try await spawn make(0)\nfn main():\n  p=attempt() catch "
          "fn(error):\n    match error:\n      TaskError.Fault {kind}:\n        match kind:\n      "
          "    FaultKind.Arithmetic: Point {x:7,valid:false}\n          _: Point "
          "{x:8,valid:false}\n      _: Point {x:9,valid:false}\n  p.x\n",
          "fn make(x:int)->Point:\n  Point {x:10/x,valid:true}\nfn attempt()->Point ! TaskError:\n "
          " try parallel:\n    try await spawn make(0)\nfn relay()->Point ! TaskError:\n  "
          "attempt() catch fn(error): fail error\nfn main():\n  p=relay() catch fn(error): Point "
          "{x:7,valid:false}\n  p.x\n",
          "fn make(p:Point)->Point:\n  p with {x:p.x+1}\nfn main()->[int] ! TaskError:\n  p=try "
          "parallel:\n    try await spawn make(Point {x:1,valid:true})\n  map([1,2],fn(x):Point "
          "{x:x,valid:true}.x+p.x)\n",
          "fn make(p:Point)->Point:\n  p\nfn main()->int ! TaskError:\n  try parallel:\n    "
          "work=spawn make(Point {x:1,valid:true})\n    p=await work catch fn(error): Point "
          "{x:7,valid:false}\n    p.x\n"}) {
      auto result = analyze(declaration + body);
      require(result.ok(), "record recovery/control flow rejected");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      auto lowered = lowerNative(*result.module);
      require(bool(lowered), lowered ? "" : lowered.error());
#endif
    }
    for (auto body :
         {"fn echo(p:Point)->Point:\n  p\nfn main() ! TaskError:\n  try parallel:\n    work=spawn "
          "echo(Point {x:1,valid:true})\n    work.x\n",
          "fn echo(p:Point)->Point:\n  p\nfn main() ! TaskError:\n  try parallel:\n    work=spawn "
          "echo(Point {x:1,valid:true})\n    (await work).x\n",
          "fn echo(p:Point)->Point:\n  p\nfn main() ! TaskError:\n  parallel:\n    spawn "
          "echo(Point {x:1,valid:true})\n",
          "type Other:\n  x:int\n  valid:Bool\nfn echo(p:Point)->Point:\n  p\nfn main() ! "
          "TaskError:\n  try parallel:\n    work=spawn echo(Other {x:1,valid:true})\n    try await "
          "work\n",
          "type Other:\n  x:int\n  valid:Bool\nfn make(x:int)->Point:\n  Point "
          "{x:10/x,valid:true}\nfn attempt()->Point ! TaskError:\n  try parallel:\n    try await "
          "spawn make(0)\nfn main():\n  attempt() catch fn(error): Other {x:7,valid:false}\n"})
      require(!analyze(declaration + body).ok(), "invalid record task flow admitted");
    // A valid record task cannot erase or change its nominal identity in IR.
    auto valid = analyze(declaration +
                         "fn echo(p:Point)->Point:\n  p\nfn main()->int ! TaskError:\n  p=try "
                         "parallel:\n    try await spawn echo(Point {x:1,valid:true})\n  p.x\n");
    require(valid.ok(), "record task IR fixture failed");
    auto task =
        std::ranges::find_if(valid.module->expressions, [](const auto &e) { return e.type.task; });
    require(task != valid.module->expressions.end(), "missing record handle");
    auto forged = *valid.module;
    forged.expressions[task - valid.module->expressions.begin()].type.record = SIZE_MAX;
    require(numericCapabilityError(forged, NumericTarget::Cpu).has_value(),
            "forged record handle admitted");
    const auto constructor = std::ranges::find_if(valid.module->expressions, [](const auto &e) {
      return std::holds_alternative<RecordValue>(e.operation);
    });
    require(constructor != valid.module->expressions.end(), "missing record constructor");
    for (bool asTask : {false, true}) {
      auto forgedConstructor = *valid.module;
      auto &type =
          forgedConstructor.expressions[constructor - valid.module->expressions.begin()].type;
      type.task = asTask;
      type.fallible = !asTask;
      require(numericCapabilityError(forgedConstructor, NumericTarget::Cpu).has_value() &&
                  !continuation::plan(forgedConstructor),
              "record constructor with task/error storage flags admitted");
#ifndef KALANDRA_RECORD_FRONTEND_ONLY
      require(!lowerNative(forgedConstructor),
              "malformed record constructor reached native storage");
#endif
    }
    require(numericCapabilityError(*valid.module, NumericTarget::AppleGpu).has_value(),
            "GPU record tasks enabled");
    std::cout << "record workers: six numeric families, nested/cached results, record recovery, "
                 "match/fail, nominal identity, kernel metadata and task/GPU gates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
