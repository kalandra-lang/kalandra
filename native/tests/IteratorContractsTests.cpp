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
    std::cout
        << "PASS: canonical Option aliases, specialization, ownership and exhaustion contracts\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
