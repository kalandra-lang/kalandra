#include "Hover.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::optional<lsp::HoverInfo> query(const std::string &text, const std::string &needle,
                                           std::size_t within = 0, CheckOptions options = {}) {
  const auto at = text.find(needle);
  require(at != std::string::npos, "missing test token");
  auto source = std::make_shared<const SourceFile>("hover.k", text);
  return lsp::hoverInfo(parse(source), "hover.k", at + within, options);
}
static void expect(const std::string &text, const std::string &needle, const std::string &expected,
                   std::size_t within = 0, CheckOptions options = {}) {
  auto result = query(text, needle, within, options);
  require(result && result->text == expected, "hover mismatch for " + needle + ": " +
                                                  (result ? result->text : "null") +
                                                  " != " + expected + "\nsource:\n" + text);
  require(result->source->path() == "hover.k" && result->span.begin <= text.find(needle) + within &&
              result->span.end > text.find(needle) + within,
          "hover range did not cover token");
}
int main() {
  try {
    const std::string code = "fn double(value:i64):\n  return value*2\nfn main():\n  "
                             "result=double(21)\n  return result\n";
    expect(code, "double(value", "fn double(value: i64) -> i64");
    expect(code, "double(21", "fn double(value: i64) -> i64", 2);
    expect(code, "value:i64", "value: i64");
    expect(code, "value*2", "value: i64");
    expect(code, "result=", "result: i64");
    expect(code, "result\n", "result: i64");
    expect(code, "21", "21: i64");
    require(!query(code, "fn double"), "keyword produced misleading hover");
    require(!query(code, "  result"), "whitespace produced misleading hover");
    require(!query(code, "*2"), "operator produced misleading hover");
    const std::string store =
        "fn put(mut values:[i64],index:i64)->i64:\n  values[index]=9\n  return "
        "values[index]\nfn main()->i64:\n  return put([1],0)\n";
    expect(store, "values[index]=", "values: [i64]");
    expect(store, "index]=", "index: i64");
    require(!query(store, "=9"), "element assignment leaked its private statement marker");
    require(!query("fn main():\n  return missing\n", "main"), "failed graph leaked partial types");
    const std::string closure =
        "fn main():\n  offset=1\n  transform=fn(x:i64):return x+offset\n  return transform(2)\n";
    expect(closure, "transform=", "transform: fn(i64) -> i64");
    expect(closure, "transform(2", "transform: fn(i64) -> i64");
    expect(closure, "x:i64", "x: i64");
    const std::string tensor = "fn matrix(value:Tensor[f32,?,2])->Tensor[f32,?,2]:\n  return "
                               "value\nfn main():\n  return 0\n";
    expect(tensor, "matrix", "fn matrix(value: Tensor[f32, ?, 2]) -> Tensor[f32, ?, 2]");
    expect(tensor, "value\n", "value: Tensor[f32, ?, 2]");
    const std::string record =
        "type Box:\n  value:i64\nfn main():\n  box=Box {value:42}\n  return box.value\n";
    expect(record, "Box:", "type Box");
    expect(record, "value:i64", "value: i64");
    expect(record, "box.value", "value: i64", 4);
    expect(record, "box=", "box: Box");
    const std::string strings = "fn main()->String:\n  text=\"😀```é\"\n  return text\n";
    expect(strings, "\"😀", "String", 2);
    expect(strings, "text=", "text: String");
    const std::string errors =
        "error InputError:\n  Invalid\nfn checked(value:i64)->i64 ! InputError:\n"
        "  if value==0:\n    fail InputError.Invalid\n  return value\nfn main()->i64:\n  return "
        "checked(1) catch "
        "fn(error):return 0\n";
    expect(errors, "checked(value", "fn checked(value: i64) -> i64 ! InputError");
    expect(errors, "checked(1", "fn checked(value: i64) -> i64 ! InputError");
    const std::string factory = "error InputError:\n  Bad\nerror BuildError:\n  Bad\n"
                                "fn checked(value:i64)->i64 ! InputError:\n  return value\n"
                                "fn factory()->(fn(i64)->i64 ! InputError) ! BuildError:\n  return "
                                "checked\nfn main():\n  return 0\n";
    expect(factory, "factory()", "fn factory() -> (fn(i64) -> i64 ! InputError) ! BuildError");
    const std::string unionError =
        "error A:\n  Bad\nerror B:\n  Bad\ntype Both=A | B\n"
        "fn joined()->i64 ! Both:\n  fail A.Bad\nfn main():\n  return 0\n";
    expect(unionError, "joined()", "fn joined() -> i64 ! A | B");
    const std::string pipeline =
        "fn twice(value:i64):\n  return value*2\nfn main():\n  return 21 -> twice\n";
    expect(pipeline, "twice\n", "fn twice(value: i64) -> i64");
    // Distinct same-spelled parameters must use semantic declaration identity.
    const std::string shadow = "fn first(value:i64):\n  return value\nfn second(value:String):\n  "
                               "return value\nfn main():\n  return 0\n";
    expect(shadow, "value:i64", "value: i64");
    expect(shadow, "value:String", "value: String");
    expect(code, "i64):", "i64: i64");
    auto parsed = parse(std::make_shared<const SourceFile>("hover.k", code));
    require(!lsp::hoverInfo(std::move(parsed), "other.k", 3), "foreign source position matched");
    std::string large = "fn large(";
    for (unsigned i = 0; i < 3000; ++i) {
      if (i)
        large += ',';
      large += "parameter" + std::to_string(i) + ":i64";
    }
    large += "):\n  return 0\nfn main():\n  return 0\n";
    require(check(parse(std::make_shared<SourceFile>("hover.k", large))).ok(),
            "large fixture is invalid");
    require(!query(large, "large("), "hover presentation size limit was bypassed");
    expect("fn main():\n  return length([1,2])\n", "length", "fn length(input: [i64]) -> i64", 2);
    expect("fn main():\n  return [1,2] -> length\n", "length", "fn length(input: [i64]) -> i64");
    expect("fn main():\n  return [1,2] -> concat([3])\n", "concat",
           "fn concat(left: [i64], right: [i64]) -> [i64]");
    expect("fn main():\n  return f32(1)\n", "f32", "fn f32(input: i64) -> f32");
    expect("fn main():\n  return array(tensor([1,2],[1,2]))\n", "array",
           "fn array(input: Tensor[i64, 1, 2]) -> [i64]");
    expect("fn main():\n  return relu(tensor([-1,2]))\n", "relu",
           "fn relu(input: Tensor[i64, 2]) -> Tensor[i64, 2]");
    expect("fn main():\n  return reduceSum(tensor([1,2]))\n", "reduceSum",
           "fn reduceSum(input: Tensor[i64, 2]) -> i64");
    expect("fn main():\n  return tensor([1,2],[1,2]) -> matmul(tensor([3,4],[2,1]))\n", "matmul",
           "fn matmul(left: Tensor[i64, 1, 2], right: Tensor[i64, 2, 1]) -> Tensor[i64, 1, 1]");
    expect("fn main():\n  offset=3\n  return map([1,2],fn(v):return v+offset)\n", "map",
           "fn map(input: [i64], callback: fn(i64) -> i64) -> [i64]");
    expect("fn main()->[i64] ! TaskError:\n  return try parallelMap([1,2],fn(v):return v*2)\n",
           "parallelMap",
           "fn parallelMap(input: [i64], callback: fn(i64) -> i64) -> [i64] ! TaskError");
    expect("fn length(v:i64):\n  return v+1\nfn main():\n  return length(2)\n", "length(2)",
           "fn length(v: i64) -> i64");
    expect("fn main():\n  length=fn(v:i64):return v+1\n  return length(2)\n", "length(2)",
           "length: fn(i64) -> i64");
    expect("type Box:\n  map:fn(i64)->i64\nfn main():\n  box=Box {map:fn(v:i64):return v+1}\n  "
           "return box.map(2)\n",
           "box.map", "map: fn(i64) -> i64", 4);
    expect("fn main()->[i64]:\n  return sort([2,1])\n", "sort",
           "fn sort(mut input: [i64]) -> [i64]");
    expect("fn main()->[i64]:\n  return sorted([2,1])\n", "sorted",
           "fn sorted(input: [i64]) -> [i64]");
    expect("fn main()->[i64]:\n  return copy([1,2])\n", "copy", "fn copy(input: [i64]) -> [i64]");
    expect("fn main()->[i64]:\n  offset=2\n  return generate(3,fn(i):return i+offset)\n",
           "generate", "fn generate(count: i64, callback: fn(i64) -> i64) -> [i64]");
    require(!query("fn main():\n  return length(42)\n", "length"), "invalid builtin leaked hover");
    require(!query("fn main():\n  length([1],)\n  return missing\n", "length"),
            "later graph errors invalidate builtin hovers");
    require(!query("fn main():\n  return concat([1],)\n", "concat"),
            "signature snapshots are not valid builtin hovers");
    require(!query("fn main():\n  # length([1])\n  return 0\n", "length"),
            "comments cannot create builtin hover targets");
    require(!query("fn main():\n  return length([1])\n", "length", 6),
            "builtin hover excludes the following parenthesis");
    CheckOptions placement;
    placement.placement = true;
    const std::string transfers =
        "fn main()->i64 ! TaskError:\n  return try parallel:\n    values=tensor([1,2])\n"
        "    resident=try await transfer(values,devices.gpu(1))\n"
        "    host=try await transfer(resident,devices.cpu())\n    reduceSum(host)\n";
    expect(transfers, "transfer(values",
           "fn transfer(input: Tensor[i64, 2], target: Device[gpu:1]) -> task<Tensor[i64, 2] "
           "on gpu:1>",
           0, placement);
    expect(transfers, "transfer(resident",
           "fn transfer(input: Tensor[i64, 2] on gpu:1, target: Device[cpu]) -> "
           "task<Tensor[i64, 2]>",
           0, placement);
    require(!query(transfers, "transfer(values"), "CPU mode rejects placement builtin hover");
    require(!query(transfers + "fn broken():\n  return missing\n", "transfer(values", 0, placement),
            "failed graphs suppress transfer hover");
    expect("fn transfer(v:i64):\n  return v+1\nfn main():\n  return transfer(2)\n", "transfer(2)",
           "fn transfer(v: i64) -> i64");
    expect("fn main():\n  target=devices.cpu()\n  return 0\n", "devices.cpu",
           "fn devices.cpu() -> Device[cpu]", 8, placement);
    expect("fn main():\n  target=devices.gpu()\n  return 0\n", "devices.gpu",
           "fn devices.gpu() -> Device[gpu:0]", 0, placement);
    expect("fn main():\n  target=devices.gpu(1)\n  return 0\n", "devices.gpu",
           "fn devices.gpu(index: u32) -> Device[gpu:1]", 8, placement);
    expect("fn main():\n  target=1 -> devices.gpu()\n  return 0\n", "devices.gpu",
           "fn devices.gpu(index: u32) -> Device[gpu:1]", 8, placement);
    expect("fn main():\n  target=1 -> devices.gpu\n  return 0\n", "devices.gpu",
           "fn devices.gpu(index: u32) -> Device[gpu:1]", 0, placement);
    require(!query("fn main():\n  target=devices.gpu(index)\n  return 0\n", "devices.gpu", 0,
                   placement),
            "invalid literal selector suppresses hover");
    require(!query("fn main():\n  target=devices.gpu()\n  return missing\n", "devices.gpu", 0,
                   placement),
            "later errors suppress selector hover");
    require(!query("fn main():\n  target=devices.gpu()\n  return 0\n", "devices.gpu"),
            "placement-disabled selector has no builtin hover");
    std::cout << "Native hover: checked values, signatures, fields, closures, shapes, errors, "
                 "literal safety, source ranges and failure isolation passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
