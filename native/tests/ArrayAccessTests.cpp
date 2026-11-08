#include "kalandra/Semantics.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static CheckResult analyze(const std::string &source) {
  return check(parse(std::make_shared<SourceFile>("array-access.k", source)));
}
int main() {
  try {
    const std::string prefix = "type Box:\n  values:[i64]\n"
                               "fn edit(mut values:[i64])->i64:\n  values[0]=9\n  return length(values)\n"
                               "fn read(values:[i64])->i64:\n  return length(values)\n"
                               "fn identity(values:[i64])->[i64]:\n  return values\n"
                               "fn box(values:[i64])->Box:\n  return Box {values:values}\n";
    // Repeated source allocation sites denote new backing on each execution.
    // Preserve every old alias and reader while distinguishing the new instance.
    for (const auto *replacement : {"sorted(a)", "copy(a)", "slice(a,0,length(a))", "concat(a,[1])",
                                    "map(a,fn(x):x)", "generate(length(a),fn(j):j)", "fresh(a)"}) {
      for (bool captured : {false, true})
        for (unsigned variant = 0; variant < 3; ++variant) {
          const auto source =
              prefix +
              "fn fresh(a:[i64])->[i64]:\n  copy(a)\n"
              "fn main()->i64 ! TaskError:\n  a=[3,1,2]\n  i=0\n  while i<3:\n"
              "    try parallel:\n      saved=a\n" +
              (captured ? std::string("      f=fn():read(saved)\n      t=spawn f()\n")
                        : std::string("      t=spawn read(saved)\n")) +
              "      a=" + replacement + "\n" +
              (variant == 2 ? std::string("      u=spawn read(a)\n") : std::string()) +
              (variant == 1 ? std::string("      edit(saved)\n") : std::string("      edit(a)\n")) +
              "      n=try await t\n" +
              (variant == 2 ? std::string("      n+try await u\n") : std::string("      n\n")) +
              "    i=i+1\n  length(a)\n";
          const auto result = analyze(source);
          require(variant ? !result.ok() &&
                                std::ranges::any_of(result.diagnostics,
                                                    [](const auto &d) { return d.code == "S077"; })
                          : result.ok(),
                  "allocation-instance proof lost fresh/old reader distinction:\n" + source);
        }
    }
    for (const auto *source : {
             "fn main()->i64 ! TaskError:\n  a=[3,1,2]\n  i=0\n  total=0\n  while i<1000:\n    "
             "old=try parallel:\n      task=spawn read(a)\n      a=sorted(a)\n      a[0]=9\n      "
             "try await task\n    total=total+old\n    i=i+1\n  return total+a[0]\n",
             "fn make()->[i64]:\n  a=[1]\n  i=0\n  while i<3:\n    a=copy(a)\n    i=i+1\n  return a\nfn "
             "main()->i64:\n  return edit(make())\n",
             "fn main()->i64 ! TaskError:\n  a=[3,1,2]\n  i=0\n  while i<3:\n    try parallel:\n   "
             "   t=spawn read(a)\n      j=0\n      while j<2:\n        a=sorted(a)\n        "
             "a[0]=9\n        j=j+1\n      try await t\n    i=i+1\n  return a[0]\n",
             "fn run(a:[i64])->i64:\n  a=sorted(a)\n  a[0]=9\n  return a[0]\nfn main()->i64:\n  return "
             "run([1,2])\n",
             "fn run(a:[i64],flag:bool)->i64:\n  b=a\n  if flag:\n    b=copy(a)\n  else:\n    "
             "b=[2]\n  b[0]=3\n  return b[0]\nfn main()->i64:\n  return run([1],true)\n",
             "fn main()->i64:\n  a=[3,1,2]\n  b=sort(a)\n  return b[0]\n",
             "fn run(a:[i64])->[i64]:\n  return sorted(a)\nfn main()->i64:\n  return run([2,1])[0]\n",
             "fn main()->i64 ! TaskError:\n  a=[2,1]\n  return try parallel:\n    t=spawn read(a)\n    "
             "b=sorted(a)\n    b[0]+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[2,1]\n  b=try parallel:\n    try await spawn "
             "sorted(a)\n  return b[0]\n",
             "fn main()->i64:\n  a=[1,2]\n  b=a\n  return edit(b)\n",
             "fn main()->i64:\n  a=[1,2]\n  return edit(identity(a))\n",
             "fn main()->i64:\n  a=[1,2]\n  f=fn()->[i64]:return a\n  return edit(f())\n",
             "fn make()->[i64]:\n  return [1,2]\nfn main()->i64:\n  f=make\n  return edit(f())\n",
             "fn main()->i64:\n  a=[1,2]\n  b=box(a)\n  return edit(b.values)\n",
             "fn forward(mut a:[i64])->i64:\n  return edit(a)\nfn main()->i64:\n  return forward([1])\n",
             "fn forward(a:[i64])->i64:\n  return edit(copy(a))\nfn main()->i64:\n  return forward([1])\n",
             "fn make()->[i64]:\n  return [1,2]\nfn main()->i64:\n  return edit(make())\n",
             "fn main()->i64:\n  a=[1,2]\n  f=fn():return length(a)\n  return edit(a)+f()\n",
             "fn main()->i64:\n  f:fn(mut [i64])->i64=edit\n  return f([1])\n",
             "fn apply(f:fn(mut [i64])->i64,mut a:[i64])->i64:\n  return f(a)\n"
             "fn main()->i64:\n  return apply(edit,[1])\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn read(a)\n"
             "    n=try await t\n    edit(a)+n\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn read(a)\n"
             "    alias=t\n    n=try await alias\n    edit(a)+n\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn read(a)\n"
             "    n=edit(copy(a))\n    n+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  b=copy(a)\n  return try parallel:\n"
             "    t=spawn read(b)\n    n=edit(a)\n    n+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  n=try parallel:\n    t=spawn read(a)\n"
             "    try await t\n  return edit(a)+n\n",
             "fn privateWork()->i64:\n  a=[1,2]\n  return edit(a)\n"
             "fn main()->i64 ! TaskError:\n  return try parallel:\n    try await spawn privateWork()\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn read(a)\n"
             "    u=spawn read(a)\n    n=try await t\n    m=try await u\n    edit(a)+n+m\n",
         }) {
      auto result = analyze(prefix + source);
      if (!result.ok()) {
        std::string errors;
        for (const auto &d : result.diagnostics)
          errors += d.code + ": " + d.message + "\n";
        throw std::runtime_error("valid local access rejected:\n" + std::string(source) + errors);
      }
    }
    for (const auto *source : {
             "fn main()->i64 ! TaskError:\n  a=[3,1,2]\n  i=0\n  while i<3:\n    try parallel:\n   "
             "   saved=a\n      t=spawn read(saved)\n      a=sorted(a)\n      saved[0]=9\n      "
             "try await t\n    i=i+1\n  return a[0]\n",
             "fn main()->i64 ! TaskError:\n  a=[3,1,2]\n  i=0\n  while i<3:\n    try parallel:\n   "
             "   t=spawn read(a)\n      a=sorted(a)\n      u=spawn read(a)\n      a[0]=9\n      "
             "first=try await t\n      first+try await u\n    i=i+1\n  return a[0]\n",
             "fn main()->i64 ! TaskError:\n  a=[3,1,2]\n  i=0\n  while i<3:\n    try parallel:\n   "
             "   t=spawn read(a)\n      a[0]=if true:\n        j=0\n        while j<2:\n          "
             "a=copy(a)\n          j=j+1\n        4\n      else:0\n      try await t\n    i=i+1\n  "
             "return a[0]\n",
             "fn run(a:[i64])->i64:\n  b=[0]\n  i=0\n  while i<2:\n    b[0]=1\n    b=a\n    "
             "i=i+1\n  return 0\nfn main()->i64:\n  return 0\n",
             "fn run(a:[i64],flag:bool)->i64:\n  b=[1]\n  if flag:\n    b=a\n  b[0]=3\n  return b[0]\nfn "
             "main()->i64:\n  return 0\n",
             "fn run(a:[i64])->i64:\n  b=[1]\n  b=a\n  b[0]=9\n  return 0\nfn main()->i64:\n  return 0\n",
             "fn run(a:[i64])->[i64]:\n  return sort(a)\nfn main()->i64:\n  return 0\n",
             "fn main()->i64 ! TaskError:\n  a=[2,1]\n  return try parallel:\n    t=spawn read(a)\n    "
             "b=sort(a)\n    b[0]+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[2,1]\n  b=try parallel:\n    try await spawn "
             "sort(a)\n  return b[0]\n",
             "fn forward(a:[i64])->i64:\n  return edit(a)\nfn main()->i64:\n  return forward([1])\n",
             "fn forward(a:[i64])->i64:\n  b=a\n  return edit(b)\nfn main()->i64:\n  return 0\n",
             "fn forward(a:[i64])->i64:\n  f=fn()->[i64]:return a\n  return edit(f())\nfn main()->i64:\n  return 0\n",
             "fn capture(mut a:[i64])->i64:\n  f=fn():return edit(a)\n  return f()\nfn main()->i64:\n  return 0\n",
             "fn apply(f:fn(mut [i64])->i64,a:[i64])->i64:\n  return f(a)\nfn main()->i64:\n  return 0\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    try await spawn "
             "edit(a)\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn read(a)\n"
             "    n=edit(a)\n    n+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  b=a\n  return try parallel:\n    t=spawn read(a)\n"
             "    n=edit(b)\n    n+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  b=identity(a)\n  return try parallel:\n    "
             "t=spawn read(a)\n"
             "    n=edit(b)\n    n+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  b=box(a)\n  return try parallel:\n    t=spawn "
             "read(a)\n"
             "    n=edit(b.values)\n    n+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn read(a)\n"
             "    u=spawn read(a)\n    n=try await t\n    m=edit(a)\n    n+m+try await u\n",
             "fn run(flag:bool)->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn "
             "read(a)\n"
             "    if flag:\n      n=try await t\n    m=edit(a)\n    m+try await t\n"
             "fn main()->i64:\n  return 0\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn read(a)\n"
             "    while false:\n      n=try await t\n    m=edit(a)\n    m+try await t\n",
             "fn run(mut a:[i64],b:[i64])->i64 ! TaskError:\n  return try parallel:\n    t=spawn read(b)\n"
             "    n=edit(a)\n    n+try await t\nfn main()->i64:\n  return 0\n",
             "fn run(a:[i64])->i64 ! TaskError:\n  f=fn():return read(a)\n  return try parallel:\n"
             "    t=spawn f()\n    n=edit(a)\n    n+try await t\nfn main()->i64:\n  return 0\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  f=fn():return read(a)\n  return try parallel:\n"
             "    t=spawn f()\n    n=edit(a)\n    n+try await t\n",
             "fn main()->i64 ! TaskError:\n  a=[1,2]\n  return try parallel:\n    t=spawn map(a,fn(x):return x)\n"
             "    n=edit(a)\n    b=try await t\n    n+length(b)\n",
         }) {
      auto result = analyze(prefix + source);
      require(!result.ok() && std::ranges::any_of(result.diagnostics,
                                                  [](const auto &d) { return d.code == "S077"; }),
              "unsafe sharing accepted or failed before the access proof:\n" + std::string(source));
    }
    for (const auto *source : {
             "fn main()->i64:\n  return length(sort([\"b\",\"a\"]))\n",
             "fn main()->i64:\n  return length(sorted([\"b\",\"a\"]))\n",
             "fn main()->i64:\n  sort(tensor([2,1]))\n  return 0\n",
             "fn main()->i64:\n  return sorted(1)\n",
             "fn main()->i64:\n  return sort([1],[2])\n",
         })
      require(!analyze(source).ok(), "sort admitted unsupported storage or arity");
    auto validSort = analyze("fn main()->[i64]:\n  return sort([2,1])\n");
    require(validSort.ok(), "valid sorting fixture failed");
    require(numericCapabilityError(*validSort.module, NumericTarget::AppleGpu).has_value(),
            "numeric capability verifier admitted GPU sorting");
    for (auto &expression : validSort.module->expressions)
      if (auto *sort = std::get_if<ArraySort>(&expression.operation)) {
        sort->input = validSort.module->expressions.size();
        require(numericCapabilityError(*validSort.module, NumericTarget::Cpu).has_value(),
                "numeric capability verifier admitted forged sorting operand");
        break;
      }
    // No direct mut declaration exists: the indirect contract must still run
    // the access proof for a read-only array argument.
    auto indirect =
        analyze("fn apply(f:fn(mut [i64])->i64,a:[i64])->i64:\n  return f(a)\nfn main()->i64:\n  return 0\n");
    require(!indirect.ok() && std::ranges::any_of(indirect.diagnostics,
                                                  [](const auto &d) { return d.code == "S077"; }),
            "indirect-only mutation contract bypassed access checking");
    std::cout << "Array access: local/copy origins, readonly parameters/captures, alias returns, "
                 "contracts and task settlement passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
