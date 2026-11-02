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
    const std::string prefix = "type Box:\n  values:[int]\n"
                               "fn edit(mut values:[int])->int:\n  values[0]=9\n  length(values)\n"
                               "fn read(values:[int])->int:\n  length(values)\n"
                               "fn identity(values:[int])->[int]:\n  values\n"
                               "fn box(values:[int])->Box:\n  Box {values:values}\n";
    for (const auto *source : {
             "fn run(a:[int])->int:\n  a=sorted(a)\n  a[0]=9\n  a[0]\nfn main()->int:\n  run([1,2])\n",
             "fn run(a:[int],flag:Bool)->int:\n  b=a\n  if flag:\n    b=copy(a)\n  else:\n    b=[2]\n  b[0]=3\n  b[0]\nfn main()->int:\n  run([1],true)\n",
             "fn main()->int:\n  a=[3,1,2]\n  b=sort(a)\n  b[0]\n",
             "fn run(a:[int])->[int]:\n  sorted(a)\nfn main()->int:\n  run([2,1])[0]\n",
             "fn main()->int ! TaskError:\n  a=[2,1]\n  try parallel:\n    t=spawn read(a)\n    "
             "b=sorted(a)\n    b[0]+try await t\n",
             "fn main()->int ! TaskError:\n  a=[2,1]\n  b=try parallel:\n    try await spawn "
             "sorted(a)\n  b[0]\n",
             "fn main()->int:\n  a=[1,2]\n  b=a\n  edit(b)\n",
             "fn main()->int:\n  a=[1,2]\n  edit(identity(a))\n",
             "fn main()->int:\n  a=[1,2]\n  f=fn()->[int]:a\n  edit(f())\n",
             "fn make()->[int]:\n  [1,2]\nfn main()->int:\n  f=make\n  edit(f())\n",
             "fn main()->int:\n  a=[1,2]\n  b=box(a)\n  edit(b.values)\n",
             "fn forward(mut a:[int])->int:\n  edit(a)\nfn main()->int:\n  forward([1])\n",
             "fn forward(a:[int])->int:\n  edit(copy(a))\nfn main()->int:\n  forward([1])\n",
             "fn make()->[int]:\n  [1,2]\nfn main()->int:\n  edit(make())\n",
             "fn main()->int:\n  a=[1,2]\n  f=fn():length(a)\n  edit(a)+f()\n",
             "fn main()->int:\n  f:fn(mut [int])->int=edit\n  f([1])\n",
             "fn apply(f:fn(mut [int])->int,mut a:[int])->int:\n  f(a)\n"
             "fn main()->int:\n  apply(edit,[1])\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn read(a)\n"
             "    n=try await t\n    edit(a)+n\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn read(a)\n"
             "    alias=t\n    n=try await alias\n    edit(a)+n\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn read(a)\n"
             "    n=edit(copy(a))\n    n+try await t\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  b=copy(a)\n  try parallel:\n"
             "    t=spawn read(b)\n    n=edit(a)\n    n+try await t\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  n=try parallel:\n    t=spawn read(a)\n"
             "    try await t\n  edit(a)+n\n",
             "fn privateWork()->int:\n  a=[1,2]\n  edit(a)\n"
             "fn main()->int ! TaskError:\n  try parallel:\n    try await spawn privateWork()\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn read(a)\n"
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
             "fn run(a:[int])->int:\n  b=[0]\n  i=0\n  while i<2:\n    b[0]=1\n    b=a\n    i=i+1\n  0\nfn main()->int:\n  0\n",
             "fn run(a:[int],flag:Bool)->int:\n  b=[1]\n  if flag:\n    b=a\n  b[0]=3\n  b[0]\nfn main()->int:\n  0\n",
             "fn run(a:[int])->int:\n  b=[1]\n  b=a\n  b[0]=9\n  0\nfn main()->int:\n  0\n",
             "fn run(a:[int])->[int]:\n  sort(a)\nfn main()->int:\n  0\n",
             "fn main()->int ! TaskError:\n  a=[2,1]\n  try parallel:\n    t=spawn read(a)\n    "
             "b=sort(a)\n    b[0]+try await t\n",
             "fn main()->int ! TaskError:\n  a=[2,1]\n  b=try parallel:\n    try await spawn "
             "sort(a)\n  b[0]\n",
             "fn forward(a:[int])->int:\n  edit(a)\nfn main()->int:\n  forward([1])\n",
             "fn forward(a:[int])->int:\n  b=a\n  edit(b)\nfn main()->int:\n  0\n",
             "fn forward(a:[int])->int:\n  f=fn()->[int]:a\n  edit(f())\nfn main()->int:\n  0\n",
             "fn capture(mut a:[int])->int:\n  f=fn():edit(a)\n  f()\nfn main()->int:\n  0\n",
             "fn apply(f:fn(mut [int])->int,a:[int])->int:\n  f(a)\nfn main()->int:\n  0\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    try await spawn "
             "edit(a)\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn read(a)\n"
             "    n=edit(a)\n    n+try await t\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  b=a\n  try parallel:\n    t=spawn read(a)\n"
             "    n=edit(b)\n    n+try await t\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  b=identity(a)\n  try parallel:\n    "
             "t=spawn read(a)\n"
             "    n=edit(b)\n    n+try await t\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  b=box(a)\n  try parallel:\n    t=spawn "
             "read(a)\n"
             "    n=edit(b.values)\n    n+try await t\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn read(a)\n"
             "    u=spawn read(a)\n    n=try await t\n    m=edit(a)\n    n+m+try await u\n",
             "fn run(flag:Bool)->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn "
             "read(a)\n"
             "    if flag:\n      n=try await t\n    m=edit(a)\n    m+try await t\n"
             "fn main()->int:\n  0\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn read(a)\n"
             "    while false:\n      n=try await t\n    m=edit(a)\n    m+try await t\n",
             "fn run(mut a:[int],b:[int])->int ! TaskError:\n  try parallel:\n    t=spawn read(b)\n"
             "    n=edit(a)\n    n+try await t\nfn main()->int:\n  0\n",
             "fn run(a:[int])->int ! TaskError:\n  f=fn():read(a)\n  try parallel:\n"
             "    t=spawn f()\n    n=edit(a)\n    n+try await t\nfn main()->int:\n  0\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  f=fn():read(a)\n  try parallel:\n"
             "    t=spawn f()\n    n=edit(a)\n    n+try await t\n",
             "fn main()->int ! TaskError:\n  a=[1,2]\n  try parallel:\n    t=spawn map(a,fn(x):x)\n"
             "    n=edit(a)\n    b=try await t\n    n+length(b)\n",
         }) {
      auto result = analyze(prefix + source);
      require(!result.ok() && std::ranges::any_of(result.diagnostics,
                                                  [](const auto &d) { return d.code == "S077"; }),
              "unsafe sharing accepted or failed before the access proof:\n" + std::string(source));
    }
    for (const auto *source : {
             "fn main()->int:\n  length(sort([\"b\",\"a\"]))\n",
             "fn main()->int:\n  length(sorted([\"b\",\"a\"]))\n",
             "fn main()->int:\n  sort(tensor([2,1]))\n  0\n",
             "fn main()->int:\n  sorted(1)\n",
             "fn main()->int:\n  sort([1],[2])\n",
         })
      require(!analyze(source).ok(), "sort admitted unsupported storage or arity");
    auto validSort = analyze("fn main()->[int]:\n  sort([2,1])\n");
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
        analyze("fn apply(f:fn(mut [int])->int,a:[int])->int:\n  f(a)\nfn main()->int:\n  0\n");
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
