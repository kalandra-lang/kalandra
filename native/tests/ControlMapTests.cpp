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
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:21}
  return value
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):
    return match error:
      E.Bad {value}:value
fn main()->i64:
  result=map([0,21],recover)
  return result[0]+result[1]
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
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn main()->i64:
  offset:i64=21
  result=map([0,21],fn(value):return checked(value) catch fn(error):return offset)
  return result[0]+result[1]
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
    value:i64
fn select(choice:Choice)->i64:
  return match choice:
    Choice.Empty:20
    Choice.Some {value}:value
fn main()->i64:
  result=map([Choice.Empty,Choice.Some {value:22}],select)
  return result[0]+result[1]
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
    value:i64
fn select(choice:Choice)->bool:
  return match choice:
    Choice.Empty:false
    Choice.Some {value}:value==22
fn label(value:bool)->String:
  return if value:"é"
  else:"!"
fn main()->i64:
  values=map([Choice.Empty,Choice.Some {value:22}],select)
  labels=map(values,label)
  return length(labels[0])+length(labels[1])+40
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
  values:[i64]
error E:
  Bad:
    row:Row
fn checked(value:i64)->Row ! E:
  if value==0:fail E.Bad {row:Row {text:"é"+"!",values:[20,20]}}
  return Row {text:"!",values:[value]}
fn recover(value:i64)->Row:
  return checked(value) catch fn(error):
    return match error:
      E.Bad {row}:row
fn main()->i64:
  rows=map([0,1],recover)
  return rows[0].values[0]+rows[0].values[1]+length(rows[0].text)
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
    value:i64
fn select(choice:Choice)->i64:
  return match choice:
    Choice.Empty:10
    Choice.Some {value}:value
fn batch(value:i64)->i64:
  results=map([Choice.Empty,Choice.Some {value:value}],select)
  return results[0]+results[1]
fn main()->i64:
  results=map([10,12],batch)
  return results[0]+results[1]
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
    value:i64
fn make(choice:Choice)->fn(i64)->i64:
  return match choice:
    Choice.Empty:fn(value:i64):return value+20
    Choice.Some {value}:fn(extra:i64):return value+extra
fn apply(callback:fn(i64)->i64)->i64:
  return match Choice.Empty:
    Choice.Empty:callback(1)
    Choice.Some {value}:value
fn main()->i64:
  callbacks=map([Choice.Empty,Choice.Some {value:20}],make)
  results=map(callbacks,apply)
  return results[0]+results[1]
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
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):return 1/(value-value)
fn main()->i64:
  values:[i64]=[]
  result=map(values,recover)
  return length(result)+42
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
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn worker(offset:i64)->[i64]:
  return map([0,21],fn(value):return checked(value) catch fn(error):return offset)
fn main()->i64 ! TaskError:
  return try parallel:
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
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):return 21
fn main()->i64 ! TaskError:
  return try parallel:
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
  values:[i64]
error E:
  Bad
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn probe(input:i64)->i64:
  row=Row {text:"é"+"!",values:[input]}
  result=map([0,1],fn(value):return checked(value) catch fn(error):return row.values[0]+length(row.text)-1)
  return result[0]+result[1]
fn main()->i64:
  f=probe
  return probe(40)
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
    require(!analyze(R"k(fn checked(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn identity(value)
fn identity(value:i64)->i64:
  return value
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):return 0
fn main()->i64:
  values=map([1],recover)
  return values[0]
)k")
                 .ok(),
            "invalid control-flow map accepted");
    require(!analyze(R"k(error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn main()->i64:
  values=map([1],checked)
  return values[0]
)k")
                 .ok(),
            "invalid control-flow map accepted");
    require(!analyze(R"k(error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn main()->i64:
  values=map([1],fn(value):return try checked(value))
  return values[0]
)k")
                 .ok(),
            "invalid control-flow map accepted");
    require(analyze(R"k(error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn recover(callback:fn(i64)->i64)->i64:
  return checked(callback(0)) catch fn(error):return 42
fn main()->i64 ! TaskError:
  callbacks=[fn(value:i64):return value]
  return try parallel:
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
