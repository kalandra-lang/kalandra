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
static CheckResult analyze(const char *text) {
  return check(parse(std::make_shared<SourceFile>("control-maps.k", text)));
}
int main() {
  try {
    {
      auto result = analyze(R"k(error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:21}
  value
fn recover(value:int)->int:
  checked(value) catch fn(error):
    match error:
      E.Bad {value}:value
fn main()->int:
  result=map([0,21],recover)
  result[0]+result[1]
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
      bool bridge = false;
      for (const auto &entry : plan->plans)
        for (const auto &op : entry->code)
          if (op.code == continuation::Code::FunctionReference)
            bridge = true;
      require(bridge, "control-flow map omitted checked callable bridge");
      for (unsigned change = 0; change < 3; ++change) {
        auto forged = *result.module;
        auto it = std::ranges::find_if(forged.expressions, [](const auto &e) {
          return std::holds_alternative<ArrayMap>(e.operation);
        });
        auto &map = std::get<ArrayMap>(it->operation);
        if (change == 0)
          ++forged.functions[map.function].captureCount;
        if (change == 1)
          map.function = SIZE_MAX;
        if (change == 2)
          map.captures.push_back(SIZE_MAX);
        require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
                "forged map ABI admitted");
      }
    }
    {
      auto result = analyze(R"k(error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn main()->int:
  offset:int=21
  result=map([0,21],fn(value):checked(value) catch fn(error):offset)
  result[0]+result[1]
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(enum Choice:
  Empty
  Some:
    value:int
fn select(choice:Choice)->int:
  match choice:
    Choice.Empty:20
    Choice.Some {value}:value
fn main()->int:
  result=map([Choice.Empty,Choice.Some {value:22}],select)
  result[0]+result[1]
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(enum Choice:
  Empty
  Some:
    value:int
fn select(choice:Choice)->Bool:
  match choice:
    Choice.Empty:false
    Choice.Some {value}:value==22
fn label(value:Bool)->String:
  if value:"é"
  else:"!"
fn main()->int:
  values=map([Choice.Empty,Choice.Some {value:22}],select)
  labels=map(values,label)
  length(labels[0])+length(labels[1])+40
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(type Row:
  text:String
  values:[int]
error E:
  Bad:
    row:Row
fn checked(value:int)->Row ! E:
  if value==0:fail E.Bad {row:Row {text:"é"+"!",values:[20,20]}}
  Row {text:"!",values:[value]}
fn recover(value:int)->Row:
  checked(value) catch fn(error):
    match error:
      E.Bad {row}:row
fn main()->int:
  rows=map([0,1],recover)
  rows[0].values[0]+rows[0].values[1]+length(rows[0].text)
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(enum Choice:
  Empty
  Some:
    value:int
fn select(choice:Choice)->int:
  match choice:
    Choice.Empty:10
    Choice.Some {value}:value
fn batch(value:int)->int:
  results=map([Choice.Empty,Choice.Some {value:value}],select)
  results[0]+results[1]
fn main()->int:
  results=map([10,12],batch)
  results[0]+results[1]
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(enum Choice:
  Empty
  Some:
    value:int
fn make(choice:Choice)->fn(int)->int:
  match choice:
    Choice.Empty:fn(value:int):value+20
    Choice.Some {value}:fn(extra:int):value+extra
fn apply(callback:fn(int)->int)->int:
  match Choice.Empty:
    Choice.Empty:callback(1)
    Choice.Some {value}:value
fn main()->int:
  callbacks=map([Choice.Empty,Choice.Some {value:20}],make)
  results=map(callbacks,apply)
  results[0]+results[1]
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn recover(value:int)->int:
  checked(value) catch fn(error):1/(value-value)
fn main()->int:
  values:[int]=[]
  result=map(values,recover)
  length(result)+42
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn worker(offset:int)->[int]:
  map([0,21],fn(value):checked(value) catch fn(error):offset)
fn main()->int ! TaskError:
  try parallel:
    task=spawn worker(21)
    first=try await task
    second=try await task
    first[0]+second[1]
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn recover(value:int)->int:
  checked(value) catch fn(error):21
fn main()->int ! TaskError:
  try parallel:
    task=spawn map([0,21],recover)
    values=try await task
    values[0]+values[1]
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
    }
    {
      auto result = analyze(R"k(type Row:
  text:String
  values:[int]
error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn probe(input:int)->int:
  row=Row {text:"é"+"!",values:[input]}
  result=map([0,1],fn(value):checked(value) catch fn(error):row.values[0]+length(row.text)-1)
  result[0]+result[1]
fn main()->int:
  f=probe
  probe(40)
)k");
      require(result.ok(), "control-flow map rejected");
      require(!numericCapabilityError(*result.module, NumericTarget::Cpu),
              "CPU capability rejected");
      auto plan = continuation::plan(*result.module);
      require(bool(plan), "control-flow map planning failed");
      bool bridge = false;
      for (const auto &entry : plan->plans)
        for (const auto &op : entry->code)
          if (op.code == continuation::Code::FunctionReference)
            bridge = true;
      require(bridge, "control-flow map omitted checked callable bridge");
      for (unsigned change = 0; change < 3; ++change) {
        auto forged = *result.module;
        auto it = std::ranges::find_if(forged.expressions, [](const auto &e) {
          return std::holds_alternative<ArrayMap>(e.operation);
        });
        auto &map = std::get<ArrayMap>(it->operation);
        if (change == 0)
          ++forged.functions[map.function].captureCount;
        if (change == 1)
          map.function = SIZE_MAX;
        if (change == 2)
          map.captures.push_back(SIZE_MAX);
        require(bool(numericCapabilityError(forged, NumericTarget::Cpu)),
                "forged map ABI admitted");
      }
    }
    require(!analyze(R"k(fn checked(value:int)->int ! TaskError:
  try parallel:
    try await spawn identity(value)
fn identity(value:int)->int:
  value
fn recover(value:int)->int:
  checked(value) catch fn(error):0
fn main()->int:
  values=map([1],recover)
  values[0]
)k")
                 .ok(),
            "invalid control-flow map accepted");
    require(!analyze(R"k(error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn main()->int:
  values=map([1],checked)
  values[0]
)k")
                 .ok(),
            "invalid control-flow map accepted");
    require(!analyze(R"k(error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn main()->int:
  values=map([1],fn(value):try checked(value))
  values[0]
)k")
                 .ok(),
            "invalid control-flow map accepted");
    require(analyze(R"k(error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn recover(callback:fn(int)->int)->int:
  checked(callback(0)) catch fn(error):42
fn main()->int ! TaskError:
  callbacks=[fn(value:int):value]
  try parallel:
    values=try await spawn map(callbacks,recover)
    values[0]
)k")
                 .ok(),
            "supported function-array transport rejected");
    std::cout << "Control maps: 12 source programs, 6 forged ABI cases and 3 effect "
                 "rejections passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
