#include "Hover.h"
#include "Signature.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::optional<lsp::SignatureInfo> query(std::string text, CheckOptions options = {}) {
  const auto cursor = text.rfind('|');
  require(cursor != text.npos, "missing cursor");
  text.erase(cursor, 1);
  return lsp::signatureInfo(parse(std::make_shared<const SourceFile>("signature.k", text)),
                            "signature.k", cursor, options);
}
static std::optional<lsp::SignatureInfo> recover(std::string text) {
  const auto cursor = text.rfind('|');
  require(cursor != text.npos, "missing recovery cursor");
  text.erase(cursor, 1);
  return lsp::signatureAt(
      (std::filesystem::temp_directory_path() / "signature-recovery.k").string(), text, cursor);
}
int main() {
  try {
    const std::string add = "fn add(left:int,right:int)->int:\n  left+right\n";
    auto result = query(add + "fn main():\n  add(|1,2)\n");
    require(result && result->label == "fn add(left: int64, right: int64) -> int64" &&
                result->parameters == std::vector<std::string>{"left: int64", "right: int64"} &&
                result->activeParameter == 0,
            "named signature and first argument");
    result = query(add + "fn main():\n  add(1, |2)\n");
    require(result && result->activeParameter == 1, "second argument");
    result = query(add + "fn main():\n  add(add(1,2), |3)\n");
    require(result && result->activeParameter == 1, "nested commas do not advance outer arguments");
    result = query(add + "fn main():\n  add(1,add(|2,3))\n");
    require(result && result->activeParameter == 0, "innermost call selected");
    result = query(add + "fn main():\n  1 -> add(|2)\n");
    require(result && result->activeParameter == 1, "pipeline supplies first parameter");
    result = query(add + "fn main():\n  callback=add\n  callback(1,|2)\n");
    require(result && result->label == "fn callback(int64, int64) -> int64" &&
                result->activeParameter == 1,
            "function binding uses checked callable type");
    result = query("fn invoke(callback:fn(int,int)->int)->int:\n  callback(1,|2)\n"
                   "fn main():\n  0\n");
    require(result && result->parameters == std::vector<std::string>{"int64", "int64"},
            "function parameter signature");
    result = query("type Box:\n  callback:fn(int)->int\nfn double(n:int)->int:\n  n*2\n"
                   "fn main():\n  box=Box {callback:double}\n  box.callback(|2)\n");
    require(result && result->label == "fn callback(int64) -> int64", "function field signature");
    result = query("fn constant()->int:\n  1\nfn main():\n  constant(|)\n");
    require(result && result->parameters.empty() && !result->activeParameter,
            "zero-parameter signature has no active parameter");
    result = query("fn count(values:[int],text:String)->int:\n  length(values)\n"
                   "fn main():\n  count([1,2,3], |\"a,b\")\n");
    require(result && result->activeParameter == 1, "array/string commas stay nested");
    result = query("error Bad:\n  Failed\nfn risky(n:int)->int ! Bad:\n  n\n"
                   "fn main():\n  risky(|1) catch fn(e):0\n");
    require(result && result->label == "fn risky(n: int64) -> int64 ! Bad",
            "expected-error channel signature");
    result = query("fn main():\n  scale=2\n  callback=fn(n:int):n*scale\n  callback(|3)\n");
    require(result && result->parameters == std::vector<std::string>{"int64"},
            "captured parameters are not public arguments");
    require(!query(add + "fn main():\n  add(|1)\n"), "argument mismatch supplies no partial types");
    require(!query(add + "fn main():\n  add(|\n"), "unfinished call not recovered");
    require(!query(add + "fn main():\n  add(1,2)|\n"), "outside call has no signature");
    require(!query(add + "fn main():\n  ad|d(1,2)\n"), "callee text has no active argument");
    require(!query(add + "fn main():\n  missing\n  add(|1,2)\n"), "invalid graph fails closed");
    result = query("fn main():\n  length(|[1,2])\n");
    require(result && result->label == "fn length(input: [int64]) -> int64" &&
                result->activeParameter == 0,
            "checked array length signature");
    result = query("fn make()->fn(int)->int:\n  fn(n:int):n\nfn main():\n  make()(|1)\n");
    require(result && result->label == "fn(int64) -> int64" && result->activeParameter == 0,
            "factory result signature rather than factory signature");
    result = query("fn make(offset:int)->fn(int,int)->int:\n  fn(a:int,b:int):a+b+offset\n"
                   "fn main():\n  make(3)(1,|2)\n");
    require(result && result->parameters == std::vector<std::string>{"int64", "int64"} &&
                result->activeParameter == 1,
            "captured factory result parameters");
    result = query("fn make(offset:int)->fn(int,int)->int:\n  fn(a:int,b:int):a+b+offset\n"
                   "fn main():\n  make(|3)(1,2)\n");
    require(result && result->label == "fn make(offset: int64) -> (fn(int64, int64) -> int64)",
            "factory arguments still show factory signature");
    result = query(add + "fn main():\n  callbacks=[add]\n  callbacks[0](1,|2)\n");
    require(result && result->label == "fn(int64, int64) -> int64" && result->activeParameter == 1,
            "indexed callable signature");
    result = query(add + "fn main():\n  callbacks=[add]\n  1 -> callbacks[0](|2)\n");
    require(result && result->activeParameter == 1, "indexed pipeline implicit parameter");
    result = query("fn make()->fn(int,int)->int:\n  fn(a:int,b:int):a+b\n"
                   "fn main():\n  1 -> make()(|2)\n");
    require(result && result->activeParameter == 1, "factory pipeline checked invocation span");
    result = query("fn main():\n  (fn(n:int):n*2)(|3)\n");
    require(result && result->label == "fn(int64) -> int64", "direct anonymous call signature");
    result = query("fn make()->[fn(int)->int]:\n  [fn(n:int):n]\n"
                   "fn main():\n  make()[0](|3)\n");
    require(result && result->label == "fn(int64) -> int64", "indexed factory result signature");
    result = query("error Bad:\n  Failed\nfn make()->fn(int)->int ! Bad:\n  fn(n:int):n\n"
                   "fn main():\n  make()(|3) catch fn(e):0\n");
    require(result && result->label == "fn(int64) -> int64 ! Bad",
            "returned callable error channel is retained");
    require(!query(add + "fn main():\n  callbacks=[add]\n  callbacks[0](|1)\n"),
            "invalid indexed arguments do not leak signature");
    require(!query(add + "fn main():\n  callbacks=[add]\n  callbacks[2](|1,2)\n"),
            "invalid index fails whole graph");
    require(!query("fn main():\n  numbers=[1]\n  numbers[0](|2)\n"),
            "indexed numeric value is not callable");
    require(!query("fn make()->int:\n  1\nfn main():\n  make()(|2)\n"),
            "numeric factory result does not suggest factory signature");
    result = recover(add + "fn main():\n  add(1,2|\n");
    require(result && result->activeParameter == 1, "recover missing closing parenthesis");
    result = recover(add + "fn main():\n  add(1,add(2,3|\n");
    require(result && result->activeParameter == 1, "recover nested trailing parentheses");
    result = recover("fn constant()->int:\n  1\nfn main():\n  constant(|\n");
    require(result && result->parameters.empty(), "empty zero-parameter call recovery");
    result = recover("fn make()->fn(int,int)->int:\n  fn(a:int,b:int):a+b\n"
                     "fn main():\n  make()(1,2|\n");
    require(result && result->label == "fn(int64, int64) -> int64", "factory-result recovery");
    result = recover(add + "fn main():\n  callbacks=[add]\n  1 -> callbacks[0](2|\n");
    require(result && result->activeParameter == 1, "indexed pipeline recovery");
    result = recover(add + "fn main():\n  add(1,2|)\n");
    require(result && result->activeParameter == 1, "ordinary valid calls retain signature");
    result = recover(add + "fn main():\n  add(|\n");
    require(result && result->activeParameter == 0, "first missing argument signature");
    result = recover(add + "fn main():\n  add(1,|\n");
    require(result && result->activeParameter == 1, "next missing argument signature");
    result = query(add + "fn main():\n  add(1,|)\n");
    require(result && result->activeParameter == 1, "balanced incomplete call signature");
    result = recover(add + "fn main():\n  callback=add\n  callback(1,|\n");
    require(result && result->label == "fn callback(int64, int64) -> int64",
            "bound callable missing arguments");
    result = recover("fn make()->fn(int,int)->int:\n  fn(a:int,b:int):a+b\n"
                     "fn main():\n  make()(1,|\n");
    require(result && result->label == "fn(int64, int64) -> int64", "factory missing argument");
    result = recover(add + "fn main():\n  callbacks=[add]\n  1 -> callbacks[0](|\n");
    require(result && result->activeParameter == 1, "indexed pipeline missing argument");
    require(!recover(add + "fn main():\n  add(\"bad\",|\n"),
            "invalid prefix argument type blocks snapshot");
    require(!recover(add + "fn main():\n  add(missing,|\n"), "invalid prefix name blocks snapshot");
    require(bool(recover(add + "fn broken()->int:\n  missing\nfn main():\n  add(1,|\n")),
            "independent broken body does not erase checked signature");
    require(!recover("fn broken(n:int)->int:\n  missing\nfn main():\n  broken(|\n"),
            "invalid target body blocks named signature snapshot");
    result = recover("type Point:\n  x:int\nfn inspect(point:Point,n:int)->int:\n  point.x+n\n"
                     "fn main():\n  inspect(Point {x:1},|\n");
    require(result && result->parameters == std::vector<std::string>{"point: Point", "n: int64"},
            "nominal parameter snapshot");
    result = recover("error First:\n  Bad\nerror Second:\n  Bad\n"
                     "fn risky(a:int,b:int)->int ! (First | Second):\n  a+b\n"
                     "fn main():\n  risky(1,|\n");
    require(result && result->label == "fn risky(a: int64, b: int64) -> int64 ! First | Second",
            "error union snapshot display metadata");
    require(!query("fn main(n:int)->int:\n  main(|)\n"),
            "incomplete recursive callee does not snapshot an unfinished definition");
    require(!query(add + "fn main():\n  add(1,|)\nfn later()->int:\n  missing\n"),
            "later reported graph error invalidates an already captured snapshot");
    const std::string missingCall = add + "fn main():\n  add(1,)\n";
    auto syntax = parse(std::make_shared<const SourceFile>("snapshot.k", missingCall));
    SourceSpan callSite{};
    for (const auto &expression : syntax.module.expressions)
      if (std::holds_alternative<CallExpr>(expression.value))
        callSite = expression.span;
    CallableSignatureQuery snapshot{callSite, {}};
    CheckOptions options;
    options.signature = &snapshot;
    auto checked = check(std::move(syntax), {}, options);
    require(!checked.ok() && !checked.module && snapshot.snapshot &&
                snapshot.snapshot->parameterNames == std::vector<std::string>{"left", "right"},
            "snapshot never returns a partial typed module");
    (void)check(parse(std::make_shared<const SourceFile>("snapshot.k", "fn main():\n  0\n")), {},
                options);
    require(!snapshot.snapshot, "query snapshot resets between checks");
    auto ordinary = check(parse(std::make_shared<const SourceFile>("snapshot.k", missingCall)));
    require(!ordinary.ok() && !ordinary.module && ordinary.diagnostics.size() == 1 &&
                ordinary.diagnostics.front().code == "S027",
            "ordinary compiler keeps its missing-argument diagnostic");
    require(!recover(add + "fn main():\n  add(1,2|\n  missing\n"), "later code is not hidden");
    require(!recover(add + "fn main():\n  missing\n  add(1,2|\n"), "earlier errors fail closed");
    require(!recover(add + "fn main():\n  add(1,\"text\"|\n"), "argument types must check");
    require(!recover(add + "fn main():\n  add([1,2|\n"), "unclosed array is not recovered");
    require(!recover(add + "fn main():\n  add(1,{|\n"), "unclosed record is not recovered");
    require(!recover(add + "fn main():\n  add(1,2]|\n"), "mismatched delimiters fail closed");
    require(!recover(add + "fn main():\n  add(1,\"unfinished|"),
            "unclosed string is not recovered");
    require(!recover(add + "fn main():\n  add(1,2 # comment|"),
            "comment does not receive delimiters");
    require(!recover(add + "fn main():\n  ((((((((add(1,2|\n"),
            "recovery has an eight-delimiter bound");
    result = query(add + "fn main():\n  add(1, va|Suffix)\n");
    require(result && result->activeParameter == 1 &&
                result->parameters == std::vector<std::string>{"left: int64", "right: int64"},
            "final partial identifier retains named parameter guidance");
    result = recover(add + "fn main():\n  add(1, val|\n");
    require(result && result->activeParameter == 1, "partial name with trailing open call");
    result = query("fn single(value:int)->int:\n  value\nfn main():\n  single(va|Suffix)\n");
    require(result && result->activeParameter == 0, "first and final bare identifier");
    result = recover("fn make()->fn(int,int)->int:\n  fn(a:int,b:int):a+b\n"
                     "fn main():\n  make()(1, val|\n");
    require(result && result->label == "fn(int64, int64) -> int64",
            "factory argument editing hole");
    result = recover(add + "fn main():\n  callbacks=[add]\n  1 -> callbacks[0](val|\n");
    require(result && result->activeParameter == 1, "indexed pipeline argument editing hole");
    result = query(add + "fn main():\n  value=2\n  add(1, val|ue)\n");
    require(result && result->activeParameter == 1, "resolved name still has parameter guidance");
    result = query(add + "fn main():\n  value=\"bad\"\n  add(1, val|ue)\n");
    require(result && result->activeParameter == 1,
            "active editing hole is not claimed to have a valid type");
    require(!query(add + "fn main():\n  add(missing, va|Suffix)\n"),
            "earlier invalid argument still blocks guidance");
    result = query(add + "fn main():\n  add(va|Suffix, 2)\n");
    require(result && result->activeParameter == 0,
            "earlier partial argument retains parameter guidance");
    require(!query(add + "fn main():\n  add(1, 2 + va|Suffix)\n"),
            "partial expression is not a bare-name editing hole");
    const std::string records = "type Reading:\n  value:int\n"
                                "type Box:\n  reading:Reading\n";
    result = query(add + records +
                   "fn main():\n  reading=Reading {value:2}\n  add(1, reading.va|Suffix)\n");
    require(result && result->activeParameter == 1, "final field identifier retains guidance");
    result =
        recover(add + records +
                "fn main():\n  box=Box {reading:Reading {value:2}}\n  add(1, box.reading.va|\n");
    require(result && result->activeParameter == 1, "nested receiver with open trailing call");
    result = query(add + records +
                   "fn make()->Reading:\n  Reading {value:2}\n"
                   "fn main():\n  add(1, make().va|Suffix)\n");
    require(result && result->activeParameter == 1, "checked factory-result receiver");
    result = query(add + records +
                   "fn main():\n  values=[Reading {value:2}]\n  1 -> add(values[0].va|Suffix)\n");
    require(result && result->activeParameter == 1, "indexed receiver in pipeline");
    result = query(add + records +
                   "fn make()->fn(int,int)->int:\n  add\n"
                   "fn main():\n  reading=Reading {value:2}\n  make()(1, reading.va|Suffix)\n");
    require(result && result->label == "fn(int64, int64) -> int64",
            "structural callee field editing");
    result = query(add + "type Wrong:\n  value:String\nfn main():\n  reading=Wrong "
                         "{value:\"text\"}\n  add(1, reading.val|ue)\n");
    require(result && result->activeParameter == 1,
            "active field is not claimed to have expected type");
    require(!query(add + records +
                   "fn main():\n  reading=Reading {value:2}\n  add(missing, reading.va|Suffix)\n"),
            "invalid earlier argument blocks field guidance");
    result = query(add + records +
                   "fn main():\n  reading=Reading {value:2}\n  add(reading.va|Suffix, 1)\n");
    require(result && result->activeParameter == 0,
            "earlier field argument receives checked guidance");
    require(
        !query(add + records +
               "fn main():\n  reading=Reading {value:2}\n  add(1, reading.missing.va|Suffix)\n"),
        "invalid intermediate field receiver blocks guidance");
    require(!query(add + "fn main():\n  n=1\n  add(1, n.va|Suffix)\n"),
            "numeric receiver rejected");
    require(!query(add + records +
                   "fn main():\n  values=[Reading {value:2}]\n  add(1, values[2].va|Suffix)\n"),
            "invalid indexed receiver rejected");
    require(!query(add + "enum Choice:\n  Some {value:int}\nfn main():\n  choice=Choice.Some "
                         "{value:2}\n  add(1, choice.va|Suffix)\n"),
            "enum payload requires narrowing rather than arbitrary field access");
    result =
        recover(add + records + "fn main():\n  reading=Reading {value:2}\n  add(1, reading.|\n");
    require(result && result->activeParameter == 1, "empty field and trailing call recovery");
    result =
        recover(add + records + "fn main():\n  reading=Reading {value:2}\n  add(1, reading.|)\n");
    require(result && result->activeParameter == 1, "balanced empty field slot");
    result =
        recover(add + records +
                "fn main():\n  box=Box {reading:Reading {value:2}}\n  1 -> add(box.reading.|)\n");
    require(result && result->activeParameter == 1, "nested empty field in pipeline");
    result = recover(add + records +
                     "fn make()->Reading:\n  Reading {value:2}\nfn main():\n  add(1, make().|)\n");
    require(result && result->activeParameter == 1, "empty field on factory result");
    result = recover(add + records +
                     "fn main():\n  values=[Reading {value:2}]\n  add(1, values[0].|)\n");
    require(result && result->activeParameter == 1, "empty field on indexed record");
    result = recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  callbacks=[add]\n  "
                     "callbacks[0](1, reading.|)\n");
    require(result && result->label == "fn(int64, int64) -> int64",
            "empty field with structural callee");
    require(!recover(add + "fn main():\n  add(1, unknown.|)\n"),
            "unknown empty-field receiver rejected");
    require(!recover(add + "fn main():\n  n=1\n  add(1, n.|)\n"),
            "non-record empty-field receiver rejected");
    require(!recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  add(missing, reading.|)\n"),
            "empty field preserves earlier argument errors");
    result =
        recover(add + records + "fn main():\n  reading=Reading {value:2}\n  add(reading.|, 1)\n");
    require(result && result->activeParameter == 0,
            "earlier empty field receives checked guidance");
    require(bool(recover(add + records +
                         "fn main():\n  reading=Reading {value:2}\n  add(1, reading.|unknown)\n")),
            "checked field prefix retains original suffix");
    require(bool(recover(add + records +
                         "fn main():\n  reading=Reading {value:2}\n  add(1, reading.| + 2)\n")),
            "compound empty field receives checked guidance");
    require(
        bool(recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  add(1, reading.|)\nfn later():\n  "
                     "missing\n")),
        "independent later errors preserve empty-field guidance");
    require(!recover(add + "fn main():\n  add(1, \"reading.|\"\n"),
            "string content is not a field slot");
    const std::string collision = "type Probe:\n  __kalandra_signature_field:int\n"
                                  "fn main():\n  reading=Probe {__kalandra_signature_field:2}\n";
    result = recover(add + collision + "  add(1, reading.|)\n");
    require(result && result->activeParameter == 1,
            "probe-name collision still uses receiver projection");
    require(bool(recover(add + collision + "  add(1, (reading.|))\n")),
            "grouped field query captures a checked signature without supplying a value");
    require(!recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  ((((((((add(1, reading.|\n"),
            "empty-field delimiter recovery keeps eight-parenthesis limit");
    require(!query(add + "fn main():\n  add(1, box.va|Suffix)\n"),
            "unknown field receiver blocks signature guidance");
    require(!query(add + "fn main():\n  add(1, 2, va|Suffix)\n"),
            "extra partial argument does not invent another parameter");
    require(!query("fn main():\n  missing(va|Suffix)\n"), "unknown callee has no guidance");
    require(!query(add + "fn main():\n  add=1\n  add(1, va|Suffix)\n"),
            "noncallable lexical shadow has no guidance");
    result = query("fn main():\n  text=\"hello\"\n  length(te|xt)\n");
    require(result && result->label == "fn length(input: String) -> int64",
            "checked builtin argument is not mistaken for an editing hole");
    result = query("fn main():\n  byteLength(|\"😀\")\n");
    require(result && result->label == "fn byteLength(input: String) -> int64",
            "String byte length signature");
    result = query("fn main():\n  shape(|tensor([1,2]))\n");
    require(result && result->label.find("fn shape(input: Tensor[int64, 2])") == 0 &&
                result->parameters.size() == 1,
            "tensor shape specialization");
    result = query("fn main():\n  slice([1,2,3],0,|2)\n");
    require(result &&
                result->label == "fn slice(input: [int64], start: int64, end: int64) -> [int64]" &&
                result->activeParameter == 2,
            "array slice checked bounds parameters");
    result = query("fn main():\n  \"hello\" -> slice(0,|2)\n");
    require(result &&
                result->label == "fn slice(input: String, start: int64, end: int64) -> String" &&
                result->activeParameter == 2,
            "String slice pipeline active parameter");
    result = query("fn main():\n  concat([true],|[false])\n");
    require(result && result->label == "fn concat(left: [Bool], right: [Bool]) -> [Bool]" &&
                result->activeParameter == 1,
            "Bool array concat signature");
    result = query("fn main():\n  concat([\"a\"],|[\"b\"])\n");
    require(result && result->label == "fn concat(left: [String], right: [String]) -> [String]",
            "String array concat signature");
    require(!query("fn main():\n  concat(\"a\",|\"b\")\n"),
            "unsupported scalar String concat has no guidance");
    result =
        query("type Box:\n  value:int\nfn main():\n  concat([Box {value:1}],|[Box {value:2}])\n");
    require(result && result->label == "fn concat(left: [Box], right: [Box]) -> [Box]",
            "nominal array context remains available to the type printer");
    result = recover("fn main():\n  values=[1,2]\n  length(values|\n");
    require(result && result->label == "fn length(input: [int64]) -> int64",
            "trailing parenthesis recovery checks complete builtin arguments");
    require(!query("fn main():\n  length(|)\n"), "empty builtin has no guessed specialization");
    require(!query("fn main():\n  slice([1,2],|0)\n"),
            "missing builtin argument has no guessed type");
    require(!query("fn main():\n  length(|42)\n"), "invalid builtin input has no signature");
    require(!query("fn main():\n  missing\n  length(|[1,2])\n"),
            "prior errors block builtin guidance");
    require(!query("fn main():\n  length(|[1,2])\n  missing\n"),
            "later errors block builtin guidance");
    require(!query("fn main():\n  reshape(tensor([1,2]),|[2])\n"),
            "other builtins remain unsupported");
    result = query("fn main():\n  length=fn(v:int):v*2\n  length(|3)\n");
    require(result && result->label == "fn length(int64) -> int64",
            "lexical callable shadow is not a builtin");
    result = recover("fn main():\n  concat([1,2],|\n");
    require(result && result->label == "fn concat(left: [int64], right: [int64]) -> [int64]" &&
                result->activeParameter == 1,
            "missing concat operand from first array");
    result = query("fn main():\n  [true] -> concat(|)\n");
    require(result && result->label == "fn concat(left: [Bool], right: [Bool]) -> [Bool]" &&
                result->activeParameter == 1,
            "piped missing concat operand");
    result = recover("fn main():\n  concat([\"a\"], ri|Suffix)\n");
    require(result && result->label == "fn concat(left: [String], right: [String]) -> [String]",
            "partly typed right operand uses first array");
    result = query("fn main()->[int32]:\n  concat([],|)\n");
    require(result && result->label == "fn concat(left: [int32], right: [int32]) -> [int32]",
            "expected array context can type an empty first operand");
    result = query("type Box:\n  value:int\nfn main():\n  concat([Box {value:1}],|)\n");
    require(result && result->label == "fn concat(left: [Box], right: [Box]) -> [Box]",
            "nominal array snapshot context");
    result = query("fn main():\n  callbacks=[fn(v:int):v*2]\n  concat(callbacks,|)\n");
    require(result && result->label == "fn concat(left: [fn(int64) -> int64], right: [fn(int64) -> "
                                       "int64]) -> ([fn(int64) -> int64])",
            "callable array contract survives the snapshot");
    result =
        recover("type Box:\n  value:int\nfn main():\n  box=Box {value:1}\n  concat([1], box.|)\n");
    require(result && result->activeParameter == 1, "missing field still checks its receiver");
    require(!recover("fn main():\n  concat([1], missing.|)\n"),
            "unknown projected field receiver prevents a snapshot");
    require(!query("fn main():\n  concat(|)\n"), "no first array means no element contract");
    require(!query("fn main():\n  concat([],|)\n"),
            "uncontextual empty array has no element contract");
    require(!query("fn main():\n  concat(42,|)\n"), "scalar does not establish array contract");
    require(!query("fn main():\n  missing\n  concat([1],|)\n"),
            "prior errors prevent concat snapshots");
    require(!query("fn main():\n  concat([1],2,|)\n"), "extra arguments do not receive a snapshot");
    require(!recover("fn main():\n  concat([1], unknown + 1|)\n"),
            "compound right operand does not receive expression recovery");
    for (const auto &source : numericTypes)
      for (const auto &destination : numericTypes) {
        const auto from = std::string(source.name), to = std::string(destination.name);
        result = query("fn main():\n  " + to + "(|" + from + "(1))\n");
        require(result && result->label == "fn " + to + "(input: " + from + ") -> " + to &&
                    result->activeParameter == 0,
                "conversion signature pair " + from + " to " + to);
      }
    result = query("fn main():\n  float(|1)\n");
    require(result && result->label == "fn float(input: int64) -> float64",
            "conversion alias keeps its surface name and concrete result");
    result = query("fn main():\n  uint(|1)\n");
    require(result && result->label == "fn uint(input: int64) -> uint64", "uint conversion alias");
    result = query("fn main():\n  int(|float32(1))\n");
    require(result && result->label == "fn int(input: float32) -> int64", "int conversion alias");
    result = query("fn main():\n  int16(|tensor([1,2],[1,2]))\n");
    require(result &&
                result->label == "fn int16(input: Tensor[int64, 1, 2]) -> Tensor[int16, 1, 2]",
            "tensor conversion retains rank and fixed extents");
    result =
        query("fn convert(values:Tensor[float32,?,2]):\n  float64(va|lues)\nfn main():\n  0\n");
    require(result && result->label ==
                          "fn float64(input: Tensor[float32, ?, 2]) -> Tensor[float64, ?, 2]",
            "dynamic conversion shapes hide inference identities");
    result = query("fn main():\n  1 -> float32(|)\n");
    require(result && result->label == "fn float32(input: int64) -> float32" &&
                result->activeParameter == 0,
            "piped scalar conversion signature");
    result = recover("fn main():\n  value=1\n  uint8(value|\n");
    require(result && result->label == "fn uint8(input: int64) -> uint8",
            "conversion guidance after bounded delimiter repair");
    result = query("fn main():\n  float32=fn(v:int):v+1\n  float32(|2)\n");
    require(result && result->label == "fn float32(int64) -> int64",
            "same-spelled lexical function is not numeric conversion");
    require(!query("fn main():\n  int8(|300)\n"), "out-of-range conversion has no checked hint");
    require(!query("fn main():\n  float32(|[1,2])\n"), "numeric arrays remain unsupported casts");
    require(!query("fn main():\n  int64(|true)\n"), "Bool is not a numeric source");
    require(!query("fn main():\n  float32(|)\n"), "missing conversion source has no guessed type");
    require(!query("fn main():\n  float32(|1,2)\n"), "extra conversion arguments have no hint");
    require(!query("fn main():\n  float32(|1)\n  missing\n"),
            "failed graphs suppress numeric hints");
    for (const auto &numeric : numericTypes) {
      const auto name = std::string(numeric.name);
      const auto matrix = "tensor([" + name + "(1)],[1,1])";
      result = query("fn main():\n  reduceSum(|" + matrix + ")\n");
      require(result &&
                  result->label == "fn reduceSum(input: Tensor[" + name + ", 1, 1]) -> " + name,
              "reduction signature for " + name);
      result = query("fn main():\n  matmul(" + matrix + ",|" + matrix + ")\n");
      require(result &&
                  result->label == "fn matmul(left: Tensor[" + name + ", 1, 1], right: Tensor[" +
                                       name + ", 1, 1]) -> Tensor[" + name + ", 1, 1]" &&
                  result->activeParameter == 1,
              "matrix signature for " + name);
      result = query("fn main():\n  array(|" + matrix + ")\n");
      require(result &&
                  result->label == "fn array(input: Tensor[" + name + ", 1, 1]) -> [" + name + "]",
              "tensor flattening signature for " + name);
    }
    result = query("fn multiply(a:Tensor[float32,?,3],b:Tensor[float32,3,?]):\n"
                   "  matmul(a,|b)\nfn main():\n  0\n");
    require(result && result->label == "fn matmul(left: Tensor[float32, ?, 3], right: "
                                       "Tensor[float32, 3, ?]) -> Tensor[float32, ?, ?]",
            "dynamic matrix result derives outer axes");
    result = query("fn main():\n  tensor([1,2],[1,2]) -> matmul(|tensor([3,4],[2,1]))\n");
    require(result && result->activeParameter == 1 &&
                result->label == "fn matmul(left: Tensor[int64, 1, 2], right: Tensor[int64, 2, 1]) "
                                 "-> Tensor[int64, 1, 1]",
            "piped matrix call tracks explicit right operand");
    result = query("fn main():\n  relu(|tensor([-1,2]))\n");
    require(result && result->label == "fn relu(input: Tensor[int64, 2]) -> Tensor[int64, 2]",
            "ReLU tensor signature");
    result = query("fn main():\n  reduceSum(|tensor([7],[]))\n");
    require(result && result->label == "fn reduceSum(input: Tensor[int64]) -> int64",
            "rank-zero reduction signature");
    result = query("fn main():\n  values:Tensor[float32,0]=tensor([])\n  array(|values)\n");
    require(result && result->label == "fn array(input: Tensor[float32, 0]) -> [float32]",
            "empty tensor array result");
    result = recover("fn main():\n  value=tensor([1,2])\n  reduceSum(value|\n");
    require(result && result->label == "fn reduceSum(input: Tensor[int64, 2]) -> int64",
            "reduction guidance after bounded delimiter recovery");
    result = query("fn main():\n  matmul=fn(v:int):v+1\n  matmul(|1)\n");
    require(result && result->label == "fn matmul(int64) -> int64",
            "lexical matrix-name shadow remains an ordinary callable");
    require(!query("fn main():\n  matmul(tensor([1],[1,1]),|tensor([1,2],[2,1]))\n"),
            "incompatible matrix inner extents have no signature");
    require(!query("fn main():\n  matmul(tensor([1],[1,1]),|tensor([float32(1)],[1,1]))\n"),
            "mixed matrix element types have no signature");
    require(!query("fn main():\n  reduceSum(|[1,2])\n"), "arrays are not reduction tensors");
    require(!query("fn main():\n  array(|[1,2])\n"), "array conversion requires a tensor");
    require(!query("fn main():\n  matmul(tensor([1],[1,1]),|)\n"),
            "missing matrix operand has no guessed shape");
    require(!query("fn main():\n  reduceSum(|tensor([1]))\n  missing\n"),
            "failed graphs suppress tensor signatures");
    const auto mapLabel = "fn map(input: [int64], callback: fn(int64) -> int64) -> [int64]";
    result = query("fn double(v:int):\n  v*2\nfn main():\n  map([1,2],|double)\n");
    require(result && result->label == mapLabel && result->activeParameter == 1,
            "named map callback signature");
    result = query("fn main():\n  map([1,2],|fn(v):v*2)\n");
    require(result && result->label == mapLabel, "inferred anonymous map signature");
    result = query("fn main():\n  offset=3\n  map([1,2],|fn(v):v+offset)\n");
    require(result && result->label == mapLabel, "inline captures stay out of public callback");
    result =
        query("fn main():\n  offset=3\n  callback=fn(v:int):v+offset\n  map([1,2],|callback)\n");
    require(result && result->label == mapLabel, "retained function wrapper target stays hidden");
    result = query("fn main():\n  callbacks=[fn(v:int):v*2]\n  map([1,2],|callbacks[0])\n");
    require(result && result->label == mapLabel, "indexed callback retains public signature");
    result = query("fn main():\n  [1,2] -> map(|fn(v):v*2)\n");
    require(result && result->label == mapLabel && result->activeParameter == 1,
            "piped map selects explicit callback");
    result = query("fn main():\n  map([1,2],|fn(v):v>0)\n");
    require(result &&
                result->label == "fn map(input: [int64], callback: fn(int64) -> Bool) -> [Bool]",
            "map result element differs from input");
    result = query("type Box:\n  value:int\nfn main():\n  map([Box {value:2}],|fn(v):v.value)\n");
    require(result &&
                result->label == "fn map(input: [Box], callback: fn(Box) -> int64) -> [int64]",
            "nominal callback input context");
    for (const auto &numeric : numericTypes) {
      const auto name = std::string(numeric.name);
      result = query("fn main():\n  map([" + name + "(1)],|fn(v):v)\n");
      require(result && result->label == "fn map(input: [" + name + "], callback: fn(" + name +
                                             ") -> " + name + ") -> [" + name + "]",
              "map element width " + name);
    }
    result = query("fn main()->[int] ! TaskError:\n  try parallelMap([1,2],|fn(v):v*2)\n");
    require(result && result->label == "fn parallelMap(input: [int64], callback: fn(int64) -> "
                                       "int64) -> [int64] ! TaskError",
            "parallel map exposes scheduling error channel only on outer result");
    result = query("error Bad:\n  Failed\nfn checked(v:int)->int ! Bad:\n  v\n"
                   "fn main()->[int] ! (Bad | TaskError):\n  try parallelMap([1,2],|checked)\n");
    require(result && result->label == "fn parallelMap(input: [int64], callback: fn(int64) -> "
                                       "int64 ! Bad) -> [int64] ! Bad | TaskError",
            "callback application errors remain separate from task errors");
    result = recover("fn main():\n  map([1,2],fn(v:int):v*2|\n");
    require(result && result->label == mapLabel, "complete callback after bounded recovery");
    result = query("fn main():\n  map=fn(v:int):v*2\n  map(|2)\n");
    require(result && result->label == "fn map(int64) -> int64", "same-spelled map binding");
    require(!query("fn main():\n  map([1],|)\n"), "missing callback has no guessed signature");
    require(!query("fn main():\n  map([1],|fn(v:int,w:int):v+w)\n"), "invalid callback arity");
    require(!query("fn main():\n  parallelMap([1],|fn(v:int):v*2)\n"),
            "unhandled task errors block hints");
    require(!query("fn main():\n  map([1],|fn(v:String):v)\n"), "incompatible map callback input");
    CheckOptions placement;
    placement.placement = true;
    const std::string transferStart = "fn main()->int ! TaskError:\n  try parallel:\n"
                                      "    values=tensor([1,2])\n";
    const std::string transferEnd =
        "\n    host=try await transfer(resident,devices.cpu())\n    reduceSum(host)\n";
    result = query(transferStart + "    resident=try await transfer(values,|devices.gpu(1))" +
                       transferEnd,
                   placement);
    require(result &&
                result->label == "fn transfer(input: Tensor[int64, 2], target: Device[gpu:1]) -> "
                                 "task<Tensor[int64, 2] on gpu:1>" &&
                result->activeParameter == 1 && result->parameters.size() == 2,
            "transfer exposes checked shape, exact device and task result");
    result = query(transferStart + "    resident=try await (values -> transfer(|devices.gpu(1)))" +
                       transferEnd,
                   placement);
    require(result && result->activeParameter == 1 &&
                result->parameters[0] == "input: Tensor[int64, 2]",
            "piped transfer selects device argument");
    result =
        query(transferStart +
                  "    resident=try await transfer(values,devices.gpu(1))\n"
                  "    host=try await transfer(resident,|devices.cpu())\n    reduceSum(host)\n",
              placement);
    require(result && result->label == "fn transfer(input: Tensor[int64, 2] on gpu:1, target: "
                                       "Device[cpu]) -> task<Tensor[int64, 2]>",
            "return transfer preserves input residency and CPU result");
    result = query("fn move(values:Tensor[float32,?,2])->int ! TaskError:\n  try parallel:\n"
                   "    resident=try await transfer(values,|devices.gpu())\n"
                   "    host=try await transfer(resident,devices.cpu())\n    0\nfn main():\n  0\n",
                   placement);
    require(result && result->label == "fn transfer(input: Tensor[float32, ?, 2], target: "
                                       "Device[gpu:0]) -> task<Tensor[float32, ?, 2] on gpu:0>",
            "dynamic transfer shapes hide private extent identities");
    require(!query("fn main():\n  transfer(tensor([1]),|devices.gpu())\n", placement),
            "transfer outside an owning group cannot offer a signature");
    require(!query(transferStart + "    resident=try await transfer(values,|1)" + transferEnd,
                   placement),
            "invalid transfer target has no guessed signature");
    require(!query(transferStart + "    resident=try await transfer(values,|devices.gpu(1))" +
                   transferEnd),
            "placement-disabled transfer has no builtin signature");
    result = query("fn transfer(value:int):\n  value+1\nfn main():\n  transfer(|2)\n");
    require(result && result->label == "fn transfer(value: int64) -> int64",
            "CPU-mode source function retains ordinary transfer signature");
    result = query("fn main():\n  target=devices.cpu(|)\n  0\n", placement);
    require(result && result->label == "fn devices.cpu() -> Device[cpu]" &&
                result->parameters.empty() && !result->activeParameter,
            "CPU selector has no parameters");
    result = query("fn main():\n  target=devices.gpu(|)\n  0\n", placement);
    require(result && result->label == "fn devices.gpu() -> Device[gpu:0]" &&
                result->parameters.empty() && !result->activeParameter,
            "omitted GPU index describes checked default selection");
    result = query("fn main():\n  target=devices.gpu(|1)\n  0\n", placement);
    require(result && result->label == "fn devices.gpu(index: uint32) -> Device[gpu:1]" &&
                result->parameters == std::vector<std::string>{"index: uint32"} &&
                result->activeParameter == 0,
            "explicit selector index uses its checked literal contract");
    result = query("fn main():\n  target=devices.gpu(|4294967295)\n  0\n", placement);
    require(result && result->label.ends_with("Device[gpu:4294967295]"),
            "maximum selector index does not require available hardware");
    result = query("fn main():\n  target=1 -> devices.gpu(|)\n  0\n", placement);
    require(result && result->label == "fn devices.gpu(index: uint32) -> Device[gpu:1]",
            "pipeline supplies the checked selector literal");
    for (const auto *argument : {"1,2", "-1", "1.5", "4294967296", "uint32(1)", "index"})
      require(!query("fn main():\n  index=1\n  target=devices.gpu(|" + std::string(argument) +
                         ")\n  0\n",
                     placement),
              "invalid selector has no guessed signature");
    require(!query("fn main():\n  target=devices.cpu(|1)\n  0\n", placement),
            "CPU selector rejects extra arguments");
    require(!query("fn main():\n  devices=1\n  target=devices.gpu(|1)\n  0\n", placement),
            "shadowed devices namespace has no selector signature");
    require(!query("fn main():\n  target=devices.gpu(|1)\n  0\n"),
            "selector signature requires placement checking");
    result = recover(add + "fn main():\n  value=2\n  add(1,va|Suffix\n");
    require(result && result->label == "fn add(left: int64, right: int64) -> int64" &&
                result->activeParameter == 1,
            "final name suffix receives validated callee snapshot before EOF closure");
    result = recover(add + "fn main():\n  value=2\n  callback=add\n  callback(1,va|Suffix\n");
    require(result && result->label == "fn callback(int64, int64) -> int64" &&
                result->activeParameter == 1,
            "function-value callee snapshot survives final argument suffix");
    result = recover(add + "fn main():\n  callbacks=[add]\n  1 -> callbacks[0](va|Suffix\n");
    require(result && result->activeParameter == 1 && result->parameters.size() == 2,
            "indexed pipeline callable preserves implicit first argument");
    const std::string fieldPrefix =
        "type Reading:\n  value:int\n" + add + "fn main():\n  reading=Reading {value:2}\n";
    result = recover(fieldPrefix + "  add(1,reading.va|Suffix\n");
    require(result && result->activeParameter == 1 && result->label.starts_with("fn add("),
            "record field suffix requires checked receiver projection");
    result = recover(fieldPrefix + "  concat([int32(1)],reading.va|Suffix\n");
    require(result && result->label == "fn concat(left: [int32], right: [int32]) -> [int32]",
            "existing concat snapshot supports a projected final field suffix");
    require(!recover(add + "fn main():\n  add(\"bad\",va|Suffix\n"),
            "bad prefix argument suppresses EOF suffix snapshot");
    require(!recover(add + "fn main():\n  add(1,unknown.va|Suffix\n"),
            "unknown receiver suppresses EOF suffix snapshot");
    require(!recover(add + "fn main():\n  missing(1,va|Suffix\n"),
            "unknown callee suppresses EOF suffix snapshot");
    require(!recover(add + "fn main():\n  callback=1\n  callback(1,va|Suffix\n"),
            "noncallable callee suppresses EOF suffix snapshot");
    result = recover(add + "fn main():\n  add(va|Suffix,2\n");
    require(result && result->activeParameter == 0,
            "earlier argument suffix receives EOF signature repair");
    require(bool(recover(add + "fn main():\n  value=2\n  add(1,value+va|Suffix\n")),
            "compound final argument receives checked signature guidance");
    require(!recover(add + "fn main():\n  add(1,[va|Suffix\n"),
            "open arrays are not repaired for signature suffix queries");
    require(!recover(add + "fn main():\n  ((((((((add(1,va|Suffix\n"),
            "signature suffix repair retains eight-parenthesis bound");
    const std::string triple = "fn triple(first:int,second:int,third:int):\n  first+second+third\n";
    result = recover(triple + "fn main():\n  triple(va|Suffix,2\n");
    require(result &&
                result->label == "fn triple(first: int64, second: int64, third: int64) -> int64" &&
                result->activeParameter == 0,
            "earlier slot with missing later required parameter");
    result = query(triple + "fn main():\n  triple(1,va|Suffix,missing)\n");
    require(result && result->activeParameter == 1,
            "later invalid arguments do not change prefix-only contract");
    result =
        recover(triple + "fn main():\n  callbacks=[triple]\n  1 -> callbacks[0](va|Suffix,3\n");
    require(result && result->activeParameter == 1 && result->parameters.size() == 3,
            "indexed callable counts the implicit pipeline argument in earlier slot");
    result = recover(triple + "type Reading:\n  value:int\nfn main():\n"
                              "  reading=Reading {value:1}\n  triple(reading.va|Suffix,2\n");
    require(result && result->activeParameter == 0,
            "earlier field slot checks receiver before snapshot");
    result = query("error Bad:\n  Failed\nfn risky(first:int,second:int)->int ! Bad:\n"
                   "  first+second\nfn main()->int ! Bad:\n  try risky(va|Suffix,2)\n");
    require(result && result->label == "fn risky(first: int64, second: int64) -> int64 ! Bad",
            "earlier snapshots retain expected error channels");
    require(!recover(triple + "fn main():\n  triple(\"bad\",va|Suffix,3\n"),
            "invalid earlier type suppresses a nonfinal slot snapshot");
    require(!query(add + "fn main():\n  add(va|Suffix,2,3)\n"),
            "extra arguments suppress earlier snapshots");
    require(!query(add + "fn main():\n  add(1+va|Suffix,2)\n"),
            "compound earlier holes remain excluded");
    require(!query(add + "fn main():\n  add(missing.va|Suffix,2)\n"),
            "unknown earlier field receiver");
    const std::string earlierSource = add + "fn main():\n  add(vaSuffix,2)\n";
    auto earlierSyntax =
        parse(std::make_shared<const SourceFile>("earlier-signature.k", earlierSource));
    SourceSpan earlierSite{};
    for (const auto &expression : earlierSyntax.module.expressions)
      if (std::holds_alternative<CallExpr>(expression.value))
        earlierSite = expression.span;
    CallableSignatureQuery earlierQuery{earlierSite, {}, {}, 0};
    CheckOptions earlierOptions;
    earlierOptions.signature = &earlierQuery;
    auto earlierChecked = check(earlierSyntax, {}, earlierOptions);
    require(!earlierChecked.ok() && !earlierChecked.module && earlierQuery.snapshot &&
                earlierChecked.diagnostics.size() == 1 &&
                earlierChecked.diagnostics[0].code == "S027",
            "earlier signature query publishes only a snapshot, never executable partial IR");
    auto originalChecked = check(std::move(earlierSyntax));
    require(!originalChecked.ok() && !originalChecked.module &&
                originalChecked.diagnostics[0].code == "S016",
            "ordinary compilation keeps actual unknown argument diagnostics");
    const std::string emptyPrefix =
        "type Reading:\n  value:int\n" + triple + "fn main():\n  reading=Reading {value:1}\n";
    result = recover(emptyPrefix + "  triple(reading.|,2\n");
    require(result && result->activeParameter == 0 && result->parameters.size() == 3,
            "empty earlier field combines with EOF repair and missing later parameter");
    result = recover(emptyPrefix + "  1 -> triple(reading.|,3)\n");
    require(result && result->activeParameter == 1, "empty middle field counts pipeline argument");
    result = recover(emptyPrefix + "  callbacks=[triple]\n  callbacks[0](reading.|,2,3)\n");
    require(result && result->activeParameter == 0 && result->label.starts_with("fn("),
            "empty earlier field with indexed callable");
    result = recover("type Reading:\n  __kalandra_signature_field:String\n" + add +
                     "fn main():\n  reading=Reading {__kalandra_signature_field:\"text\"}\n  "
                     "add(reading.|,2)\n");
    require(result && result->label == "fn add(left: int64, right: int64) -> int64",
            "colliding field probe never supplies an actual argument value");
    require(!recover(emptyPrefix + "  triple(\"bad\",reading.|,3)\n"),
            "invalid prefix suppresses empty middle slot");
    require(!recover(emptyPrefix + "  triple(missing.|,2,3)\n"),
            "unknown receiver suppresses empty first slot");
    require(!recover(emptyPrefix + "  triple(1.|,2,3)\n"),
            "numeric receiver cannot supply empty field guidance");
    require(bool(recover(emptyPrefix + "  triple(reading.|,,3)\n")),
            "checked first slot survives a later missing argument");
    require(bool(recover(emptyPrefix + "  triple(reading.|,[2,3\n")),
            "checked first slot survives a later unclosed array");
    std::cout << "Native checked signature help passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
