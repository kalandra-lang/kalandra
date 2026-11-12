#include "../gpu-source/DeviceErrorEntry.h"
#include "kalandra/Semantics.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
CheckResult analyze(std::string source, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("generics.k", std::move(source))), limits);
}
void good(const std::string &name, const std::string &source) {
  auto result = analyze(source);
  for (const auto &d : result.diagnostics)
    std::cerr << renderDiagnostic(*result.source, d);
  require(result.ok(), name);
  std::cout << "PASS " << name << '\n';
}
void bad(const std::string &name, const std::string &source, const std::string &code) {
  auto result = analyze(source);
  require(
      !result.ok() && !result.module &&
          std::ranges::any_of(result.diagnostics, [&](const auto &d) { return d.code == code; }),
      name + ": expected " + code);
  std::cout << "PASS " << name << '\n';
}
const std::string measure =
    "type Point:\n  x:i64\n"
    "trait Measure:\n  fn size(value:Self)->i64\n"
    "impl Measure for Point:\n  fn size(value:Self)->i64:\n    return value.x\n";
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected temporary module directory");
    good("inferred/explicit function specialization and values",
         "fn id[T](value:T)->T:\n  return value\n"
         "fn main()->i64:\n  f=id[i64]\n  a=id(12)\n  b=id[i32](7)\n  return f(a)+i64(b)\n");
    auto deduplicated = analyze("fn id[T](x:T)->T:\n  return x\n"
                                "fn main()->i64:\n  a=id(1)\n  b=id(2)\n  c=id[i32](3)\n"
                                "  return a+b+i64(c)\n");
    require(deduplicated.ok(), "specialization cache source");
    require(std::ranges::count_if(deduplicated.module->functions,
                                  [](const auto &f) {
                                    return !f.compileTimeOnly && f.name.starts_with("__generic_");
                                  }) == 2,
            "same concrete specialization was duplicated");
    good("generic nominal fields and enum inference",
         "type Box[T]:\n  value:T\nenum Option[T]:\n  None\n  Some:\n    value:T\n"
         "fn get[T](b:Box[T])->T:\n  return b.value\n"
         "fn unwrap[T](v:Option[T],other:T)->T:\n"
         "  return match v:\n    Option.None:other\n    Option.Some {value}:value\n"
         "fn main()->i64:\n  b=Box {value:12}\n  n:Option[i64]=Option.None\n"
         "  return get(b)+unwrap(n,3)\n");
    good("trait dispatch and specialization",
         measure + "fn get[T:Measure](x:T)->i64:\n  return Measure.size(x)\n"
                   "fn main()->i64:\n  p=Point {x:42}\n  return get(p)\n");
    good("associated types and generic implementation",
         "type Box[T]:\n  value:T\ntrait Read:\n  type Item\n"
         "  fn get(value:Self)->Self.Item\n"
         "impl[T] Read for Box[T]:\n  type Item=T\n"
         "  fn get(value:Self)->Self.Item:\n    return value.value\n"
         "fn read[T:Read](value:T)->T.Item:\n  return Read.get(value)\n"
         "fn main()->i64:\n  return read(Box {value:42})\n");
    good("generic errors inferred from a callback",
         "error Invalid:\n  Bad\nfn source(x:i64)->i64 ! Invalid:\n  return x\n"
         "fn apply[T,U,E](value:T,f:fn(T)->U ! E)->U ! E:\n  return try f(value)\n"
         "fn main()->i64 ! Invalid:\n  return try apply(42,source)\n");
    good("generic recursion and a closure before further specialization",
         "fn id[T](x:T)->T:\n  return x\n"
         "fn repeat[T](x:T,n:i64)->T:\n  if n==0:\n    return x\n"
         "  return repeat(x,n-1)\nfn main()->i64:\n"
         "  f=fn(x:i64):return id(x)\n  y=id[i32](2)\n  return repeat(f(40),20)+i64(y)\n");
    good("independent concrete values do not inherit abstract bounds",
         measure + "fn local[T](ignored:T)->i64:\n"
                   "  p=Point {x:7}\n  return Measure.size(p)\n"
                   "fn main()->i64:\n  return local(Point {x:42})\n");
    for (const auto &body :
         {"  return Measure.size(x)\n", "  alias=x\n  return Measure.size(alias)\n",
          "  b=Box {value:x}\n  return Measure.size(b.value)\n",
          "  f=fn():return Measure.size(x)\n  return f()\n", "  return Measure.size(identity(x))\n",
          "  return apply(x,fn(item):return Measure.size(item))\n",
          "  values=map([x],fn(item):return Measure.size(item))\n  return values[0]\n",
          "  for item in [x]:\n    return Measure.size(item)\n  return 0\n"}) {
      bad("undeclared bound through value flow",
          measure +
              "type Box[T]:\n  value:T\n"
              "fn identity[U](x:U):\n  return x\n"
              "fn apply[U](value:U,f:fn(U)->i64)->i64:\n  return f(value)\n"
              "fn use[T](x:T)->i64:\n" +
              body + "fn main()->i64:\n  return use(Point {x:42})\n",
          "S083");
    }
    good("bound preserved through a collecting loop",
         measure + "fn use[T:Measure](x:T)->i64:\n"
                   "  values=for item in [x]:\n    yield item\n"
                   "  return Measure.size(values[0])\n"
                   "fn main()->i64:\n  return use(Point {x:42})\n");
    bad("collecting loop cannot invent a bound",
        measure + "fn use[T](x:T)->i64:\n"
                  "  values=for item in [x]:\n    yield item\n"
                  "  return Measure.size(values[0])\n"
                  "fn main()->i64:\n  return use(Point {x:42})\n",
        "S083");
    good("associated field available during nominal registration",
         measure + "type Projection[T:Measure]:\n  value:T\n"
                   "fn main()->i64:\n  x=Projection {value:Point {x:42}}\n"
                   "  return x.value.x\n");
    bad("signature type arguments must carry their declared bounds",
        measure + "type Box[T:Measure]:\n  value:T\n"
                  "fn use[U](x:Box[U])->i64:\n  return 0\n"
                  "fn main()->i64:\n  return use(Box {value:Point {x:42}})\n",
        "S083");
    bad("specialized function values cannot invent bounds",
        measure + "fn need[T:Measure](x:T)->i64:\n  return Measure.size(x)\n"
                  "fn use[U](x:U)->i64:\n  f=need[U]\n  return f(x)\n"
                  "fn main()->i64:\n  return use(Point {x:42})\n",
        "S083");
    good("nested nominal Self inference",
         "type Box[T]:\n  value:T\ntrait Unbox:\n  fn get(value:Box[Self])->Self\n"
         "impl Unbox for i64:\n  fn get(value:Box[Self])->Self:\n    return value.value\n"
         "fn main()->i64:\n  return Unbox.get(Box {value:42})\n");
    bad("nested nominal Self cannot invent a bound",
        "type Box[T]:\n  value:T\ntrait Unbox:\n  fn get(value:Box[Self])->Self\n"
        "impl Unbox for i64:\n  fn get(value:Box[Self])->Self:\n    return value.value\n"
        "fn use[T](x:T)->T:\n  return Unbox.get(Box {value:x})\n"
        "fn main()->i64:\n  return use(42)\n",
        "S083");
    bad("unused generic record bounds are validated",
        "type Box[T:Missing]:\n  value:T\nfn main()->i64:\n  return 0\n", "S083");
    bad("unused associated bounds are validated",
        "trait Read:\n  type Item:Missing\nfn main()->i64:\n  return 0\n", "S083");
    good("generic callable implementation target",
         "trait Apply:\n  type Item\n  fn run(value:Self,x:Self.Item)->Self.Item\n"
         "impl[T] Apply for fn(T)->T:\n  type Item=T\n"
         "  fn run(value:Self,x:Self.Item)->Self.Item:\n    return value(x)\n"
         "fn id(x:i64)->i64:\n  return x\n"
         "fn main()->i64:\n  return Apply.run(id,42)\n");
    good("callback signature contextualizes an earlier integer literal",
         "fn apply[T,U](x:T,f:fn(T)->U)->U:\n  return f(x)\n"
         "fn narrow(x:i32)->i32:\n  return x\n"
         "fn main()->i64:\n  return i64(apply(42,narrow))\n");
    good("a local callback shadows a global result hint",
         "fn global()->String:\n  return \"text\"\n"
         "fn id[T](x:T)->T:\n  return x\n"
         "fn main()->i64:\n  global=fn()->i64:return 42\n  return id(global())\n");
    good("recursive generic enum retains nominal identity",
         "enum Chain[T]:\n  End\n  More:\n    value:T\n    next:Chain[T]\n"
         "fn head[T](chain:Chain[T],fallback:T)->T:\n"
         "  return match chain:\n    Chain.End:fallback\n    Chain.More {value,next}:value\n"
         "fn main()->i64:\n  end:Chain[i64]=Chain.End\n"
         "  return head(Chain.More {value:42,next:end},0)\n");
    for (const bool bounded : {false, true}) {
      const auto source =
          std::string("error Invalid:\n  Bad\ntrait Describe:\n  fn code(error:Self)->i64\n"
                      "impl Describe for Invalid:\n  fn code(error:Self)->i64:\n    return 7\n"
                      "fn fails()->i64 ! Invalid:\n  fail Invalid.Bad\n") +
          "fn handle[E" + (bounded ? ":Describe" : "") +
          "](callback:fn()->i64 ! E)->i64:\n"
          "  return callback() catch fn(error):return Describe.code(error)\n"
          "fn main()->i64:\n  return handle(fails)\n";
      if (bounded)
        good("error handler retains a declared generic bound", source);
      else
        bad("error handler cannot invent a generic bound", source, "S083");
    }
    for (const bool bounded : {false, true}) {
      const auto source =
          measure +
          "enum Option[T]:\n  None\n  Some:\n    value:T\n"
          "trait Wrapper:\n  fn size(value:Self)->i64\n"
          "impl[T:Measure] Wrapper for Option[T]:\n"
          "  fn size(value:Self)->i64:\n    return 0\n" +
          std::string("fn use[U") + (bounded ? ":Measure" : "") +
          "](x:U)->i64:\n  option:Option[U]=Option.None\n  return Wrapper.size(option)\n"
          "fn main()->i64:\n  return use(Point {x:42})\n";
      if (bounded)
        good("payload-free constructor preserves contextual type provenance", source);
      else
        bad("payload-free constructor cannot erase a generic requirement", source, "S083");
    }
    for (const bool bounded : {false, true}) {
      const auto source =
          measure + "type Callback[T]:\n  value:T\n  callback:fn(T)->i64\n" +
          std::string("fn use[U") + (bounded ? ":Measure" : "") +
          "](x:U)->i64:\n"
          "  box:Callback[U]=Callback {value:x,callback:fn(v):return Measure.size(v)}\n"
          "  return box.callback(x)\nfn main()->i64:\n  return use(Point {x:42})\n";
      if (bounded)
        good("nominal callback field preserves contextual parameter bounds", source);
      else
        bad("nominal callback field cannot invent a contextual bound", source, "S083");
    }
    for (const bool bounded : {false, true}) {
      const auto source =
          std::string("error Invalid:\n  Bad\ntrait Describe:\n  fn code(error:Self)->i64\n"
                      "impl Describe for Invalid:\n  fn code(error:Self)->i64:\n    return 7\n"
                      "fn fails()->i64 ! Invalid:\n  fail Invalid.Bad\n"
                      "fn produce[E](callback:fn()->i64 ! E):\n  return callback\n") +
          "fn handle[E" + (bounded ? ":Describe" : "") +
          "](callback:fn()->i64 ! E)->i64:\n"
          "  forwarded=produce[E](callback)\n"
          "  return forwarded() catch fn(error):return Describe.code(error)\n"
          "fn main()->i64:\n  return handle(fails)\n";
      if (bounded)
        good("inferred callback result preserves generic error bounds", source);
      else
        bad("inferred callback result cannot erase a generic error bound", source, "S083");
    }
    bad("constructed error union cannot invent a bound",
        "error Invalid:\n  Bad\nerror Fixed:\n  Other\ntype Both=Invalid | Fixed\n"
        "trait Describe:\n  fn code(error:Self)->i64\nimpl Describe for Both:\n"
        "  fn code(error:Self)->i64:\n    return 7\n"
        "fn fails()->i64 ! Invalid:\n  fail Invalid.Bad\n"
        "fn widen[E](callback:fn()->i64 ! E)->i64 ! (E | Fixed):\n  return try callback()\n"
        "fn handle[E](callback:fn()->i64 ! E)->i64:\n"
        "  return widen[E](callback) catch fn(error):return Describe.code(error)\n"
        "fn main()->i64:\n  return handle(fails)\n",
        "S083");
    bad("catch preserves its generic success path",
        measure + "error Invalid:\n  Bad\n"
                  "fn source()->Point ! Invalid:\n  return Point {x:42}\n"
                  "fn use[T](callback:fn()->T ! Invalid)->i64:\n"
                  "  value=callback() catch fn(error):return Point {x:7}\n"
                  "  return Measure.size(value)\nfn main()->i64:\n  return use(source)\n",
        "S083");
    const std::string errorTraits =
        "error Invalid:\n  Bad\ntype Both=Invalid | TaskError\n"
        "trait Describe:\n  fn code(error:Self)->i64\n"
        "impl Describe for Invalid:\n  fn code(error:Self)->i64:\n    return 7\n"
        "impl Describe for TaskError:\n  fn code(error:Self)->i64:\n    return 8\n"
        "impl Describe for Both:\n  fn code(error:Self)->i64:\n    return 9\n"
        "fn fails()->i64 ! Invalid:\n  fail Invalid.Bad\n";
    bad("a task group union does not inherit a member's bound",
        errorTraits + "fn handle[E:Describe](callback:fn()->i64 ! E)->i64:\n"
                      "  outcome=parallel:\n    try callback()\n    42\n"
                      "  return outcome catch fn(error):return Describe.code(error)\n"
                      "fn main()->i64:\n  return handle(fails)\n",
        "S083");
    good("handled generic errors do not taint a task group's error type",
         errorTraits +
             "fn handle[E](callback:fn()->i64 ! E)->i64:\n"
             "  outcome=parallel:\n    ignored=callback() catch fn(error):return 0\n    42\n"
             "  return outcome catch fn(error):return Describe.code(error)\n"
             "fn main()->i64:\n  return handle(fails)\n");
    bad("an opaque callable type does not expose an error trait contract",
        "error Invalid:\n  Bad\ntrait Describe:\n  fn code(error:Self)->i64\n"
        "impl Describe for Invalid:\n  fn code(error:Self)->i64:\n    return 7\n"
        "fn fails()->i64 ! Invalid:\n  fail Invalid.Bad\n"
        "fn handle[G](callback:G)->i64:\n"
        "  return callback() catch fn(error):return Describe.code(error)\n"
        "fn main()->i64:\n  return handle(fails)\n",
        "S083");
    const std::string narrowing =
        "error Invalid:\n  Bad\nerror Fixed:\n  Other\ntype Both=Invalid | Fixed\n"
        "trait Describe:\n  fn code(error:Self)->i64\n"
        "impl Describe for Invalid:\n  fn code(error:Self)->i64:\n    return 7\n"
        "impl Describe for Fixed:\n  fn code(error:Self)->i64:\n    return 8\n"
        "fn fails()->i64 ! Both:\n  fail Fixed.Other\n";
    good("named error branches expose concrete contracts",
         narrowing + "fn handle[E](callback:fn()->i64 ! E)->i64:\n"
                     "  return callback() catch fn(error):\n"
                     "    return match error:\n"
                     "      Invalid.Bad:Describe.code(error)\n"
                     "      Fixed.Other:Describe.code(error)\n"
                     "fn main()->i64:\n  return handle(fails)\n");
    bad("wildcard error narrowing cannot erase the abstract channel",
        narrowing + "fn handle[E](callback:fn()->i64 ! E)->i64:\n"
                    "  return callback() catch fn(error):\n"
                    "    return match error:\n"
                    "      Invalid.Bad:Describe.code(error)\n"
                    "      _:Describe.code(error)\n"
                    "fn main()->i64:\n  return handle(fails)\n",
        "S083");
    bad("an opaque array cannot expose an element's concrete trait",
        measure + "fn need[U:Measure](values:[U])->i64:\n"
                  "  return Measure.size(values[0])\n"
                  "fn use[G](values:G)->i64:\n  return need(values)\n"
                  "fn main()->i64:\n  return use([Point {x:42}])\n",
        "S083");
    bad("an opaque nominal cannot expose an argument's concrete trait",
        measure + "type Box[T]:\n  value:T\n"
                  "fn need[U:Measure](box:Box[U])->i64:\n  return Measure.size(box.value)\n"
                  "fn use[G](box:G)->i64:\n  return need(box)\n"
                  "fn main()->i64:\n  return use(Box {value:Point {x:42}})\n",
        "S083");
    for (const bool bounded : {false, true}) {
      const auto source =
          measure + "fn factory[T](value:T):\n  return fn():return value\n" +
          std::string("fn use[U") + (bounded ? ":Measure" : "") +
          "](value:U)->i64:\n  callback=factory(value)\n  return Measure.size(callback())\n"
          "fn main()->i64:\n  return use(Point {x:42})\n";
      if (bounded)
        good("inferred captured closure preserves its result contract", source);
      else
        bad("inferred captured closure cannot erase its result contract", source, "S083");
    }
    for (const bool bounded : {false, true}) {
      const auto source =
          std::string("error Invalid:\n  Bad\ntrait Describe:\n  fn code(error:Self)->i64\n"
                      "impl Describe for Invalid:\n  fn code(error:Self)->i64:\n    return 7\n"
                      "fn fails()->i64 ! Invalid:\n  fail Invalid.Bad\n"
                      "fn factory[E](callback:fn()->i64 ! E):\n"
                      "  return fn()->i64 ! E:return try callback()\n") +
          "fn handle[E" + (bounded ? ":Describe" : "") +
          "](callback:fn()->i64 ! E)->i64:\n"
          "  forwarded=factory(callback)\n"
          "  return forwarded() catch fn(error):return Describe.code(error)\n"
          "fn main()->i64:\n  return handle(fails)\n";
      if (bounded)
        good("inferred fallible closure retains its generic error contract", source);
      else
        bad("inferred fallible closure cannot erase its generic error contract", source, "S083");
    }
    auto adapter =
        analyze("error Invalid:\n  Bad\nfn checked(value:i64)->i64 ! Invalid:\n  return value\n"
                "fn apply(value:i64,callback:fn(i64)->i64 ! Invalid)->i64 ! Invalid:\n"
                "  return try callback(value)\nfn main()->i64 ! Invalid:\n  return try "
                "apply(42,checked)\n");
    require(adapter.ok(), "adapter callable-flow fixture");
    auto beforeAdapter = functionInvocationFlow(*adapter.module);
    require(bool(beforeAdapter), "original callable-flow proof");
    const auto beforeValues = adapter.module->expressions.size();
    std::size_t adapterBudget = 300000;
    auto entry = appendDeviceErrorEntry(*adapter.module, 1, adapterBudget);
    require(bool(entry), "error adapter creation");
    auto afterAdapter = functionInvocationFlow(*adapter.module);
    require(bool(afterAdapter), "adapter callable-flow proof");
    bool invocationChecked = false;
    for (ValueId value = 0; value < beforeValues; ++value)
      if (std::holds_alternative<FunctionInvoke>(adapter.module->expressions[value].operation)) {
        invocationChecked = true;
        require(!beforeAdapter->unknown[value] && !afterAdapter->unknown[value] &&
                    beforeAdapter->targets[value] == afterAdapter->targets[value],
                "generated error adapter polluted an existing closed target set");
      }
    require(invocationChecked, "adapter proof regression did not invoke a callback");
    adapter.module->functions[*entry].parameterOrigin = adapter.module->functions.size();
    require(!functionInvocationFlow(*adapter.module), "invalid adapter identity was trusted");
    bad("missing implementation",
        measure + "fn use[T:Measure](x:T)->i64:\n  return Measure.size(x)\n"
                  "fn main()->i64:\n  return use(42)\n",
        "S083");
    bad("missing required method",
        "trait A:\n  fn a(value:Self)->i64\n"
        "impl A for i64:\n  type Wrong=i64\n"
        "fn main()->i64:\n  return 0\n",
        "S083");
    bad("incorrect access contract",
        "trait A:\n  fn a(value:Self)->i64\nimpl A for [i64]:\n"
        "  fn a(mut value:[i64])->i64:\n    return length(value)\n"
        "fn main()->i64:\n  return 0\n",
        "S083");
    bad("incorrect effect contract",
        "error E:\n  Bad\ntrait A:\n  fn a(value:Self)->i64 ! E\n"
        "impl A for i64:\n  fn a(value:i64)->i64:\n    return value\n"
        "fn main()->i64:\n  return 0\n",
        "S083");
    bad("overlapping implementations",
        measure + "impl Measure for Point:\n  fn size(value:Point)->i64:\n"
                  "    return 0\nfn main()->i64:\n  return 0\n",
        "S083");
    bad("associated cycle",
        "trait A:\n  type First\n  type Second\n"
        "impl A for i64:\n  type First=Self.Second\n  type Second=Self.First\n"
        "fn main()->i64:\n  return 0\n",
        "S083");
    bad("unresolved type argument",
        "enum Option[T]:\n  None\n  Some:\n    value:T\n"
        "fn main()->i64:\n  x=Option.None\n  return 0\n",
        "S082");
    bad("conflicting type inference",
        "fn choose[T](a:T,b:T)->T:\n  return a\n"
        "fn main()->i64:\n  return choose(i64(1),i32(2))\n",
        "S082");
    bad("generic subscript is not an evaluated index",
        "fn id[T](x:T)->T:\n  return x\nfn main()->i64:\n  return id[0](42)\n", "S082");
    bad("generic function cannot escape unspecialized",
        "fn id[T](x:T)->T:\n  return x\nfn main()->i64:\n"
        "  f=id\n  return 0\n",
        "S082");
    SemanticLimits limited;
    limited.maxSpecializations = 1;
    auto exceeded = analyze("fn id[T](x:T)->T:\n  return x\n"
                            "fn main()->i64:\n  a=id[i64](1)\n  b=id[i32](2)\n"
                            "  return a+i64(b)\n",
                            limited);
    require(!exceeded.ok() && !exceeded.module &&
                std::ranges::any_of(exceeded.diagnostics,
                                    [](const auto &d) { return d.code == "S082"; }),
            "specialization limit did not fail closed");

    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto write = [&](const char *name, const std::string &text) {
      std::ofstream stream(root / name);
      stream << text;
      require(bool(stream), "module fixture write failed");
    };
    write("types.k", "export type Box[T]:\n  value:T\nexport trait Read:\n"
                     "  type Item\n  fn read(value:Self)->Self.Item\n"
                     "impl[T] Read for Box[T]:\n  type Item=T\n"
                     "  fn read(value:Self)->Self.Item:\n    return value.value\n"
                     "export fn get[T](b:Box[T])->T:\n  return b.value\n");
    write("main.k", "from \"./types.k\" import Box as B, Read as R, get as first, get as second\n"
                    "fn read[T:R](value:T)->T.Item:\n  return R.read(value)\n"
                    "fn main()->i64:\n  b=B {value:42}\n  return read(b)+first(b)+second(b)\n");
    auto loaded = loadModules((root / "main.k").string());
    require(bool(loaded), "module load");
    auto imported = check(std::move(*loaded));
    for (const auto &d : imported.diagnostics)
      std::cerr << renderDiagnostic(*imported.source, d);
    require(imported.ok(), "generic/trait imports lost nominal identity");
    write("foreign.k", "from \"./types.k\" import Box, Read\n"
                       "impl Read for Box[i64]:\n  type Item=i64\n"
                       "  fn read(value:Self)->i64:\n    return value.value\n"
                       "fn main()->i64:\n  return 0\n");
    loaded = loadModules((root / "foreign.k").string());
    require(bool(loaded), "orphan module load");
    auto orphan = check(std::move(*loaded));
    require(!orphan.ok() && std::ranges::any_of(orphan.diagnostics,
                                                [](const auto &d) { return d.code == "S083"; }),
            "foreign trait/type implementation accepted");
    std::cout << "PASS module identities and implementation coherence\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
