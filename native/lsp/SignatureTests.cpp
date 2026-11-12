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
    const std::string add = "fn add(left:i64,right:i64)->i64:\n  return left+right\n";
    auto genericSignature = query("fn identity[T](value:T)->T:\n  return value\n"
                                  "fn main()->i64:\n  return identity(|42)\n");
    require(genericSignature && genericSignature->label == "fn identity[T](value: T) -> T",
            "generic source signature lost");
    genericSignature = recover("fn choose[T](left:T,right:T)->T:\n  return left\n"
                               "fn main()->i64:\n  return choose(42, |\n");
    require(genericSignature && genericSignature->label == "fn choose[T](left: T, right: T) -> T" &&
                genericSignature->activeParameter == 1,
            "incomplete generic argument signature lost");
    auto result = query(add + "fn main():\n  return add(|1,2)\n");
    require(result && result->label == "fn add(left: i64, right: i64) -> i64" &&
                result->parameters == std::vector<std::string>{"left: i64", "right: i64"} &&
                result->activeParameter == 0,
            "named signature and first argument");
    result = query(add + "fn main():\n  return add(1, |2)\n");
    require(result && result->activeParameter == 1, "second argument");
    result = query(add + "fn main():\n  return add(add(1,2), |3)\n");
    require(result && result->activeParameter == 1, "nested commas do not advance outer arguments");
    result = query(add + "fn main():\n  return add(1,add(|2,3))\n");
    require(result && result->activeParameter == 0, "innermost call selected");
    result = query(add + "fn main():\n  return 1 -> add(|2)\n");
    require(result && result->activeParameter == 1, "pipeline supplies first parameter");
    result = query(add + "fn main():\n  callback=add\n  return callback(1,|2)\n");
    require(result && result->label == "fn callback(i64, i64) -> i64" &&
                result->activeParameter == 1,
            "function binding uses checked callable type");
    result = query("fn invoke(callback:fn(i64,i64)->i64)->i64:\n  return callback(1,|2)\nfn "
                   "main():\n  return 0\n");
    require(result && result->parameters == std::vector<std::string>{"i64", "i64"},
            "function parameter signature");
    result = query("fn inspect(mut values:[i64])->i64:\n  return length(values)\nfn main():\n  "
                   "return inspect(|[1])\n");
    require(result && result->label == "fn inspect(mut values: [i64]) -> i64" &&
                result->parameters == std::vector<std::string>{"mut values: [i64]"},
            "named signature lost mutating access contract");
    result = query("fn inspect(mut values:[i64])->i64:\n  return length(values)\nfn main():\n  "
                   "f=inspect\n  return f(|[1])\n");
    require(result && result->label == "fn f(mut [i64]) -> i64",
            "indirect signature lost mutating access contract");
    result = query("type Box:\n  callback:fn(i64)->i64\nfn double(n:i64)->i64:\n  return n*2\nfn "
                   "main():\n  box=Box {callback:double}\n  return box.callback(|2)\n");
    require(result && result->label == "fn callback(i64) -> i64", "function field signature");
    result = query("fn constant()->i64:\n  return 1\nfn main():\n  return constant(|)\n");
    require(result && result->parameters.empty() && !result->activeParameter,
            "zero-parameter signature has no active parameter");
    result = query("fn count(values:[i64],text:String)->i64:\n  return length(values)\nfn "
                   "main():\n  return count([1,2,3], |\"a,b\")\n");
    require(result && result->activeParameter == 1, "array/string commas stay nested");
    result = query("error Bad:\n  Failed\nfn risky(n:i64)->i64 ! Bad:\n  return n\nfn main():\n  "
                   "return risky(|1) catch fn(e):return 0\n");
    require(result && result->label == "fn risky(n: i64) -> i64 ! Bad",
            "expected-error channel signature");
    result = query(
        "fn main():\n  scale=2\n  callback=fn(n:i64):return n*scale\n  return callback(|3)\n");
    require(result && result->parameters == std::vector<std::string>{"i64"},
            "captured parameters are not public arguments");
    require(!query(add + "fn main():\n  return add(|1)\n"),
            "argument mismatch supplies no partial types");
    require(!query(add + "fn main():\n  return add(|\n"), "unfinished call not recovered");
    require(!query(add + "fn main():\n  return add(1,2)|\n"), "outside call has no signature");
    require(!query(add + "fn main():\n  return ad|d(1,2)\n"), "callee text has no active argument");
    require(!query(add + "fn main():\n  missing\n  return add(|1,2)\n"),
            "invalid graph fails closed");
    result = query("fn main()->[i64]:\n  return copy(|[1,2])\n");
    require(result && result->label == "fn copy(input: [i64]) -> [i64]",
            "checked independent-copy signature");
    result = query("fn main():\n  return generate(|4,fn(i): return i*2)\n");
    require(result &&
                result->label == "fn generate(count: i64, callback: fn(i64) -> i64) -> [i64]" &&
                result->activeParameter == 0,
            "generator signature describes count and public callback without captures");
    result = query("fn main():\n  return length(|[1,2])\n");
    require(result && result->label == "fn length(input: [i64]) -> i64" &&
                result->activeParameter == 0,
            "checked array length signature");
    result = query(
        "fn make()->fn(i64)->i64:\n  return fn(n:i64):return n\nfn main():\n  return make()(|1)\n");
    require(result && result->label == "fn(i64) -> i64" && result->activeParameter == 0,
            "factory result signature rather than factory signature");
    result = query("fn make(offset:i64)->fn(i64,i64)->i64:\n  return fn(a:i64,b:i64):return "
                   "a+b+offset\nfn main():\n  return make(3)(1,|2)\n");
    require(result && result->parameters == std::vector<std::string>{"i64", "i64"} &&
                result->activeParameter == 1,
            "captured factory result parameters");
    result = query("fn make(offset:i64)->fn(i64,i64)->i64:\n  return fn(a:i64,b:i64):return "
                   "a+b+offset\nfn main():\n  return make(|3)(1,2)\n");
    require(result && result->label == "fn make(offset: i64) -> (fn(i64, i64) -> i64)",
            "factory arguments still show factory signature");
    result = query(add + "fn main():\n  callbacks=[add]\n  return callbacks[0](1,|2)\n");
    require(result && result->label == "fn(i64, i64) -> i64" && result->activeParameter == 1,
            "indexed callable signature");
    result = query(add + "fn main():\n  callbacks=[add]\n  return 1 -> callbacks[0](|2)\n");
    require(result && result->activeParameter == 1, "indexed pipeline implicit parameter");
    result = query("fn make()->fn(i64,i64)->i64:\n  return fn(a:i64,b:i64):return a+b\nfn "
                   "main():\n  return 1 -> make()(|2)\n");
    require(result && result->activeParameter == 1, "factory pipeline checked invocation span");
    result = query("fn main():\n  return (fn(n:i64):return n*2)(|3)\n");
    require(result && result->label == "fn(i64) -> i64", "direct anonymous call signature");
    result = query("fn make()->[fn(i64)->i64]:\n  return [fn(n:i64):return n]\nfn main():\n  "
                   "return make()[0](|3)\n");
    require(result && result->label == "fn(i64) -> i64", "indexed factory result signature");
    result = query("error Bad:\n  Failed\nfn make()->fn(i64)->i64 ! Bad:\n  return "
                   "fn(n:i64):return n\nfn main():\n  return make()(|3) catch fn(e):return 0\n");
    require(result && result->label == "fn(i64) -> i64 ! Bad",
            "returned callable error channel is retained");
    require(!query(add + "fn main():\n  callbacks=[add]\n  return callbacks[0](|1)\n"),
            "invalid indexed arguments do not leak signature");
    require(!query(add + "fn main():\n  callbacks=[add]\n  return callbacks[2](|1,2)\n"),
            "invalid index fails whole graph");
    require(!query("fn main():\n  numbers=[1]\n  return numbers[0](|2)\n"),
            "indexed numeric value is not callable");
    require(!query("fn make()->i64:\n  return 1\nfn main():\n  return make()(|2)\n"),
            "numeric factory result does not suggest factory signature");
    result = recover(add + "fn main():\n  return add(1,2|\n");
    require(result && result->activeParameter == 1, "recover missing closing parenthesis");
    result = recover(add + "fn main():\n  return add(1,add(2,3|\n");
    require(result && result->activeParameter == 1, "recover nested trailing parentheses");
    result = recover("fn constant()->i64:\n  return 1\nfn main():\n  return constant(|\n");
    require(result && result->parameters.empty(), "empty zero-parameter call recovery");
    result = recover("fn make()->fn(i64,i64)->i64:\n  return fn(a:i64,b:i64):return a+b\nfn "
                     "main():\n  return make()(1,2|\n");
    require(result && result->label == "fn(i64, i64) -> i64", "factory-result recovery");
    result = recover(add + "fn main():\n  callbacks=[add]\n  return 1 -> callbacks[0](2|\n");
    require(result && result->activeParameter == 1, "indexed pipeline recovery");
    result = recover(add + "fn main():\n  return add(1,2|)\n");
    require(result && result->activeParameter == 1, "ordinary valid calls retain signature");
    result = recover(add + "fn main():\n  return add(|\n");
    require(result && result->activeParameter == 0, "first missing argument signature");
    result = recover(add + "fn main():\n  return add(1,|\n");
    require(result && result->activeParameter == 1, "next missing argument signature");
    result = query(add + "fn main():\n  return add(1,|)\n");
    require(result && result->activeParameter == 1, "balanced incomplete call signature");
    result = recover(add + "fn main():\n  callback=add\n  return callback(1,|\n");
    require(result && result->label == "fn callback(i64, i64) -> i64",
            "bound callable missing arguments");
    result = recover("fn make()->fn(i64,i64)->i64:\n  return fn(a:i64,b:i64):return a+b\nfn "
                     "main():\n  return make()(1,|\n");
    require(result && result->label == "fn(i64, i64) -> i64", "factory missing argument");
    result = recover(add + "fn main():\n  callbacks=[add]\n  return 1 -> callbacks[0](|\n");
    require(result && result->activeParameter == 1, "indexed pipeline missing argument");
    require(!recover(add + "fn main():\n  return add(\"bad\",|\n"),
            "invalid prefix argument type blocks snapshot");
    require(!recover(add + "fn main():\n  return add(missing,|\n"),
            "invalid prefix name blocks snapshot");
    require(
        bool(recover(add + "fn broken()->i64:\n  return missing\nfn main():\n  return add(1,|\n")),
        "independent broken body does not erase checked signature");
    require(!recover("fn broken(n:i64)->i64:\n  return missing\nfn main():\n  return broken(|\n"),
            "invalid target body blocks named signature snapshot");
    result = recover("type Point:\n  x:i64\nfn inspect(point:Point,n:i64)->i64:\n  return "
                     "point.x+n\nfn main():\n  return inspect(Point {x:1},|\n");
    require(result && result->parameters == std::vector<std::string>{"point: Point", "n: i64"},
            "nominal parameter snapshot");
    result = recover("error First:\n  Bad\nerror Second:\n  Bad\nfn risky(a:i64,b:i64)->i64 ! "
                     "(First | Second):\n  return a+b\nfn main():\n  return risky(1,|\n");
    require(result && result->label == "fn risky(a: i64, b: i64) -> i64 ! First | Second",
            "error union snapshot display metadata");
    require(!query("fn main(n:i64)->i64:\n  return main(|)\n"),
            "incomplete recursive callee does not snapshot an unfinished definition");
    require(!query(add + "fn main():\n  return add(1,|)\nfn later()->i64:\n  return missing\n"),
            "later reported graph error invalidates an already captured snapshot");
    const std::string missingCall = add + "fn main():\n  return add(1,)\n";
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
    (void)check(parse(std::make_shared<const SourceFile>("snapshot.k", "fn main():\n  return 0\n")),
                {}, options);
    require(!snapshot.snapshot, "query snapshot resets between checks");
    auto ordinary = check(parse(std::make_shared<const SourceFile>("snapshot.k", missingCall)));
    require(!ordinary.ok() && !ordinary.module && ordinary.diagnostics.size() == 1 &&
                ordinary.diagnostics.front().code == "S027",
            "ordinary compiler keeps its missing-argument diagnostic");
    require(!recover(add + "fn main():\n  add(1,2|\n  return missing\n"),
            "later code is not hidden");
    require(!recover(add + "fn main():\n  missing\n  return add(1,2|\n"),
            "earlier errors fail closed");
    require(!recover(add + "fn main():\n  return add(1,\"text\"|\n"), "argument types must check");
    require(!recover(add + "fn main():\n  return add([1,2|\n"), "unclosed array is not recovered");
    require(!recover(add + "fn main():\n  return add(1,{|\n"), "unclosed record is not recovered");
    require(!recover(add + "fn main():\n  return add(1,2]|\n"),
            "mismatched delimiters fail closed");
    require(!recover(add + "fn main():\n  return add(1,\"unfinished|"),
            "unclosed string is not recovered");
    require(!recover(add + "fn main():\n  return add(1,2 # comment|"),
            "comment does not receive delimiters");
    require(!recover(add + "fn main():\n  return ((((((((add(1,2|\n"),
            "recovery has an eight-delimiter bound");
    result = query(add + "fn main():\n  return add(1, va|Suffix)\n");
    require(result && result->activeParameter == 1 &&
                result->parameters == std::vector<std::string>{"left: i64", "right: i64"},
            "final partial identifier retains named parameter guidance");
    result = recover(add + "fn main():\n  return add(1, val|\n");
    require(result && result->activeParameter == 1, "partial name with trailing open call");
    result = query(
        "fn single(value:i64)->i64:\n  return value\nfn main():\n  return single(va|Suffix)\n");
    require(result && result->activeParameter == 0, "first and final bare identifier");
    result = recover("fn make()->fn(i64,i64)->i64:\n  return fn(a:i64,b:i64):return a+b\nfn "
                     "main():\n  return make()(1, val|\n");
    require(result && result->label == "fn(i64, i64) -> i64", "factory argument editing hole");
    result = recover(add + "fn main():\n  callbacks=[add]\n  return 1 -> callbacks[0](val|\n");
    require(result && result->activeParameter == 1, "indexed pipeline argument editing hole");
    result = query(add + "fn main():\n  value=2\n  return add(1, val|ue)\n");
    require(result && result->activeParameter == 1, "resolved name still has parameter guidance");
    result = query(add + "fn main():\n  value=\"bad\"\n  return add(1, val|ue)\n");
    require(result && result->activeParameter == 1,
            "active editing hole is not claimed to have a valid type");
    require(!query(add + "fn main():\n  return add(missing, va|Suffix)\n"),
            "earlier invalid argument still blocks guidance");
    result = query(add + "fn main():\n  return add(va|Suffix, 2)\n");
    require(result && result->activeParameter == 0,
            "earlier partial argument retains parameter guidance");
    require(!query(add + "fn main():\n  return add(1, 2 + va|Suffix)\n"),
            "partial expression is not a bare-name editing hole");
    const std::string records = "type Reading:\n  value:i64\n"
                                "type Box:\n  reading:Reading\n";
    result = query(add + records +
                   "fn main():\n  reading=Reading {value:2}\n  return add(1, reading.va|Suffix)\n");
    require(result && result->activeParameter == 1, "final field identifier retains guidance");
    result = recover(
        add + records +
        "fn main():\n  box=Box {reading:Reading {value:2}}\n  return add(1, box.reading.va|\n");
    require(result && result->activeParameter == 1, "nested receiver with open trailing call");
    result = query(add + records +
                   "fn make()->Reading:\n  return Reading {value:2}\nfn main():\n  return add(1, "
                   "make().va|Suffix)\n");
    require(result && result->activeParameter == 1, "checked factory-result receiver");
    result =
        query(add + records +
              "fn main():\n  values=[Reading {value:2}]\n  return 1 -> add(values[0].va|Suffix)\n");
    require(result && result->activeParameter == 1, "indexed receiver in pipeline");
    result = query(add + records +
                   "fn make()->fn(i64,i64)->i64:\n  return add\nfn main():\n  reading=Reading "
                   "{value:2}\n  return make()(1, reading.va|Suffix)\n");
    require(result && result->label == "fn(i64, i64) -> i64", "structural callee field editing");
    result = query(add + "type Wrong:\n  value:String\nfn main():\n  reading=Wrong "
                         "{value:\"text\"}\n  return add(1, reading.val|ue)\n");
    require(result && result->activeParameter == 1,
            "active field is not claimed to have expected type");
    require(
        !query(
            add + records +
            "fn main():\n  reading=Reading {value:2}\n  return add(missing, reading.va|Suffix)\n"),
        "invalid earlier argument blocks field guidance");
    result = query(add + records +
                   "fn main():\n  reading=Reading {value:2}\n  return add(reading.va|Suffix, 1)\n");
    require(result && result->activeParameter == 0,
            "earlier field argument receives checked guidance");
    require(!query(add + records +
                   "fn main():\n  reading=Reading {value:2}\n  return add(1, "
                   "reading.missing.va|Suffix)\n"),
            "invalid intermediate field receiver blocks guidance");
    require(!query(add + "fn main():\n  n=1\n  return add(1, n.va|Suffix)\n"),
            "numeric receiver rejected");
    require(
        !query(add + records +
               "fn main():\n  values=[Reading {value:2}]\n  return add(1, values[2].va|Suffix)\n"),
        "invalid indexed receiver rejected");
    require(!query(add + "enum Choice:\n  Some {value:i64}\nfn main():\n  choice=Choice.Some "
                         "{value:2}\n  return add(1, choice.va|Suffix)\n"),
            "enum payload requires narrowing rather than arbitrary field access");
    result = recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  return add(1, reading.|\n");
    require(result && result->activeParameter == 1, "empty field and trailing call recovery");
    result = recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  return add(1, reading.|)\n");
    require(result && result->activeParameter == 1, "balanced empty field slot");
    result = recover(
        add + records +
        "fn main():\n  box=Box {reading:Reading {value:2}}\n  return 1 -> add(box.reading.|)\n");
    require(result && result->activeParameter == 1, "nested empty field in pipeline");
    result = recover(
        add + records +
        "fn make()->Reading:\n  return Reading {value:2}\nfn main():\n  return add(1, make().|)\n");
    require(result && result->activeParameter == 1, "empty field on factory result");
    result = recover(add + records +
                     "fn main():\n  values=[Reading {value:2}]\n  return add(1, values[0].|)\n");
    require(result && result->activeParameter == 1, "empty field on indexed record");
    result = recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  callbacks=[add]\n  return "
                     "callbacks[0](1, reading.|)\n");
    require(result && result->label == "fn(i64, i64) -> i64", "empty field with structural callee");
    require(!recover(add + "fn main():\n  return add(1, unknown.|)\n"),
            "unknown empty-field receiver rejected");
    require(!recover(add + "fn main():\n  n=1\n  return add(1, n.|)\n"),
            "non-record empty-field receiver rejected");
    require(!recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  return add(missing, reading.|)\n"),
            "empty field preserves earlier argument errors");
    result = recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  return add(reading.|, 1)\n");
    require(result && result->activeParameter == 0,
            "earlier empty field receives checked guidance");
    require(bool(recover(
                add + records +
                "fn main():\n  reading=Reading {value:2}\n  return add(1, reading.|unknown)\n")),
            "checked field prefix retains original suffix");
    require(
        bool(recover(add + records +
                     "fn main():\n  reading=Reading {value:2}\n  return add(1, reading.| + 2)\n")),
        "compound empty field receives checked guidance");
    require(bool(recover(add + records +
                         "fn main():\n  reading=Reading {value:2}\n  return add(1, reading.|)\nfn "
                         "later():\n  return missing\n")),
            "independent later errors preserve empty-field guidance");
    require(!recover(add + "fn main():\n  return add(1, \"reading.|\"\n"),
            "string content is not a field slot");
    const std::string collision = "type Probe:\n  __kalandra_signature_field:i64\n"
                                  "fn main():\n  reading=Probe {__kalandra_signature_field:2}\n";
    result = recover(add + collision + "  add(1, reading.|)\n");
    require(result && result->activeParameter == 1,
            "probe-name collision still uses receiver projection");
    require(bool(recover(add + collision + "  add(1, (reading.|))\n")),
            "grouped field query captures a checked signature without supplying a value");
    require(
        !recover(add + records +
                 "fn main():\n  reading=Reading {value:2}\n  return ((((((((add(1, reading.|\n"),
        "empty-field delimiter recovery keeps eight-parenthesis limit");
    require(!query(add + "fn main():\n  return add(1, box.va|Suffix)\n"),
            "unknown field receiver blocks signature guidance");
    require(!query(add + "fn main():\n  return add(1, 2, va|Suffix)\n"),
            "extra partial argument does not invent another parameter");
    require(!query("fn main():\n  return missing(va|Suffix)\n"), "unknown callee has no guidance");
    require(!query(add + "fn main():\n  add=1\n  return add(1, va|Suffix)\n"),
            "noncallable lexical shadow has no guidance");
    result = query("fn main():\n  text=\"hello\"\n  return length(te|xt)\n");
    require(result && result->label == "fn length(input: String) -> i64",
            "checked builtin argument is not mistaken for an editing hole");
    result = query("fn main():\n  return byteLength(|\"😀\")\n");
    require(result && result->label == "fn byteLength(input: String) -> i64",
            "String byte length signature");
    result = query("fn main():\n  return shape(|tensor([1,2]))\n");
    require(result && result->label.find("fn shape(input: Tensor[i64, 2])") == 0 &&
                result->parameters.size() == 1,
            "tensor shape specialization");
    result = query("fn main():\n  return slice([1,2,3],0,|2)\n");
    require(result && result->label == "fn slice(input: [i64], start: i64, end: i64) -> [i64]" &&
                result->activeParameter == 2,
            "array slice checked bounds parameters");
    result = query("fn main():\n  return \"hello\" -> slice(0,|2)\n");
    require(result && result->label == "fn slice(input: String, start: i64, end: i64) -> String" &&
                result->activeParameter == 2,
            "String slice pipeline active parameter");
    result = query("fn main():\n  return concat([true],|[false])\n");
    require(result && result->label == "fn concat(left: [bool], right: [bool]) -> [bool]" &&
                result->activeParameter == 1,
            "Bool array concat signature");
    result = query("fn main():\n  return concat([\"a\"],|[\"b\"])\n");
    require(result && result->label == "fn concat(left: [String], right: [String]) -> [String]",
            "String array concat signature");
    require(!query("fn main():\n  return concat(\"a\",|\"b\")\n"),
            "unsupported scalar String concat has no guidance");
    result = query(
        "type Box:\n  value:i64\nfn main():\n  return concat([Box {value:1}],|[Box {value:2}])\n");
    require(result && result->label == "fn concat(left: [Box], right: [Box]) -> [Box]",
            "nominal array context remains available to the type printer");
    result = recover("fn main():\n  values=[1,2]\n  return length(values|\n");
    require(result && result->label == "fn length(input: [i64]) -> i64",
            "trailing parenthesis recovery checks complete builtin arguments");
    require(!query("fn main():\n  return length(|)\n"),
            "empty builtin has no guessed specialization");
    require(!query("fn main():\n  return slice([1,2],|0)\n"),
            "missing builtin argument has no guessed type");
    require(!query("fn main():\n  return length(|42)\n"), "invalid builtin input has no signature");
    require(!query("fn main():\n  missing\n  return length(|[1,2])\n"),
            "prior errors block builtin guidance");
    require(!query("fn main():\n  length(|[1,2])\n  return missing\n"),
            "later errors block builtin guidance");
    require(!query("fn main():\n  return reshape(tensor([1,2]),|[2])\n"),
            "other builtins remain unsupported");
    result = query("fn main():\n  length=fn(v:i64):return v*2\n  return length(|3)\n");
    require(result && result->label == "fn length(i64) -> i64",
            "lexical callable shadow is not a builtin");
    result = recover("fn main():\n  return concat([1,2],|\n");
    require(result && result->label == "fn concat(left: [i64], right: [i64]) -> [i64]" &&
                result->activeParameter == 1,
            "missing concat operand from first array");
    result = query("fn main():\n  return [true] -> concat(|)\n");
    require(result && result->label == "fn concat(left: [bool], right: [bool]) -> [bool]" &&
                result->activeParameter == 1,
            "piped missing concat operand");
    result = recover("fn main():\n  return concat([\"a\"], ri|Suffix)\n");
    require(result && result->label == "fn concat(left: [String], right: [String]) -> [String]",
            "partly typed right operand uses first array");
    result = query("fn main()->[i32]:\n  return concat([],|)\n");
    require(result && result->label == "fn concat(left: [i32], right: [i32]) -> [i32]",
            "expected array context can type an empty first operand");
    result = query("type Box:\n  value:i64\nfn main():\n  return concat([Box {value:1}],|)\n");
    require(result && result->label == "fn concat(left: [Box], right: [Box]) -> [Box]",
            "nominal array snapshot context");
    result =
        query("fn main():\n  callbacks=[fn(v:i64):return v*2]\n  return concat(callbacks,|)\n");
    require(result && result->label == "fn concat(left: [fn(i64) -> i64], right: [fn(i64) -> "
                                       "i64]) -> ([fn(i64) -> i64])",
            "callable array contract survives the snapshot");
    result = recover(
        "type Box:\n  value:i64\nfn main():\n  box=Box {value:1}\n  return concat([1], box.|)\n");
    require(result && result->activeParameter == 1, "missing field still checks its receiver");
    require(!recover("fn main():\n  return concat([1], missing.|)\n"),
            "unknown projected field receiver prevents a snapshot");
    require(!query("fn main():\n  return concat(|)\n"), "no first array means no element contract");
    require(!query("fn main():\n  return concat([],|)\n"),
            "uncontextual empty array has no element contract");
    require(!query("fn main():\n  return concat(42,|)\n"),
            "scalar does not establish array contract");
    require(!query("fn main():\n  missing\n  return concat([1],|)\n"),
            "prior errors prevent concat snapshots");
    require(!query("fn main():\n  return concat([1],2,|)\n"),
            "extra arguments do not receive a snapshot");
    require(!recover("fn main():\n  return concat([1], unknown + 1|)\n"),
            "compound right operand does not receive expression recovery");
    for (const auto &source : numericTypes)
      for (const auto &destination : numericTypes) {
        const auto from = std::string(source.sourceName), to = std::string(destination.sourceName);
        result = query("fn main():\n  return " + to + "(|" + from + "(1))\n");
        require(result && result->label == "fn " + to + "(input: " + from + ") -> " + to &&
                    result->activeParameter == 0,
                "conversion signature pair " + from + " to " + to);
      }
    result = query("fn main():\n  return f64(|1)\n");
    require(result && result->label == "fn f64(input: i64) -> f64",
            "conversion alias keeps its surface name and concrete result");
    result = query("fn main():\n  return u64(|1)\n");
    require(result && result->label == "fn u64(input: i64) -> u64", "uint conversion alias");
    result = query("fn main():\n  return i64(|f32(1))\n");
    require(result && result->label == "fn i64(input: f32) -> i64", "int conversion alias");
    result = query("fn main():\n  return i16(|tensor([1,2],[1,2]))\n");
    require(result && result->label == "fn i16(input: Tensor[i64, 1, 2]) -> Tensor[i16, 1, 2]",
            "tensor conversion retains rank and fixed extents");
    result = query(
        "fn convert(values:Tensor[f32,?,2]):\n  return f64(va|lues)\nfn main():\n  return 0\n");
    require(result && result->label == "fn f64(input: Tensor[f32, ?, 2]) -> Tensor[f64, ?, 2]",
            "dynamic conversion shapes hide inference identities");
    result = query("fn main():\n  return 1 -> f32(|)\n");
    require(result && result->label == "fn f32(input: i64) -> f32" && result->activeParameter == 0,
            "piped scalar conversion signature");
    result = recover("fn main():\n  value=1\n  return u8(value|\n");
    require(result && result->label == "fn u8(input: i64) -> u8",
            "conversion guidance after bounded delimiter repair");
    result = query("fn main():\n  f32=fn(v:i64):return v+1\n  return f32(|2)\n");
    require(result && result->label == "fn f32(i64) -> i64",
            "same-spelled lexical function is not numeric conversion");
    require(!query("fn main():\n  return i8(|300)\n"),
            "out-of-range conversion has no checked hint");
    require(!query("fn main():\n  return f32(|[1,2])\n"),
            "numeric arrays remain unsupported casts");
    require(!query("fn main():\n  return i64(|true)\n"), "Bool is not a numeric source");
    require(!query("fn main():\n  return f32(|)\n"),
            "missing conversion source has no guessed type");
    require(!query("fn main():\n  return f32(|1,2)\n"), "extra conversion arguments have no hint");
    require(!query("fn main():\n  f32(|1)\n  return missing\n"),
            "failed graphs suppress numeric hints");
    for (const auto &numeric : numericTypes) {
      const auto name = std::string(numeric.sourceName);
      const auto matrix = "tensor([" + name + "(1)],[1,1])";
      result = query("fn main():\n  return reduceSum(|" + matrix + ")\n");
      require(result &&
                  result->label == "fn reduceSum(input: Tensor[" + name + ", 1, 1]) -> " + name,
              "reduction signature for " + name);
      result = query("fn main():\n  return matmul(" + matrix + ",|" + matrix + ")\n");
      require(result &&
                  result->label == "fn matmul(left: Tensor[" + name + ", 1, 1], right: Tensor[" +
                                       name + ", 1, 1]) -> Tensor[" + name + ", 1, 1]" &&
                  result->activeParameter == 1,
              "matrix signature for " + name);
      result = query("fn main():\n  return array(|" + matrix + ")\n");
      require(result &&
                  result->label == "fn array(input: Tensor[" + name + ", 1, 1]) -> [" + name + "]",
              "tensor flattening signature for " + name);
    }
    result = query("fn multiply(a:Tensor[f32,?,3],b:Tensor[f32,3,?]):\n  return matmul(a,|b)\nfn "
                   "main():\n  return 0\n");
    require(result && result->label == "fn matmul(left: Tensor[f32, ?, 3], right: "
                                       "Tensor[f32, 3, ?]) -> Tensor[f32, ?, ?]",
            "dynamic matrix result derives outer axes");
    result = query("fn main():\n  return tensor([1,2],[1,2]) -> matmul(|tensor([3,4],[2,1]))\n");
    require(result && result->activeParameter == 1 &&
                result->label == "fn matmul(left: Tensor[i64, 1, 2], right: Tensor[i64, 2, 1]) "
                                 "-> Tensor[i64, 1, 1]",
            "piped matrix call tracks explicit right operand");
    result = query("fn main():\n  return relu(|tensor([-1,2]))\n");
    require(result && result->label == "fn relu(input: Tensor[i64, 2]) -> Tensor[i64, 2]",
            "ReLU tensor signature");
    result = query("fn main():\n  return reduceSum(|tensor([7],[]))\n");
    require(result && result->label == "fn reduceSum(input: Tensor[i64]) -> i64",
            "rank-zero reduction signature");
    result = query("fn main():\n  values:Tensor[f32,0]=tensor([])\n  return array(|values)\n");
    require(result && result->label == "fn array(input: Tensor[f32, 0]) -> [f32]",
            "empty tensor array result");
    result = recover("fn main():\n  value=tensor([1,2])\n  return reduceSum(value|\n");
    require(result && result->label == "fn reduceSum(input: Tensor[i64, 2]) -> i64",
            "reduction guidance after bounded delimiter recovery");
    result = query("fn main():\n  matmul=fn(v:i64):return v+1\n  return matmul(|1)\n");
    require(result && result->label == "fn matmul(i64) -> i64",
            "lexical matrix-name shadow remains an ordinary callable");
    require(!query("fn main():\n  return matmul(tensor([1],[1,1]),|tensor([1,2],[2,1]))\n"),
            "incompatible matrix inner extents have no signature");
    require(!query("fn main():\n  return matmul(tensor([1],[1,1]),|tensor([f32(1)],[1,1]))\n"),
            "mixed matrix element types have no signature");
    require(!query("fn main():\n  return reduceSum(|[1,2])\n"), "arrays are not reduction tensors");
    require(!query("fn main():\n  return array(|[1,2])\n"), "array conversion requires a tensor");
    require(!query("fn main():\n  return matmul(tensor([1],[1,1]),|)\n"),
            "missing matrix operand has no guessed shape");
    require(!query("fn main():\n  reduceSum(|tensor([1]))\n  return missing\n"),
            "failed graphs suppress tensor signatures");
    const auto mapLabel = "fn map(input: [i64], callback: fn(i64) -> i64) -> [i64]";
    result = query("fn double(v:i64):\n  return v*2\nfn main():\n  return map([1,2],|double)\n");
    require(result && result->label == mapLabel && result->activeParameter == 1,
            "named map callback signature");
    result = query("fn main():\n  return map([1,2],|fn(v):return v*2)\n");
    require(result && result->label == mapLabel, "inferred anonymous map signature");
    result = query("fn main():\n  offset=3\n  return map([1,2],|fn(v):return v+offset)\n");
    require(result && result->label == mapLabel, "inline captures stay out of public callback");
    result = query("fn main():\n  offset=3\n  callback=fn(v:i64):return v+offset\n  return "
                   "map([1,2],|callback)\n");
    require(result && result->label == mapLabel, "retained function wrapper target stays hidden");
    result = query(
        "fn main():\n  callbacks=[fn(v:i64):return v*2]\n  return map([1,2],|callbacks[0])\n");
    require(result && result->label == mapLabel, "indexed callback retains public signature");
    result = query("fn main():\n  return [1,2] -> map(|fn(v):return v*2)\n");
    require(result && result->label == mapLabel && result->activeParameter == 1,
            "piped map selects explicit callback");
    result = query("fn main():\n  return map([1,2],|fn(v):return v>0)\n");
    require(result && result->label == "fn map(input: [i64], callback: fn(i64) -> bool) -> [bool]",
            "map result element differs from input");
    result = query("type Box:\n  value:i64\nfn main():\n  return map([Box {value:2}],|fn(v):return "
                   "v.value)\n");
    require(result && result->label == "fn map(input: [Box], callback: fn(Box) -> i64) -> [i64]",
            "nominal callback input context");
    for (const auto &numeric : numericTypes) {
      const auto name = std::string(numeric.sourceName);
      result = query("fn main():\n  return map([" + name + "(1)],|fn(v):return v)\n");
      require(result && result->label == "fn map(input: [" + name + "], callback: fn(" + name +
                                             ") -> " + name + ") -> [" + name + "]",
              "map element width " + name);
    }
    result =
        query("fn main()->[i64] ! TaskError:\n  return try parallelMap([1,2],|fn(v):return v*2)\n");
    require(result && result->label == "fn parallelMap(input: [i64], callback: fn(i64) -> "
                                       "i64) -> [i64] ! TaskError",
            "parallel map exposes scheduling error channel only on outer result");
    result =
        query("error Bad:\n  Failed\nfn checked(v:i64)->i64 ! Bad:\n  return v\nfn main()->[i64] ! "
              "(Bad | TaskError):\n  return try parallelMap([1,2],|checked)\n");
    require(result && result->label == "fn parallelMap(input: [i64], callback: fn(i64) -> "
                                       "i64 ! Bad) -> [i64] ! Bad | TaskError",
            "callback application errors remain separate from task errors");
    result = recover("fn main():\n  return map([1,2],fn(v:i64):return v*2|\n");
    require(result && result->label == mapLabel, "complete callback after bounded recovery");
    result = query("fn main():\n  map=fn(v:i64):return v*2\n  return map(|2)\n");
    require(result && result->label == "fn map(i64) -> i64", "same-spelled map binding");
    require(!query("fn main():\n  return map([1],|)\n"),
            "missing callback has no guessed signature");
    require(!query("fn main():\n  return map([1],|fn(v:i64,w:i64):return v+w)\n"),
            "invalid callback arity");
    require(!query("fn main():\n  return parallelMap([1],|fn(v:i64):return v*2)\n"),
            "unhandled task errors block hints");
    require(!query("fn main():\n  return map([1],|fn(v:String):return v)\n"),
            "incompatible map callback input");
    CheckOptions placement;
    placement.placement = true;
    const std::string transferStart = "fn main()->i64 ! TaskError:\n  return try parallel:\n"
                                      "    values=tensor([1,2])\n";
    const std::string transferEnd =
        "\n    host=try await transfer(resident,devices.cpu())\n    reduceSum(host)\n";
    result = query(transferStart + "    resident=try await transfer(values,|devices.gpu(1))" +
                       transferEnd,
                   placement);
    require(result &&
                result->label == "fn transfer(input: Tensor[i64, 2], target: Device[gpu:1]) -> "
                                 "task<Tensor[i64, 2] on gpu:1>" &&
                result->activeParameter == 1 && result->parameters.size() == 2,
            "transfer exposes checked shape, exact device and task result");
    result = query(transferStart + "    resident=try await (values -> transfer(|devices.gpu(1)))" +
                       transferEnd,
                   placement);
    require(result && result->activeParameter == 1 &&
                result->parameters[0] == "input: Tensor[i64, 2]",
            "piped transfer selects device argument");
    result =
        query(transferStart +
                  "    resident=try await transfer(values,devices.gpu(1))\n"
                  "    host=try await transfer(resident,|devices.cpu())\n    reduceSum(host)\n",
              placement);
    require(result && result->label == "fn transfer(input: Tensor[i64, 2] on gpu:1, target: "
                                       "Device[cpu]) -> task<Tensor[i64, 2]>",
            "return transfer preserves input residency and CPU result");
    result = query("fn move(values:Tensor[f32,?,2])->i64 ! TaskError:\n  return try parallel:\n    "
                   "resident=try await transfer(values,|devices.gpu())\n    host=try await "
                   "transfer(resident,devices.cpu())\n    0\nfn main():\n  return 0\n",
                   placement);
    require(result && result->label == "fn transfer(input: Tensor[f32, ?, 2], target: "
                                       "Device[gpu:0]) -> task<Tensor[f32, ?, 2] on gpu:0>",
            "dynamic transfer shapes hide private extent identities");
    require(!query("fn main():\n  return transfer(tensor([1]),|devices.gpu())\n", placement),
            "transfer outside an owning group cannot offer a signature");
    require(!query(transferStart + "    resident=try await transfer(values,|1)" + transferEnd,
                   placement),
            "invalid transfer target has no guessed signature");
    require(!query(transferStart + "    resident=try await transfer(values,|devices.gpu(1))" +
                   transferEnd),
            "placement-disabled transfer has no builtin signature");
    result =
        query("fn transfer(value:i64):\n  return value+1\nfn main():\n  return transfer(|2)\n");
    require(result && result->label == "fn transfer(value: i64) -> i64",
            "CPU-mode source function retains ordinary transfer signature");
    result = query("fn main():\n  target=devices.cpu(|)\n  return 0\n", placement);
    require(result && result->label == "fn devices.cpu() -> Device[cpu]" &&
                result->parameters.empty() && !result->activeParameter,
            "CPU selector has no parameters");
    result = query("fn main():\n  target=devices.gpu(|)\n  return 0\n", placement);
    require(result && result->label == "fn devices.gpu() -> Device[gpu:0]" &&
                result->parameters.empty() && !result->activeParameter,
            "omitted GPU index describes checked default selection");
    result = query("fn main():\n  target=devices.gpu(|1)\n  return 0\n", placement);
    require(result && result->label == "fn devices.gpu(index: u32) -> Device[gpu:1]" &&
                result->parameters == std::vector<std::string>{"index: u32"} &&
                result->activeParameter == 0,
            "explicit selector index uses its checked literal contract");
    result = query("fn main():\n  target=devices.gpu(|4294967295)\n  return 0\n", placement);
    require(result && result->label.ends_with("Device[gpu:4294967295]"),
            "maximum selector index does not require available hardware");
    result = query("fn main():\n  target=1 -> devices.gpu(|)\n  return 0\n", placement);
    require(result && result->label == "fn devices.gpu(index: u32) -> Device[gpu:1]",
            "pipeline supplies the checked selector literal");
    for (const auto *argument : {"1,2", "-1", "1.5", "4294967296", "uint32(1)", "index"})
      require(!query("fn main():\n  index=1\n  target=devices.gpu(|" + std::string(argument) +
                         ")\n  0\n",
                     placement),
              "invalid selector has no guessed signature");
    require(!query("fn main():\n  target=devices.cpu(|1)\n  return 0\n", placement),
            "CPU selector rejects extra arguments");
    require(!query("fn main():\n  devices=1\n  target=devices.gpu(|1)\n  return 0\n", placement),
            "shadowed devices namespace has no selector signature");
    require(!query("fn main():\n  target=devices.gpu(|1)\n  return 0\n"),
            "selector signature requires placement checking");
    result = recover(add + "fn main():\n  value=2\n  return add(1,va|Suffix\n");
    require(result && result->label == "fn add(left: i64, right: i64) -> i64" &&
                result->activeParameter == 1,
            "final name suffix receives validated callee snapshot before EOF closure");
    result =
        recover(add + "fn main():\n  value=2\n  callback=add\n  return callback(1,va|Suffix\n");
    require(result && result->label == "fn callback(i64, i64) -> i64" &&
                result->activeParameter == 1,
            "function-value callee snapshot survives final argument suffix");
    result = recover(add + "fn main():\n  callbacks=[add]\n  return 1 -> callbacks[0](va|Suffix\n");
    require(result && result->activeParameter == 1 && result->parameters.size() == 2,
            "indexed pipeline callable preserves implicit first argument");
    const std::string fieldPrefix =
        "type Reading:\n  value:i64\n" + add + "fn main():\n  reading=Reading {value:2}\n";
    result = recover(fieldPrefix + "  add(1,reading.va|Suffix\n");
    require(result && result->activeParameter == 1 && result->label.starts_with("fn add("),
            "record field suffix requires checked receiver projection");
    result = recover(fieldPrefix + "  concat([i32(1)],reading.va|Suffix\n");
    require(result && result->label == "fn concat(left: [i32], right: [i32]) -> [i32]",
            "existing concat snapshot supports a projected final field suffix");
    require(!recover(add + "fn main():\n  return add(\"bad\",va|Suffix\n"),
            "bad prefix argument suppresses EOF suffix snapshot");
    require(!recover(add + "fn main():\n  return add(1,unknown.va|Suffix\n"),
            "unknown receiver suppresses EOF suffix snapshot");
    require(!recover(add + "fn main():\n  return missing(1,va|Suffix\n"),
            "unknown callee suppresses EOF suffix snapshot");
    require(!recover(add + "fn main():\n  callback=1\n  return callback(1,va|Suffix\n"),
            "noncallable callee suppresses EOF suffix snapshot");
    result = recover(add + "fn main():\n  return add(va|Suffix,2\n");
    require(result && result->activeParameter == 0,
            "earlier argument suffix receives EOF signature repair");
    require(bool(recover(add + "fn main():\n  value=2\n  return add(1,value+va|Suffix\n")),
            "compound final argument receives checked signature guidance");
    require(!recover(add + "fn main():\n  return add(1,[va|Suffix\n"),
            "open arrays are not repaired for signature suffix queries");
    require(!recover(add + "fn main():\n  return ((((((((add(1,va|Suffix\n"),
            "signature suffix repair retains eight-parenthesis bound");
    const std::string triple =
        "fn triple(first:i64,second:i64,third:i64):\n  return first+second+third\n";
    result = recover(triple + "fn main():\n  return triple(va|Suffix,2\n");
    require(result && result->label == "fn triple(first: i64, second: i64, third: i64) -> i64" &&
                result->activeParameter == 0,
            "earlier slot with missing later required parameter");
    result = query(triple + "fn main():\n  return triple(1,va|Suffix,missing)\n");
    require(result && result->activeParameter == 1,
            "later invalid arguments do not change prefix-only contract");
    result = recover(triple +
                     "fn main():\n  callbacks=[triple]\n  return 1 -> callbacks[0](va|Suffix,3\n");
    require(result && result->activeParameter == 1 && result->parameters.size() == 3,
            "indexed callable counts the implicit pipeline argument in earlier slot");
    result = recover(triple + "type Reading:\n  value:i64\nfn main():\n  reading=Reading "
                              "{value:1}\n  return triple(reading.va|Suffix,2\n");
    require(result && result->activeParameter == 0,
            "earlier field slot checks receiver before snapshot");
    result = query("error Bad:\n  Failed\nfn risky(first:i64,second:i64)->i64 ! Bad:\n  return "
                   "first+second\nfn main()->i64 ! Bad:\n  return try risky(va|Suffix,2)\n");
    require(result && result->label == "fn risky(first: i64, second: i64) -> i64 ! Bad",
            "earlier snapshots retain expected error channels");
    require(!recover(triple + "fn main():\n  return triple(\"bad\",va|Suffix,3\n"),
            "invalid earlier type suppresses a nonfinal slot snapshot");
    require(!query(add + "fn main():\n  return add(va|Suffix,2,3)\n"),
            "extra arguments suppress earlier snapshots");
    require(!query(add + "fn main():\n  return add(1+va|Suffix,2)\n"),
            "compound earlier holes remain excluded");
    require(!query(add + "fn main():\n  return add(missing.va|Suffix,2)\n"),
            "unknown earlier field receiver");
    const std::string earlierSource = add + "fn main():\n  return add(vaSuffix,2)\n";
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
        "type Reading:\n  value:i64\n" + triple + "fn main():\n  reading=Reading {value:1}\n";
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
                     "return add(reading.|,2)\n");
    require(result && result->label == "fn add(left: i64, right: i64) -> i64",
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
    const std::string buffers =
        "from \"std/buffer\" import ByteBuffer, BufferError, byteBuffer, append, intoArray\n"
        "from \"std/fs\" import open, OpenMode, FsError, readInto, writeAll\n"
        "fn main()->unit ! (BufferError | FsError):\n  b=try byteBuffer()\n";
    result = recover(buffers + "  try append(b,|");
    require(result && result->label.find("mut") != std::string::npos &&
                result->label.find("ByteBuffer") != std::string::npos &&
                result->activeParameter == 1,
            "buffer append signature preserves mutable access");
    result = recover(buffers + "  values=intoArray(|b)\n  return ()\n");
    require(result && result->label.find("own") != std::string::npos,
            "consuming buffer signature describes ownership");
    result = recover(buffers +
                     "  file=try open(\"input.bin\",OpenMode.Read)\n  count=try readInto(file,b,|");
    require(result && result->parameters.size() == 3 &&
                result->parameters[1].find("ByteBuffer") != std::string::npos &&
                result->parameters[2] == "maxBytes: u64",
            "filesystem buffer overload offers maximum byte count");
    std::cout << "Native checked signature help passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
