#include "../lib/IteratorCapabilities.h"
#include "../lib/RecordLayout.h"
#include "kalandra/Semantics.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected isolated fixture directory");
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto checkSource = [&](const std::string &text) {
      const auto path = root / "main.k";
      {
        std::ofstream stream(path);
        stream << text;
        require(bool(stream), "cannot write iterator contract fixture");
      }
      auto loaded = loadModules(path.string());
      require(bool(loaded), "cannot load iterator contract fixture");
      return check(std::move(*loaded));
    };
    auto canonical = checkSource(
        "from \"std/option\" import Option\nfrom \"std/option\" import Option as Maybe\n"
        "fn get[T](value:Option[T],fallback:T)->T:\n  return match value:\n"
        "    Option.None:fallback\n    Option.Some {value}:value\n"
        "fn main()->i64:\n  absent:Maybe[i64]=Maybe.None\n"
        "  present=Option.Some {value:42}\n  text=Maybe.Some {value:\"yes\"}\n"
        "  unused=get(text,\"no\")\n  return get(absent,7)+get(present,0)\n");
    for (const auto &d : canonical.diagnostics)
      std::cerr << renderDiagnostic(*canonical.source, d);
    require(canonical.ok(), "standard Option aliases lost generic nominal identity");
    auto resource = checkSource("from \"std/option\" import Option\n"
                                "from \"std/buffer\" import byteBuffer,BufferError\n"
                                "fn main()->unit ! BufferError:\n  value=try byteBuffer(0)\n"
                                "  illegal=Option.Some {value:value}\n  return ()\n");
    require(!resource.ok() && !resource.module,
            "standard Option bypassed owned-resource aggregate restrictions");
    auto exhaustive =
        checkSource("from \"std/option\" import Option\nfn get(value:Option[i64])->i64:\n"
                    "  return match value:\n    Option.Some {value}:value\n"
                    "fn main()->i64:\n  return get(Option[i64].None)\n");
    require(!exhaustive.ok() && !exhaustive.module,
            "standard Option omitted exhaustion without an exhaustiveness error");
    auto emptyErrors = checkSource("fn plain(value:i64)->i64:\n  return value\n"
                                   "fn pure(value:i64)->i64 ! never:\n  return value+1\n"
                                   "fn apply[T,E](value:T,callback:fn(T)->T ! E)->T ! E:\n"
                                   "  return try callback(value)\n"
                                   "fn main()->i64:\n  a=try pure(40)\n  b=try apply(a,plain)\n"
                                   "  callback=fn(value:i64)->i64 ! never:return value+1\n"
                                   "  return try apply(b,callback)\n");
    for (const auto &d : emptyErrors.diagnostics)
      std::cerr << renderDiagnostic(*emptyErrors.source, d);
    require(emptyErrors.ok(), "never did not unify direct, generic and anonymous error channels");
    require(std::ranges::all_of(emptyErrors.module->functions,
                                [](const auto &f) { return !f.resultType.fallible; }),
            "never retained a runtime expected-error signature");
    require(std::ranges::none_of(emptyErrors.module->expressions,
                                 [](const auto &e) {
                                   const auto *operation = std::get_if<TaskOperation>(&e.operation);
                                   return operation && operation->prefix == TaskPrefix::Try;
                                 }),
            "try of a proven-empty channel retained an error operation");
    auto associatedError =
        checkSource("trait Flow:\n  type Error\n  fn run(value:Self)->i64 ! Self.Error\n"
                    "impl Flow for i64:\n  type Error=never\n"
                    "  fn run(value:Self)->i64 ! never:\n    return value\n"
                    "fn invoke[T:Flow](value:T)->i64 ! T.Error:\n  return try Flow.run(value)\n"
                    "fn main()->i64:\n  return try invoke(42)\n");
    require(associatedError.ok(), "associated never error channel failed static trait dispatch");
    auto normalized = checkSource("error Invalid:\n  Bad\ntype Empty=never\n"
                                  "fn pure()->i64 ! (never | Empty):\n  return 42\n"
                                  "fn checked(value:bool)->i64 ! (never | Invalid):\n"
                                  "  if value:\n    fail Invalid.Bad\n  return try pure()\n"
                                  "fn main()->i64 ! Invalid:\n  return try checked(false)\n");
    require(normalized.ok(), "never aliases/unions did not normalize");
    for (const auto &record : normalized.module->records)
      require(
          std::ranges::none_of(record.errorMembers,
                               [&](auto id) { return normalized.module->records[id].uninhabited; }),
          "never survived inside an error union");
    for (const auto &source :
         {"error Invalid:\n  Bad\nfn wrong()->i64 ! never:\n  fail Invalid.Bad\n"
          "fn main()->i64:\n  return wrong()\n",
          "fn main()->never:\n  return never {}\n",
          "type never:\n  value:i64\nfn main()->i64:\n  return 0\n",
          "type Empty=never\ntrait Mark:\n  fn mark(value:Self)->i64\n"
          "impl Mark for never:\n  fn mark(value:Self)->i64:\n    return 0\n"
          "impl Mark for Empty:\n  fn mark(value:Self)->i64:\n    return 1\n"
          "fn main()->i64:\n  return 0\n"}) {
      auto invalid = checkSource(source);
      require(!invalid.ok() && !invalid.module,
              "never admitted a value, error, reserved declaration or duplicate implementation");
    }
    auto malformed = *emptyErrors.module;
    const auto bottom =
        std::ranges::find_if(malformed.records, [](const auto &r) { return r.uninhabited; });
    require(bottom != malformed.records.end(), "never lost its canonical empty type");
    bottom->variants.push_back({"Forged", {}, {}});
    require(bool(recordFieldError(malformed)), "an inhabited never type passed IR validation");
    auto ranges = checkSource("from \"std/iter\" import Range, range as steps\n"
                              "fn sum(values:Range)->i64:\n  total=0\n"
                              "  for value in values:\n    total=total+value\n  return total\n"
                              "fn main()->i64:\n  values=steps(10,0,-2)\n"
                              "  return sum(values)+sum(steps(4))+sum(steps(2,5))\n");
    for (const auto &d : ranges.diagnostics)
      std::cerr << renderDiagnostic(*ranges.source, d);
    require(ranges.ok(), "first-class reusable Range values failed source checking");
    auto forgedRange = *ranges.module;
    auto range = std::ranges::find_if(forgedRange.records, [](const auto &r) {
      return r.standard == StandardRecord::Range;
    });
    require(range != forgedRange.records.end(), "Range lost its standard nominal identity");
    range->fields[2].type.builtin = BuiltinValue::Bool;
    require(bool(recordFieldError(forgedRange)), "a forged Range field passed layout validation");
    const std::string cursorPrelude = "from \"std/iter\" import Iterator,iter,next,close,range\n"
                                      "from \"std/option\" import Option\n";
    auto cursors = checkSource(
        cursorPrelude +
        "fn make()->Iterator[i64,never]:\n  return iter(range(3))\n"
        "fn advance(mut cursor:Iterator[i64,never])->Option[i64]:\n  return next(cursor)\n"
        "fn main()->unit:\n  cursor=make()\n  value=advance(cursor)\n  close(cursor)\n  return "
        "()\n");
    for (const auto &d : cursors.diagnostics)
      std::cerr << renderDiagnostic(*cursors.source, d);
    require(cursors.ok(), "owned cursor factory, mutation and transfer failed source checking");
    for (const auto &expr : cursors.module->expressions)
      require(!iteratorCapabilityError(*cursors.module, expr, NumericTarget::Cpu),
              "checked iterator operation failed its executable-IR contract");
    for (const auto &source :
         {"fn bad(cursor:Iterator[i64,never])->Option[i64]:\n  return next(cursor)\n"
          "fn main()->unit:\n  return ()\n",
          "fn bad(cursor:Iterator[i64,never])->unit:\n  close(cursor)\n  return ()\n"
          "fn main()->unit:\n  return ()\n",
          "fn main()->unit:\n  cursor=iter(range(3))\n  close(cursor)\n"
          "  value=next(cursor)\n  return ()\n",
          "fn main()->unit:\n  cursor=iter(range(3))\n  values=[cursor]\n  return ()\n",
          "fn main()->unit:\n  cursor=iter(range(3))\n"
          "  value=Option.Some {value:cursor}\n  return ()\n",
          "fn main()->unit:\n  cursor=iter(range(3))\n"
          "  raw=cursor.__storage\n  return ()\n",
          "fn main()->unit:\n  cursor=Iterator[i64,never] {__storage:[]}\n  return ()\n",
          "fn main()->unit:\n  cursor=iter(range(3))\n"
          "  changed=cursor with {__storage:[]}\n  return ()\n"}) {
      auto invalid = checkSource(cursorPrelude + source);
      require(!invalid.ok() && !invalid.module,
              "cursor admitted unauthorized mutation, ownership reuse or opaque storage access");
    }
    const auto loanCheck = [&](const std::string &program, bool valid) {
      auto result = checkSource(cursorPrelude +
                                "fn borrow(values:[i64])->Iterator[i64,never]:\n"
                                "  return iter(values)\n" +
                                program);
      if (valid != result.ok()) {
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << renderDiagnostic(*result.source, diagnostic);
        throw std::runtime_error("array cursor loan proof mismatch:\n" + program);
      }
      if (valid)
        for (const auto &expr : result.module->expressions)
          require(!iteratorCapabilityError(*result.module, expr, NumericTarget::Cpu),
                  "array cursor passed checking with an invalid runtime schema");
      else
        require(
            std::ranges::any_of(result.diagnostics,
                                [](const auto &diagnostic) { return diagnostic.code == "S077"; }),
            "array cursor failed without a read-loan diagnostic");
    };
    for (const auto *program : {
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  value=next(c)\n  close(c)\n  a[0]=9\n  "
             "return ()\n",
             "fn main()->unit:\n  i=0\n  while i<4:\n    c=iter([i])\n    value=next(c)\n    "
             "close(c)\n    i=i+1\n  return ()\n",
             "fn main()->unit:\n  a=[3,1]\n  i=0\n  while i<4:\n    c=borrow(a)\n    "
             "value=next(c)\n    close(c)\n    a[0]=i\n    i=i+1\n  return ()\n",
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  d=borrow(a)\n  x=next(c)\n  close(c)\n "
             " y=next(d)\n  close(d)\n  a[0]=9\n  return ()\n",
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  d=c\n  close(d)\n  a[0]=9\n  return "
             "()\n",
             "fn done(own c:Iterator[i64,never])->unit:\n  close(c)\n  return ()\n"
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  done(c)\n  a[0]=9\n  return ()\n",
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  a=copy(a)\n  a[0]=9\n  close(c)\n  "
             "return ()\n",
             "fn wrap[T](values:T)->T:\n  return values\n"
             "fn make(values:[i64])->Iterator[i64,never]:\n  return borrow(values)\n"
             "fn main()->unit:\n  a=[3,1]\n  c=make(a)\n  close(c)\n  a[0]=9\n  return ()\n",
             "fn move(own c:Iterator[i64,never])->Iterator[i64,never]:\n  return c\n"
             "fn main()->unit:\n  a=[3,1]\n  c=move(borrow(a))\n  close(c)\n  a[0]=9\n  return "
             "()\n",
         })
      loanCheck(program, true);
    for (const auto *program : {
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  a[0]=9\n  close(c)\n  return ()\n",
             "fn main()->unit:\n  a=[3,1]\n  b=a\n  c=borrow(a)\n  sort(b)\n  close(c)\n  return "
             "()\n",
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  d=borrow(a)\n  close(c)\n  a[0]=9\n  "
             "close(d)\n  return ()\n",
             "fn make(values:[i64])->Iterator[i64,never]:\n  return borrow(values)\n"
             "fn main()->unit:\n  a=[3,1]\n  c=make(a)\n  a[0]=9\n  close(c)\n  return ()\n",
             "fn move(own c:Iterator[i64,never])->Iterator[i64,never]:\n  return c\n"
             "fn main()->unit:\n  a=[3,1]\n  c=move(borrow(a))\n  a[0]=9\n  close(c)\n  return "
             "()\n",
             "fn bad(mut values:[i64],mut c:Iterator[i64,never])->unit:\n"
             "  values[0]=9\n  value=next(c)\n  return ()\n"
             "fn main()->unit:\n  a=[3,1]\n  c=borrow(a)\n  bad(a,c)\n  close(c)\n  return ()\n",
         })
      loanCheck(program, false);
    for (const auto *program : {
             "fn bad(mut values:[i64],mut c:Iterator[i64,never])->unit:\n"
             "  values[0]=9\n  value=next(c)\n  return ()\n"
             "fn main()->unit:\n  return ()\n",
             "fn edit(mut values:[i64])->i64:\n  values[0]=9\n  return 1\n"
             "fn receive(own cursor:Iterator[i64,never],count:i64)->unit:\n  close(cursor)\n  "
             "return ()\n"
             "fn main()->unit:\n  a=[1]\n  receive(iter(a),edit(a))\n  return ()\n",
             "fn drain(own c:Iterator[i64,never])->i64:\n  value=next(c)\n  close(c)\n  return 1\n"
             "fn main()->i64 ! TaskError:\n  a=[1]\n  c=iter(a)\n  return try parallel:\n"
             "    pending=spawn drain(c)\n    a[0]=9\n    try await pending\n",
             "fn main()->unit:\n  a=[1]\n  c=iter(a)\n  if true:\n    close(c)\n  a[0]=9\n  return "
             "()\n",
             "fn main()->unit:\n  a=[1]\n  c=iter(a)\n  d=iter(a)\n  close(c)\n  a[0]=9\n  "
             "close(d)\n  return ()\n",
         }) {
      const auto invalid = checkSource(cursorPrelude + program);
      require(!invalid.ok() && std::ranges::any_of(invalid.diagnostics,
                                                   [](const auto &diagnostic) {
                                                     return diagnostic.code == "S077";
                                                   }),
              "array cursor lost a call, task or branch read loan");
    }
    const std::string drainSource =
        "fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n  while true:\n"
        "    match next(c):\n      Option.None:break\n      Option.Some {value}:()\n"
        "  a[0]=9\n  again=next(c)\n  a[0]=8\n  close(c)\n  return ()\n";
    for (const auto &program : {
             drainSource,
             "type Recursive:\n  tail:Option[Recursive]\n" + drainSource,
             std::string(
                 "fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n  result=next(c)\n  alias=result\n"
                 "  match alias:\n    Option.None:\n      a[0]=9\n      ()\n    Option.Some "
                 "{value}:()\n  close(c)\n  return ()\n"),
             std::string("fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n"
                         "  advance:fn(mut Iterator[i64,never])->Option[i64]=next[i64,never]\n"
                         "  while true:\n    match advance(c):\n      Option.Some {value}:()\n     "
                         " Option.None:break\n"
                         "  a[0]=9\n  close(c)\n  return ()\n"),
         }) {
      auto valid = checkSource(cursorPrelude + program);
      if (!valid.ok()) {
        for (const auto &diagnostic : valid.diagnostics)
          std::cerr << renderDiagnostic(*valid.source, diagnostic);
        throw std::runtime_error("proven exhaustion did not release the cursor's read loan");
      }
    }
    for (const auto *program : {
             "fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n  fake:Option[i64]=Option.None\n"
             "  match fake:\n    Option.None:\n      a[0]=9\n      ()\n    Option.Some "
             "{value}:()\n  close(c)\n  return ()\n",
             "fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n  while true:\n"
             "    match next(c):\n      Option.None:break\n      Option.Some {value}:break\n"
             "  a[0]=9\n  close(c)\n  return ()\n",
             "fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n  d=iter(a)\n  while true:\n"
             "    match next(c):\n      Option.None:break\n      Option.Some {value}:()\n"
             "  a[0]=9\n  close(c)\n  close(d)\n  return ()\n",
             "fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n  i=0\n  while i<1:\n"
             "    match next(c):\n      Option.None:break\n      Option.Some {value}:()\n    "
             "i=i+1\n"
             "  a[0]=9\n  close(c)\n  return ()\n",
         }) {
      const auto invalid = checkSource(cursorPrelude + program);
      require(!invalid.ok() && std::ranges::any_of(invalid.diagnostics,
                                                   [](const auto &diagnostic) {
                                                     return diagnostic.code == "S077";
                                                   }),
              "an unrelated Option or early loop exit released a live read loan");
    }
    for (const auto *program : {
             "fn read(values:[i64])->i64:\n  return values[0]\n"
             "fn main()->unit ! TaskError:\n  a=[1,2]\n  c=iter(a)\n  try parallel:\n"
             "    pending=spawn read(a)\n    while true:\n      match next(c):\n"
             "        Option.None:break\n        Option.Some {value}:()\n"
             "    a[0]=9\n    value=try await pending\n    ()\n  close(c)\n  return ()\n",
             "fn replace(own c:Iterator[i64,never],a:[i64])->Iterator[i64,never]:\n"
             "  close(c)\n  return iter(a)\n"
             "fn main()->unit:\n  a=[1,2]\n  c=iter(a)\n  old=next(c)\n  fresh=replace(c,a)\n"
             "  match old:\n    Option.None:\n      a[0]=9\n      ()\n    Option.Some {value}:()\n"
             "  close(fresh)\n  return ()\n",
         }) {
      const auto invalid = checkSource(cursorPrelude + program);
      require(!invalid.ok() && std::ranges::any_of(invalid.diagnostics,
                                                   [](const auto &diagnostic) {
                                                     return diagnostic.code == "S077";
                                                   }),
              "cursor exhaustion released another cursor or task's read loan");
    }
    auto nestedLoan = checkSource(
        cursorPrelude +
        "type Box:\n  values:[i64]\n"
        "fn main()->unit:\n  a=[1]\n  c=iter([Box {values:a}])\n"
        "  value=match next(c):\n    Option.None:Box {values:[]}\n    Option.Some {value}:value\n"
        "  reader=iter(value.values)\n  close(c)\n  a[0]=9\n  close(reader)\n  return ()\n");
    require(!nestedLoan.ok() && std::ranges::any_of(nestedLoan.diagnostics,
                                                    [](const auto &d) { return d.code == "S077"; }),
            "a yielded record lost its array backing provenance in another cursor");
    auto forgedCursor = *cursors.module;
    auto cursor = std::ranges::find_if(
        forgedCursor.records, [](const auto &r) { return r.standard == StandardRecord::Iterator; });
    require(cursor != forgedCursor.records.end(), "Iterator lost its canonical nominal identity");
    cursor->typeArguments.pop_back();
    require(bool(recordFieldError(forgedCursor)), "a forged cursor lost its error type metadata");
    auto capture = checkSource(cursorPrelude +
                               "fn main()->unit:\n  cursor=iter(range(3))\n"
                               "  callback=fn()->Option[i64]:return next(cursor)\n  return ()\n");
    require(!capture.ok() && std::ranges::any_of(capture.diagnostics,
                                                 [](const auto &diagnostic) {
                                                   return diagnostic.code == "S081" &&
                                                          diagnostic.message.find("Iterator") !=
                                                              std::string::npos;
                                                 }),
            "an implicit cursor capture lost its ownership diagnostic");
    std::cout
        << "PASS: canonical Option aliases, specialization, ownership and exhaustion contracts\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
