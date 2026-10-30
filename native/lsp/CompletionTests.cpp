#include "Completion.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
static lsp::CompletionInfo query(std::string text, CheckOptions options = {}) {
  const auto cursor = text.rfind('|');
  require(cursor != text.npos, "missing cursor");
  text.erase(cursor, 1);
  return lsp::completeNames((std::filesystem::temp_directory_path() / "completion.k").string(),
                            text, cursor, {}, {}, options);
}
static bool has(const lsp::CompletionInfo &info, const std::string &name) {
  for (const auto &item : info.names)
    if (item.name == name)
      return true;
  return false;
}
int main() {
  try {
    auto info = query("fn main():\n  earlier = 2\n  ear|ly\n  later = 3\n  later\n");
    require(has(info, "earlier") && info.names.size() == 1, "partial prefix and suffix");
    require(info.replacement.end - info.replacement.begin == 5, "whole identifier replacement");
    info = query(
        "fn helper(value:int):\n  value\nfn main():\n  earlier = 2\n  |\n  later = 3\n  later\n");
    require(has(info, "earlier") && has(info, "helper") && !has(info, "later"), "binding order");
    info = query("fn helper(value:int):\n  val|\nfn main():\n  0\n");
    require(has(info, "value") && info.names[0].kind == ScopeNameKind::Parameter, "parameter");
    info = query("fn helper(value:int):\n  value\nfn main():\n  hel|(2)\n");
    require(has(info, "helper"), "call callee");
    info = query("fn helper(value:int):\n  value\nfn main():\n  2 -> hel|\n");
    require(has(info, "helper"), "pipeline callee");
    info =
        query("fn main():\n  earlier = 1\n  values = [1, 2]\n  map(values, fn(value:int): ear|)\n");
    require(has(info, "earlier"), "lambda captures");
    info = query(
        "fn main():\n  visible = 1\n  if true:\n    hidden = 1\n    hidden\n  else:\n    |\n");
    require(has(info, "visible") && !has(info, "hidden"), "branch isolation");
    info = query("fn helper(value:int):\n  value\nfn main():\n  map([1,2], hel|)\n");
    require(has(info, "helper"), "named map callback");
    info = query("fn helper(value:int):\n  value\nfn main():\n  helper = 2\n  hel|\n");
    require(info.names.size() == 1 && info.names[0].kind == ScopeNameKind::Variable,
            "local shadows function");
    info = query("fn main():\n  value = 1\n  map([1,2], fn(value:int): val|)\n");
    require(info.names.size() == 1 && info.names[0].kind == ScopeNameKind::Parameter,
            "lambda parameter shadows capture");
    info = query("enum Choice:\n  Some:\n    value:int\n  Other\nfn inspect(choice:Choice):\n"
                 "  match choice:\n    Choice.Some {value:item}:\n      ite|\n"
                 "    Choice.Other: 0\nfn main():\n  0\n");
    require(has(info, "item"), "match binding");
    info = query("fn main():\n  parallel:\n    available = 1\n    av|\n");
    require(has(info, "available"), "parallel suite");
    info = query("type Point:\n  value:int\nfn main():\n  Poi|\n");
    require(info.names.size() == 1 && info.names[0].kind == ScopeNameKind::Type,
            "nominal constructor name");
    const std::string broken = "fn main():\n  earlier=1\n  missing\n";
    const auto site = broken.find("missing");
    ScopeCompletion scope{{site, site + 7}, "ear", {}, false, false};
    auto checked =
        check(parse(std::make_shared<const SourceFile>("scope.k", broken)), {}, {false, &scope});
    require(!checked.module && scope.reached && scope.names.size() == 1,
            "scope names must not publish partial IR");
    info = query("fn main():\n  broken = unknown\n  bro|\n");
    require(info.names.empty(), "prior errors fail closed");
    require(query("fn main():\n  value = 1\n  \"val|\"\n").names.empty(), "string position");
    require(query("fn main():\n  value = 1\n  # val|\n  value\n").names.empty(),
            "comment position");
    require(query("fn main():\n  value = 1\n  value.val|\n").names.empty(), "member not lexical");
    info = query("type Point:\n  x:int\n  y:int\nfn main():\n  p=Point {x:1,y:2}\n  p.|\n");
    require(info.names.size() == 2 && info.names[0].name == "x" && info.names[1].name == "y" &&
                info.names[0].kind == ScopeNameKind::Field,
            "completed record fields at a dot");
    info = query("type Box:\n  value:int\nfn main():\n  b=Box {value:1}\n  b.val|ue\n");
    require(info.names.size() == 1 && has(info, "value") &&
                info.replacement.end - info.replacement.begin == 5,
            "member prefix replaces whole suffix");
    info = query("type Inner:\n  value:int\ntype Outer:\n  inner:Inner\n"
                 "fn main():\n  o=Outer {inner:Inner {value:1}}\n  o.inner.|\n");
    require(info.names.size() == 1 && has(info, "value"), "nested receiver");
    info = query("type Box:\n  value:int\nfn make():\n  Box {value:1}\nfn main():\n  make().|\n");
    require(info.names.size() == 1 && has(info, "value"), "call result receiver");
    info = query("type Box:\n  value:int\nfn main():\n  boxes=[Box {value:1}]\n  boxes[0].|\n");
    require(info.names.size() == 1 && has(info, "value"), "indexed record receiver");
    info = query("type Box:\n  value:int\nfn inspect(b:Box):\n  map([1],fn(item:int):b.|)\nfn "
                 "main():\n  0\n");
    require(info.names.size() == 1 && has(info, "value"), "captured record receiver");
    require(
        query("type Box:\n  value:int\nfn main():\n  b=Box {value:1}\n  broken=unknown\n  b.|\n")
            .names.empty(),
        "prior errors block member names");
    require(query("type Box:\n  value:int\nfn main():\n  b=Box {value:1}\n  b.missing.|\n")
                .names.empty(),
            "invalid receiver chain fails closed");
    require(query("enum Choice:\n  Some:\n    value:int\n  Other\nfn inspect(c:Choice):\n  c.|\nfn "
                  "main():\n  0\n")
                .names.empty(),
            "unmatched enum payloads stay hidden");
    // A requested member snapshot still never publishes partial typed IR.
    const std::string member =
        "type Box:\n  value:int\nfn main():\n  b=Box {value:1}\n  b.missing\n";
    const auto memberSite = member.find("missing");
    ScopeCompletion members{{memberSite, memberSite + 7}, "", {}, false, false};
    auto failed = check(parse(std::make_shared<const SourceFile>("members.k", member)), {},
                        {false, &members});
    require(!failed.module && members.reached && members.names.size() == 1 &&
                members.names[0].kind == ScopeNameKind::Field,
            "member names do not expose partial IR");
    require(query("fn main():\n  missing(|\n").names.empty(),
            "unknown callee provides no completion after recovery");
    const std::string combine = "fn combine(left:int,right:int)->int:\n  left+right\n";
    info = query(combine + "fn main():\n  value=2\n  combine(va|\n");
    require(info.names.size() == 1 && has(info, "value"), "first argument in trailing open call");
    require(info.replacement.end - info.replacement.begin == 2,
            "repair does not extend replacement over parentheses");
    info = query(combine + "fn main():\n  value=2\n  combine(va|)\n");
    require(has(info, "value"), "balanced first argument with remaining missing parameter");
    info = query(combine + "fn main():\n  value=2\n  combine(1, va|\n");
    require(has(info, "value"), "later argument in open call");
    info = query(combine + "fn main():\n  value=2\n  combine(|\n");
    require(has(info, "value") && has(info, "combine"), "empty first argument in open call");
    info = query(combine + "fn main():\n  value=2\n  combine(1, combine(va|\n");
    require(has(info, "value"), "nested trailing open calls");
    info = query(combine + "fn main():\n  value=2\n  callback=combine\n  callback(va|\n");
    require(has(info, "value"), "bound callback with missing later parameters");
    info = query(combine +
                 "fn make()->fn(int,int)->int:\n  combine\nfn main():\n  value=2\n  make()(va|\n");
    require(has(info, "value"), "factory result with missing later parameters");
    info = query(combine + "fn main():\n  value=2\n  1 -> combine(va|\n");
    require(has(info, "value"), "piped open call");
    info = query(combine + "type Reading:\n  value:int\nfn main():\n  reading=Reading {value:2}\n  "
                           "combine(reading.va|\n");
    require(info.names.size() == 1 && has(info, "value") &&
                info.names[0].kind == ScopeNameKind::Field,
            "member completion in first argument with remaining parameters");
    require(query(combine + "fn main():\n  value=2\n  combine(missing, va|\n").names.empty(),
            "invalid supplied argument blocks open-call completion");
    require(query(combine + "fn main():\n  value=2\n  combine(\"bad\", va|\n").names.empty(),
            "wrong earlier argument type blocks completion");
    require(query(combine + "fn main():\n  value=2\n  missing(va|\n").names.empty(),
            "unknown callee blocks open-call completion");
    require(query(combine + "fn main():\n  value=2\n  callback=1\n  callback(va|\n").names.empty(),
            "noncallable value blocks open-call completion");
    require(query("fn broken(a:int,b:int)->int:\n  missing\nfn main():\n  value=2\n  broken(va|\n")
                .names.empty(),
            "invalid named callee body blocks completion");
    info = query(combine + "fn main():\n  value=2\n  combine(va|Suffix\n");
    require(has(info, "value") && info.replacement.end - info.replacement.begin == 8,
            "final argument suffix is preserved by EOF repair");
    require(query(combine + "fn main():\n  value=2\n  combine(va|\n  missing\n").names.empty(),
            "later source is not hidden");
    require(query(combine + "fn main():\n  value=2\n  combine([va|\n").names.empty(),
            "unclosed array is not repaired");
    require(query(combine + "fn main():\n  value=2\n  combine({va|\n").names.empty(),
            "unclosed record is not repaired");
    require(query(combine + "fn main():\n  value=2\n  combine(va]|\n").names.empty(),
            "mismatched delimiters rejected");
    require(query(combine + "fn main():\n  value=2\n  ((((((((combine(va|\n").names.empty(),
            "eight-parenthesis recovery bound retained");
    info = query("fn inspect(value: int|Suffix):\n  0\nfn main():\n  0\n");
    require(info.names.size() == 5 && has(info, "int") && has(info, "int8") && has(info, "int64") &&
                info.names[0].kind == ScopeNameKind::Type,
            "parameter numeric types and aliases");
    require(info.replacement.end - info.replacement.begin == 9,
            "type completion replaces identifier suffix");
    info = query("type Reading:\n  value:int\nenum Choice:\n  None\nfn make()->Rea|Suffix:\n  "
                 "0\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "Reading"), "nominal return type");
    info = query("type Reading:\n  value:int\nfn main():\n  Reading=2\n  value:Rea|=0\n  0\n");
    require(info.names.size() == 1 && has(info, "Reading") &&
                info.names[0].kind == ScopeNameKind::Type,
            "annotation uses type namespace despite value shadow");
    info = query("type Reading:\n  value: flo|Suffix\nfn main():\n  0\n");
    require(info.names.size() == 3 && has(info, "float32") && has(info, "float64") &&
                has(info, "float"),
            "record field types");
    info = query("enum Choice:\n  Some:\n    value: uint|\n  None\nfn main():\n  0\n");
    require(info.names.size() == 5 && has(info, "uint64"), "enum payload field types");
    info = query("fn inspect(values:[Bo|]):\n  0\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "Bool"), "array element type");
    info = query("fn inspect(values:[|]):\n  0\nfn main():\n  0\n");
    require(has(info, "String") && has(info, "int32") && !has(info, "TaskError") &&
                !has(info, "Tensor"),
            "array suggestions exclude unsupported nested containers and TaskError");
    info = query(
        "type Reading:\n  value:int\nfn inspect(values:Tensor[|,?]):\n  0\nfn main():\n  0\n");
    require(info.names.size() == 13 && has(info, "float32") && !has(info, "Bool") &&
                !has(info, "Reading"),
            "tensor element slot offers only numeric types");
    info = query("fn inspect(callback:fn(in|)->int):\n  0\nfn main():\n  0\n");
    require(has(info, "int32") && info.names.size() == 5, "nested callable parameter type");
    info = query("fn inspect(callback:fn(int)->flo|):\n  0\nfn main():\n  0\n");
    require(has(info, "float32") && info.names.size() == 3, "nested callable return type");
    info = query("fn main():\n  callback=fn(value: ui|):value\n  0\n");
    require(has(info, "uint32") && info.names.size() == 5, "anonymous parameter annotation");
    info = query("fn main():\n  if true:\n    value: in|=1\n    0\n  else:\n    0\n");
    require(has(info, "int8"), "nested suite binding annotation");
    info = query("fn helper(value:int):\n  value\nfn main(value: |):\n  0\n");
    require(has(info, "Bool") && has(info, "Tensor") && has(info, "TaskError") &&
                !has(info, "helper") && !has(info, "value"),
            "empty annotation offers only type names");
    require(query("fn main():\n  broken=missing\n  value:in|=1\n  0\n").names.empty(),
            "earlier semantic errors block type completion");
    require(query("fn main(value:int) ! Ba|:\n  value\n").names.empty(),
            "unknown error prefix has no candidates");
    const std::string errors = "error ParseError:\n  Bad\nerror ReadError:\n  Missing\n"
                               "type Problem = ParseError | ReadError\n"
                               "type Plain:\n  value:int\nenum Ordinary:\n  Case\n";
    info = query(errors + "fn inspect()->int ! Pa|Suffix:\n  0\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "ParseError") &&
                info.names[0].kind == ScopeNameKind::Type,
            "declared function error channel");
    info = query(errors + "fn inspect()->int ! |:\n  0\nfn main():\n  0\n");
    require(info.names.size() == 4 && has(info, "ParseError") && has(info, "ReadError") &&
                has(info, "Problem") && has(info, "TaskError") && !has(info, "Plain") &&
                !has(info, "int"),
            "error channel offers only error families and aliases");
    info = query(errors + "fn inspect()->int ! (ParseError | Re|):\n  0\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "ReadError"), "union channel member");
    info = query(errors + "type Combined = P|\nfn main():\n  0\n");
    require(info.names.size() == 2 && has(info, "ParseError") && has(info, "Problem"),
            "error alias member");
    info = query(errors + "type Combined = |\nfn main():\n  0\n");
    require(has(info, "Problem") && has(info, "TaskError") && !has(info, "Combined"),
            "currently resolving alias is excluded from its own completion");
    info = query(errors + "fn inspect(callback:fn(int)->int ! P|):\n  0\nfn main():\n  0\n");
    require(has(info, "Problem") && has(info, "ParseError"), "nested callable channel");
    info = query(errors +
                 "fn make()->(fn(int)->int ! Re|):\n  fn(value:int):value\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "ReadError"), "returned callable channel");
    info = query(errors + "fn main():\n  callback=fn(value:int)->int ! Pa|:value\n  0\n");
    require(info.names.size() == 1 && has(info, "ParseError"), "anonymous error channel");
    info = query(errors +
                 "fn main():\n  ParseError=2\n  callback=fn(value:int)->int ! Pa|:value\n  0\n");
    require(info.names.size() == 1 && has(info, "ParseError"),
            "error type namespace independent from values");
    require(query(errors +
                  "fn main():\n  broken=missing\n  callback=fn(value:int)->int ! Pa|:value\n  0\n")
                .names.empty(),
            "earlier binding error blocks anonymous channel suggestions");
    require(has(query(errors + "fn inspect()->int ! Pa|\n"), "ParseError"),
            "unfinished declaration retains checked error names");
    std::string manyErrors;
    for (int i = 0; i < 300; ++i)
      manyErrors += "error Error" + std::to_string(1000 + i).substr(1) + ":\n  Bad\n";
    info = query(manyErrors + "fn inspect()->int ! Error|:\n  0\nfn main():\n  0\n");
    require(info.names.size() == 256 && info.incomplete, "bounded error-channel list");
    std::string manyTypes;
    for (int i = 0; i < 300; ++i) {
      const auto suffix = std::to_string(1000 + i).substr(1);
      manyTypes += "type Type" + suffix + ":\n  value:int\n";
    }
    info = query(manyTypes + "fn main(value:Type|):\n  0\n");
    require(info.names.size() == 256 && info.incomplete && has(info, "Type000") &&
                !has(info, "Type299"),
            "type suggestions use sorted bounded lists");
    info = query(manyTypes + "fn main(value:Type29|):\n  0\n");
    require(info.names.size() == 10 && !info.incomplete, "type prefix narrows list before limit");
    std::string many = "fn main():\n";
    for (int i = 0; i < 300; ++i)
      many += "  candidate" + std::to_string(i) + " = 1\n";
    info = query(many + "  can|\n");
    require(info.names.size() == 256 && info.incomplete, "bounded list");
    info = query(many + "  candidate29|\n");
    require(info.names.size() == 11 && !info.incomplete, "narrowed list");
    std::string record = "type Many:\n";
    for (int i = 0; i < 300; ++i)
      record += "  candidate" + std::to_string(i) + ":int\n";
    info = query(record + "fn inspect(value:Many):\n  value.can|\nfn main():\n  0\n");
    require(info.names.size() == 256 && info.incomplete, "bounded record fields");
    info = query(record + "fn inspect(value:Many):\n  value.candidate29|\nfn main():\n  0\n");
    require(info.names.size() == 11 && !info.incomplete, "narrowed record fields");
    const std::string choices = "enum Choice:\n  Some:\n    value:int\n  Other\n";
    info = query(choices + "fn main():\n  Choice.|\n");
    require(info.names.size() == 2 && info.names[0].name == "Other" &&
                info.names[1].name == "Some" && info.names[0].kind == ScopeNameKind::EnumCase,
            "enum constructor cases");
    info = query(choices + "fn main():\n  Choice.So|me {value:1}\n");
    require(info.names.size() == 1 && has(info, "Some") &&
                info.replacement.end - info.replacement.begin == 4,
            "payload constructor prefix and suffix");
    info = query(choices + "fn inspect(c:Choice):\n  match c:\n    Choice.Other: 0\n    Choice.|: "
                           "1\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "Some"), "remaining match cases");
    info = query(choices + "fn inspect(c:Choice):\n  match c:\n    Choice.So|me {value:item}: "
                           "item\n    Choice.Other: 0\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "Some"), "payload match site");
    require(query(choices + "enum Wrong:\n  Nope\nfn inspect(c:Choice):\n  match c:\n    Wrong.|: "
                            "0\nfn main():\n  0\n")
                .names.empty(),
            "wrong match family is not suggested");
    require(query(choices + "fn main():\n  Choice=1\n  Choice.|\n").names.empty(),
            "local shadows enum family");
    info =
        query(choices + "type Box:\n  value:int\nfn main():\n  Choice=Box {value:1}\n  Choice.|\n");
    require(info.names.size() == 1 && has(info, "value") &&
                info.names[0].kind == ScopeNameKind::Field,
            "record binding shadows enum type");
    require(query(choices + "fn main():\n  broken=missing\n  Choice.|\n").names.empty(),
            "prior errors block enum candidates");
    require(query(choices +
                  "fn inspect(c:Choice):\n  match c:\n    _: 0\n    Choice.|: 1\nfn main():\n  0\n")
                .names.empty(),
            "fallback blocks later candidates");
    info = query("error Problem:\n  Bad\n  Worse\nfn main():\n  Problem.|\n");
    require(info.names.size() == 2 && has(info, "Bad") && has(info, "Worse"), "error constructors");
    info = query("error First:\n  Bad\nerror Second:\n  Worse\ntype Both = First | Second\nfn "
                 "inspect(e:Both):\n  "
                 "match e:\n    First.|: 1\n    Second.Worse: 2\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "Bad"), "error union family identity");
    require(query("error First:\n  Bad\nerror Second:\n  Worse\ntype Both = First | Second\nfn "
                  "main():\n  Both.|\n")
                .names.empty(),
            "union constructors do not expose synthetic cases");
    info = query("fn inspect(e:TaskError):\n  match e:\n    TaskError.|: 0\nfn main():\n  0\n");
    require(info.names.size() == 4 && has(info, "Cancelled") && has(info, "Fault"),
            "built-in task error patterns");
    std::string enumeration = "enum Large:\n";
    for (int i = 0; i < 300; ++i)
      enumeration += "  Candidate" + std::to_string(i) + "\n";
    info = query(enumeration + "fn main():\n  Large.|\n");
    require(info.names.size() == 256 && info.incomplete, "bounded enum candidates");
    info = query(enumeration + "fn main():\n  Large.Candidate29|\n");
    require(info.names.size() == 11 && !info.incomplete, "narrowed enum candidates");
    const std::string point = "type Point:\n  x:int\n  y:int\n";
    info = query(point + "fn main():\n  Point { | }\n");
    require(info.names.size() == 2 && has(info, "x") && has(info, "y") && info.appendFieldColon,
            "empty constructor label");
    info = query(point + "fn main():\n  Point { x:1, | }\n");
    require(info.names.size() == 1 && has(info, "y") && info.appendFieldColon,
            "exclude supplied constructor label");
    info = query(point + "fn main():\n  Point { |:0, y:2 }\n");
    require(info.names.size() == 1 && has(info, "x") && !info.appendFieldColon,
            "exclude later supplied label and preserve colon");
    info = query("type Box:\n  value:int\nfn main():\n  Box { va|lue:1 }\n");
    require(info.names.size() == 1 && has(info, "value") && !info.appendFieldColon &&
                info.replacement.end - info.replacement.begin == 5,
            "constructor suffix edit");
    info = query("type Box:\n  value:int\nfn main():\n  Box { va|: }\n");
    require(info.names.size() == 1 && has(info, "value") && !info.appendFieldColon,
            "missing value probe preserves existing colon");
    info = query(point + "fn main():\n  p=Point {x:1,y:2}\n  p with { | }\n");
    require(info.names.size() == 2 && info.appendFieldColon, "immutable update labels");
    info = query(point + "type Outer:\n  inner:Point\nfn main():\n  Outer {inner:Point { | }}\n");
    require(info.names.size() == 2 && has(info, "x") && !has(info, "inner"), "nested labels");
    info = query("enum Pick:\n  Some:\n    value:int\n  Other:\n    hidden:int\nfn main():\n  "
                 "Pick.Some { | }\n");
    require(info.names.size() == 1 && has(info, "value") && !has(info, "hidden"),
            "selected payload labels");
    require(query(point + "fn main():\n  Point { x:missing, | }\n").names.empty(),
            "prior field errors fail closed");
    require(query(point + "fn main():\n  Point=1\n  Point { | }\n").names.empty(),
            "shadowed constructor labels");
    require(query("enum Pick:\n  Some:\n    value:int\nfn main():\n  Pick.Missing { | }\n")
                .names.empty(),
            "unknown case labels fail closed");
    info = query("type Box:\n  value:int\nfn main():\n  earlier=1\n  Box {value:ear|}\n");
    require(has(info, "earlier") && !info.appendFieldColon, "initializer value remains lexical");
    require(query("fn main():\n  1 with { | }\n").names.empty(), "invalid update receiver");
    require(has(query(point + "fn main():\n  Point { |\n"), "x"),
            "unclosed constructor retains checked labels");
    info = query(record + "fn inspect(value:Many):\n  value with { | }\nfn main():\n  0\n");
    require(info.names.size() == 256 && info.incomplete, "bounded label list");
    info =
        query(record + "fn inspect(value:Many):\n  value with { candidate29| }\nfn main():\n  0\n");
    require(info.names.size() == 11 && !info.incomplete, "narrowed label list");
    const std::string patterns =
        "enum Parcel:\n  Packed:\n    value:int\n    count:int\n    note:int\n  Empty\n";
    const auto patternQuery = [&](const std::string &fields) {
      return query(patterns + "fn inspect(p:Parcel):\n  match p:\n    Parcel.Packed {" + fields +
                   "}: 0\n    Parcel.Empty: 0\nfn main():\n  0\n");
    };
    info = patternQuery(" | ");
    require(info.names.size() == 3 && has(info, "value") && !info.appendFieldColon &&
                info.names[0].kind == ScopeNameKind::Field,
            "empty shorthand pattern labels");
    info = patternQuery("va|Suffix");
    require(info.names.size() == 1 && has(info, "value") && !info.appendFieldColon &&
                info.replacement.end - info.replacement.begin == 8,
            "shorthand pattern suffix");
    info = patternQuery("va|:item");
    require(info.names.size() == 1 && has(info, "value") && !info.appendFieldColon,
            "explicit pattern binder preserved");
    info = patternQuery("va|:");
    require(info.names.size() == 1 && has(info, "value"), "bounded missing pattern binder");
    info = patternQuery("count:value, |, note:other");
    require(info.names.size() == 1 && has(info, "value"), "exclude labels rather than binders");
    require(patternQuery("value:it|em").names.empty(), "binder is not a field completion site");
    require(patternQuery("missing:x, |").names.empty(),
            "prior invalid pattern field blocks labels");
    require(query(patterns + "fn inspect(p:Parcel):\n  match p:\n    Parcel.Empty {|}: 0\n    "
                             "Parcel.Packed: 0\nfn main():\n  0\n")
                .names.empty(),
            "payloadless variant has no labels");
    require(query(patterns + "fn inspect(p:Parcel):\n  match p:\n    Parcel.Missing {|}: 0\nfn "
                             "main():\n  0\n")
                .names.empty(),
            "invalid pattern case has no labels");
    info = query(
        "error Problem:\n  Bad:\n    code:int\nerror Other:\n  Worse\ntype Both = Problem | Other\n"
        "fn inspect(e:Both):\n  match e:\n    Problem.Bad {co|}: 0\n    Other.Worse: 1\n"
        "fn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "code"), "error union pattern payload labels");
    info = query("fn inspect(e:TaskError):\n  match e:\n    TaskError.Fault {|}: 0\n    _: 1\n"
                 "fn main():\n  0\n");
    require(info.names.size() == 2 && has(info, "kind") && has(info, "message"),
            "task fault labels");
    info = query("fn inspect(e:TaskError):\n  match e:\n    TaskError.AdmissionRejected {re|}: 0\n"
                 "    _: 1\nfn main():\n  0\n");
    require(info.names.size() == 1 && has(info, "reason"), "task rejection labels");
    std::string manyPayloads = "enum LargePayload:\n  Some:\n";
    for (int i = 0; i < 300; ++i)
      manyPayloads += "    candidate" + std::to_string(i) + ":int\n";
    info = query(manyPayloads +
                 "fn inspect(p:LargePayload):\n  match p:\n    LargePayload.Some {|}: 0\n"
                 "fn main():\n  0\n");
    require(info.names.size() == 256 && info.incomplete, "bounded pattern payload labels");
    const auto importQuery = [&](std::string text, std::string library) {
      const auto cursor = text.find('|');
      text.erase(cursor, 1);
      const auto root = std::filesystem::temp_directory_path();
      return lsp::completeNames((root / "completion.k").string(), text, cursor, {},
                                {{(root / "completion-exports.k").string(), library}});
    };
    const std::string publicLibrary =
        "export fn double(v:int):\n  v*2\nfn hidden():\n  0\n"
        "export type Box:\n  value:int\nexport error Problem:\n  Bad\n"
        "export type Failure = Problem | TaskError\n";
    info =
        importQuery("from \"./completion-exports.k\" import |\nfn main():\n  0\n", publicLibrary);
    require(info.names.size() == 4 && has(info, "Box") && has(info, "double") &&
                has(info, "Failure") && !has(info, "hidden"),
            "public import namespace");
    require(info.names[0].kind == ScopeNameKind::Type &&
                info.names.back().kind == ScopeNameKind::Function,
            "import item kinds");
    info = importQuery("from \"./completion-exports.k\" import Box, do|Suffix as twice\n"
                       "fn main():\n  0\n",
                       publicLibrary);
    require(info.names.size() == 1 && has(info, "double") &&
                info.replacement.end - info.replacement.begin == 8,
            "import suffix and alias");
    require(importQuery("from \"./completion-exports.k\" import double as do|\n"
                        "fn main():\n  0\n",
                        publicLibrary)
                .names.empty(),
            "local alias declarations are not import-name sites");
    std::string manyExports;
    for (int i = 0; i < 300; ++i)
      manyExports += "export fn candidate" + std::to_string(i) + "():\n  0\n";
    info = importQuery("from \"./completion-exports.k\" import |\nfn main():\n  0\n", manyExports);
    require(info.names.size() == 256 && info.incomplete, "bounded exported names");
    const auto pathRoot = std::filesystem::temp_directory_path() / "kalandra-path-overlays";
    const auto pathQuery = [&](std::string text, std::vector<SourceOverlay> overlays) {
      const auto cursor = text.find('|');
      text.erase(cursor, 1);
      return lsp::completeNames((pathRoot / "main.k").string(), text, cursor, {},
                                std::move(overlays));
    };
    std::vector<SourceOverlay> pathOverlays{
        {(pathRoot / "math.k").string(), "export fn double(v:int):\n  v*2\n"},
        {(pathRoot / "other.k").string(), ""},
        {(pathRoot / ".hidden.k").string(), ""},
        {(pathRoot / "main.k").string(), ""},
        {(pathRoot / "foreign" / "hidden.k").string(), ""}};
    info = pathQuery("from \"./ma|Suffix.k\" import double\nfn main():\n  0\n", pathOverlays);
    require(info.names.size() == 1 && has(info, "./math.k") &&
                info.names[0].kind == ScopeNameKind::File &&
                info.replacement.end - info.replacement.begin == 12,
            "unsaved path completion replaces the full inner string: " +
                std::to_string(info.names.size()) + " candidates, span " +
                std::to_string(info.replacement.end - info.replacement.begin));
    info = pathQuery("export from \"./|\" import double\nfn main():\n  0\n", pathOverlays);
    require(info.names.size() == 2 && has(info, "./math.k") && has(info, "./other.k"),
            "unsaved public import path candidates and exclusions");
    require(pathQuery("fn main():\n  \"./ma|\"\n", pathOverlays).names.empty(),
            "ordinary strings are not import paths");
    require(pathQuery("from \"ma|\" import double\nfn main():\n  0\n", pathOverlays).names.empty(),
            "bare package names are not relative path candidates");
    require(pathQuery("from \"./ma|", pathOverlays).names.empty(),
            "unterminated import strings are not repaired");
    require(pathQuery("from \"./ma|\" import double\nfn main(\n", pathOverlays).names.empty(),
            "path completion requires parsed import syntax");
    std::vector<SourceOverlay> tooManyPaths(ModuleLimits{}.maxFiles + 1,
                                            {(pathRoot / "math.k").string(), ""});
    require(pathQuery("from \"./|\" import double\nfn main():\n  0\n", tooManyPaths).names.empty(),
            "path overlays retain the module file budget");
    info = query("fn entry():\n  ma|Suffix([1,2], fn(v:int):v*2)\n");
    require(info.names.size() == 2 && has(info, "map") && has(info, "matmul") &&
                info.names[0].kind == ScopeNameKind::Function &&
                info.replacement.end - info.replacement.begin == 8,
            "builtin callee names and full suffix replacement");
    info = query("fn entry():\n  [1,2] -> par|Suffix(fn(v:int):v*2)\n");
    require(info.names.size() == 1 && has(info, "parallelMap"), "piped builtin call target");
    info = query("fn entry():\n  [1,2] -> le|\n");
    require(info.names.size() == 1 && has(info, "length"), "bare piped builtin target");
    info = query("fn entry():\n  in|Suffix(1)\n");
    require(info.names.size() == 5 && has(info, "int") && has(info, "int8") && has(info, "int64"),
            "numeric conversion call targets reuse concrete widths and aliases");
    require(has(query("fn entry():\n  ma|\n"), "match") &&
                !has(query("fn entry():\n  ma|\n"), "matmul"),
            "expression keywords are suggested without call-only builtins");
    require(!has(query("fn entry():\n  map([1,2], ma|)\n"), "map"),
            "builtin call forms are not first-class map callbacks");
    info = query("fn entry():\n  map=fn(v:int):v*2\n  ma|(1)\n");
    require(info.names.size() == 2 && info.names[0].name == "map" &&
                info.names[0].kind == ScopeNameKind::Variable,
            "lexical function binding hides builtin item");
    info = query("fn examine(int32:int):\n  in|Suffix(1)\nfn entry():\n  0\n");
    require(info.names.size() == 5 && info.names[2].name == "int32" &&
                info.names[2].kind == ScopeNameKind::Parameter,
            "lexical numeric-name shadow retains its own kind");
    require(query("fn entry():\n  missing\n  ma|([1,2])\n").names.empty(),
            "prior errors still suppress builtin suggestions");
    info = query("fn entry():\n  co|([1], [2])\n  missing\n");
    require(info.names.size() == 1 && has(info, "concat"), "later errors do not erase names");
    CheckOptions placement;
    placement.placement = true;
    info = query("fn main():\n  target=devices.g|Suffix(1)\n  0\n", placement);
    require(info.names.size() == 1 && has(info, "gpu") &&
                info.names[0].kind == ScopeNameKind::Function &&
                info.replacement.end - info.replacement.begin == 7,
            "device selector suffix edits only the member");
    info = query("fn main():\n  target=devices.|()\n  0\n", placement);
    require(info.names.size() == 2 && has(info, "cpu") && has(info, "gpu"),
            "empty device selector member");
    info = query("fn main():\n  devices.|\n", placement);
    require(info.names.size() == 2 && has(info, "gpu"),
            "explicit selector prefix before adding parentheses");
    info = query("fn main():\n  target=devices.c|()\n  0\n", placement);
    require(info.names.size() == 1 && has(info, "cpu"), "CPU selector prefix");
    require(query("fn main():\n  target=devices.g|()\n  0\n").names.empty(),
            "device selector completion requires placement mode");
    require(query("fn main():\n  devices=1\n  target=devices.g|()\n  0\n", placement).names.empty(),
            "local devices shadow suppresses selectors");
    require(query("fn inspect(devices:int):\n  devices.g|()\nfn main():\n  0\n", placement)
                .names.empty(),
            "parameter devices shadow suppresses selectors");
    require(query("fn main():\n  missing\n  target=devices.g|()\n  0\n", placement).names.empty(),
            "prior errors prevent selector completion");
    require(query("fn main():\n  target=other.g|()\n  0\n", placement).names.empty(),
            "arbitrary receivers do not become device namespaces");
    info = query("fn main():\n  target=devices.g|()\n  missing\n", placement);
    require(info.names.size() == 1 && has(info, "gpu"), "later errors do not erase selector names");
    const std::string transferStart =
        "fn main()->int ! TaskError:\n  try parallel:\n    values=tensor([1,2])\n";
    const std::string transferCall =
        "    resident=try await tra|Suffix(values,devices.gpu(1))\n"
        "    host=try await transfer(resident,devices.cpu())\n    reduceSum(host)\n";
    info = query(transferStart + transferCall, placement);
    require(info.names.size() == 2 && has(info, "transfer") && has(info, "transpose") &&
                info.names[0].kind == ScopeNameKind::Function &&
                info.names[1].kind == ScopeNameKind::Function &&
                info.replacement.end - info.replacement.begin == 9,
            "transfer and transpose share a prefix and replace the whole call name");
    auto transferOnly = transferCall;
    transferOnly.replace(transferOnly.find("tra|Suffix"), 10, "transf|Suffix");
    info = query(transferStart + transferOnly, placement);
    require(info.names.size() == 1 && has(info, "transfer") &&
                info.names[0].kind == ScopeNameKind::Function &&
                info.replacement.end - info.replacement.begin == 12,
            "a distinct transfer prefix selects the owning-group builtin");
    require(!has(query(transferStart + transferCall), "transfer"),
            "CPU mode does not suggest the transfer builtin");
    require(
        !has(query("fn main():\n  tra|Suffix(tensor([1]),devices.gpu())\n", placement), "transfer"),
        "transfer completion requires an owning group");
    require(!has(query(transferStart + "    tra|\n", placement), "transfer"),
            "transfer is not a first-class value");
    require(!has(query(transferStart + "    map([1],tra|)\n", placement), "transfer"),
            "transfer is not a callback value");
    require(!has(query(transferStart + "    spawn tra|(values,devices.gpu())\n", placement),
                 "transfer"),
            "transfer is not a spawn entry");
    require(!has(query(transferStart + "    spawn tra|(values,devices.gpu()) on devices.gpu()\n",
                       placement),
                 "transfer"),
            "transfer is not a placed spawn entry");
    require(!has(query(transferStart + "    callback=fn(v:int):tra|(v,devices.gpu())\n    0\n",
                       placement),
                 "transfer"),
            "a closure does not inherit the enclosing owning group");
    info = query(transferStart + "    resident=try await (values -> tra|(devices.gpu()))\n    0\n",
                 placement);
    require(has(info, "transfer"), "piped transfer call target");
    info = query(transferStart + "    values -> tra|\n", placement);
    require(has(info, "transfer"), "bare pipeline transfer target");
    require(query(transferStart + "    missing\n" + transferCall, placement).names.empty(),
            "prior errors suppress transfer candidates");
    info = query(transferStart + transferCall + "    missing\n", placement);
    require(has(info, "transfer"), "later errors preserve captured transfer completion");
    info = query(transferStart + "    transfer=1\n    transf|(1)\n", placement);
    require(info.names.size() == 1 && has(info, "transfer") &&
                info.names[0].kind == ScopeNameKind::Variable,
            "local transfer binding keeps its own item kind");
    info = query("fn transfer(value:int):\n  value\nfn main():\n  transf|(1)\n");
    require(info.names.size() == 1 && has(info, "transfer") &&
                info.names[0].kind == ScopeNameKind::Function,
            "CPU-mode source functions retain normal transfer completion");
    info = query(transferStart + "    try await tra|(values,devices.gpu()\n", placement);
    require(has(info, "transfer"), "callee recovery preserves trailing selector arguments");
    info = query(transferStart + "    try await (values -> tra|\n", placement);
    require(has(info, "transfer"), "end-of-buffer pipeline recovery retains placement scope");
    info = query(combine + "fn main():\n  com|Suffix(1,2\n");
    require(info.names.size() == 1 && has(info, "combine") &&
                info.replacement.end - info.replacement.begin == 9,
            "callee completion preserves complete arguments before appending trailing parentheses");
    info = query(combine + "fn main():\n  com|(1,combine(2,3\n");
    require(has(info, "combine"), "nested missing call parentheses at EOF");
    info = query("fn main():\n  [1,2] -> ma|Suffix(fn(v:int):v*2\n");
    require(has(info, "map") && has(info, "matmul"), "builtin pipeline callee recovery");
    info = query("fn main():\n  target=devices.g|Suffix(1\n  0\n", placement);
    require(info.names.empty(), "later statements do not become hidden selector arguments");
    info = query("fn main():\n  (devices.g|Suffix(1\n", placement);
    require(has(info, "gpu"), "qualified selector callee retains member-only replacement");
    info = query("type Box:\n  callback:fn(int)->int\nfn main():\n"
                 "  box=Box {callback:fn(v:int):v+1}\n  box.ca|Suffix(2\n");
    require(has(info, "callback") && info.names[0].kind == ScopeNameKind::Field,
            "checked record callee member completion in open call");
    require(query(combine + "fn main():\n  unknown.ca|Suffix(1,2\n").names.empty(),
            "invalid member receiver suppresses callee repair");
    require(query(combine + "fn main():\n  broken=missing\n  com|(1,2\n").names.empty(),
            "prior semantic errors suppress recovered callees");
    require(has(query(combine + "fn main():\n  com|(1 +,2\n"), "combine"),
            "checked callee survives malformed later arguments");
    require(has(query(combine + "fn main():\n  com|([1,2\n"), "combine"),
            "checked callee survives an unfinished array");
    require(query(combine + "fn main():\n  com|({value:1\n").names.empty(),
            "callee recovery does not close records");
    require(has(query(combine + "fn main():\n  com|(1,2]\n"), "combine"),
            "checked callee survives a later mismatched delimiter");
    require(query(combine + "fn main():\n  com|(\"unterminated\n").names.empty(),
            "callee recovery rejects other lexical errors");
    require(query(combine + "fn main():\n  ((((((((com|(1,2\n").names.empty(),
            "callee recovery retains eight-parenthesis bound");
    info = query(combine + "fn main():\n  value=2\n  combine(1,va|Suffix\n");
    require(info.names.size() == 1 && has(info, "value"),
            "last argument before absent closing parenthesis");
    info = query(combine + "fn main():\n  value=2\n  combine(1,((((va|Suffix\n");
    require(has(info, "value"), "grouped final argument retains EOF repair");
    info = query(combine + "fn main():\n  value=2\n  callback=combine\n  callback(1,va|Suffix\n");
    require(has(info, "value"), "checked function-value callee supplies final argument context");
    const std::string openField =
        "type Reading:\n  value:int\n" + combine +
        "fn main():\n  reading=Reading {value:2}\n  combine(1,reading.va|Suffix\n";
    info = query(openField);
    require(info.names.size() == 1 && has(info, "value") &&
                info.names[0].kind == ScopeNameKind::Field &&
                info.replacement.end - info.replacement.begin == 8,
            "final record field suffix uses checked receiver and field-only edit");
    require(query(combine + "fn main():\n  value=2\n  combine(\"bad\",va|Suffix\n").names.empty(),
            "wrong earlier argument type suppresses suffix recovery");
    require(query(combine + "fn main():\n  value=2\n  missing(1,va|Suffix\n").names.empty(),
            "unknown callee suppresses suffix recovery");
    require(query(combine + "fn main():\n  value=2\n  callback=1\n  callback(1,va|Suffix\n")
                .names.empty(),
            "noncallable value suppresses suffix recovery");
    require(query(combine + "fn main():\n  combine(1,missing.va|Suffix\n").names.empty(),
            "unknown field receiver suppresses suffix recovery");
    info = query(combine + "fn main():\n  value=2\n  combine(va|Suffix,1\n");
    require(has(info, "value"), "earlier argument suffix before later supplied arguments");
    info = query(combine + "fn main():\n  value=2\n  combine(1,2+va|Suffix\n");
    require(has(info, "value"), "arithmetic argument suffix receives EOF completion");
    const std::string triple = "fn triple(first:int,second:int,third:int):\n  first+second+third\n";
    info = query(triple + "fn main():\n  value=1\n  triple(va|Suffix,2\n");
    require(info.names.size() == 1 && has(info, "value") &&
                info.replacement.end - info.replacement.begin == 8,
            "earlier argument completion retains missing later parameter context");
    info = query(triple + "fn main():\n  value=1\n  triple(va|Suffix,2)\n");
    require(has(info, "value"), "balanced call with missing later parameters");
    info = query(triple + "fn main():\n  value=1\n  callback=triple\n  callback(va|Suffix,2\n");
    require(has(info, "value"), "bound callable checks earlier argument before missing parameters");
    info =
        query(triple + "fn main():\n  value=1\n  callbacks=[triple]\n  callbacks[0](va|Suffix,2\n");
    require(has(info, "value"), "indexed callable retains earlier argument completion");
    info = query(triple + "fn main():\n  value=1\n  1 -> triple(va|Suffix,2\n");
    require(has(info, "value"), "pipeline retains implicit argument before edited argument");
    info = query("type Reading:\n  value:int\n" + triple +
                 "fn main():\n  reading=Reading {value:1}\n  triple(reading.va|Suffix,2\n");
    require(info.names.size() == 1 && info.names[0].kind == ScopeNameKind::Field &&
                has(info, "value"),
            "earlier field argument requires checked receiver");
    info = query(triple + "fn main():\n  value=1\n  triple(va|Suffix,\"bad\"\n");
    require(has(info, "value"), "later argument errors do not erase captured completion");
    require(query(triple + "fn main():\n  value=1\n  triple(\"bad\",va|Suffix,2\n").names.empty(),
            "wrong argument prefix still suppresses earlier-slot completion");
    require(query(triple + "fn main():\n  value=1\n  missing(va|Suffix,2\n").names.empty(),
            "unknown callee cannot expose earlier-slot names");
    require(query(triple + "fn main():\n  callback=1\n  value=1\n  callback(va|Suffix,2\n")
                .names.empty(),
            "noncallable binding cannot expose earlier-slot names");
    require(query(triple + "fn main():\n  triple(missing.va|Suffix,2\n").names.empty(),
            "unknown earlier field receiver suppresses suggestions");
    info = query(triple + "fn main():\n  value=1\n  triple(1+va|Suffix,2\n");
    require(has(info, "value"), "arithmetic earlier argument before missing later parameter");
    for (const auto *operation : {"+", "-", "*", "/", "%"}) {
      info = query(combine + "fn main():\n  value=2\n  combine(1,4" + std::string(operation) +
                   "va|Suffix\n");
      require(info.names.size() == 1 && has(info, "value"),
              "arithmetic operator suffix completion");
    }
    info = query(combine + "fn main():\n  value=2\n  combine(1,-va|Suffix\n");
    require(has(info, "value"), "numeric unary operand completion");
    info = query(combine + "fn main():\n  value=2\n  combine(1,(2+3)*-va|Suffix\n");
    require(has(info, "value"), "nested arithmetic operand completion");
    info = query(combine + "fn main():\n  value=2\n  combine(va|Suffix+3,1\n");
    require(has(info, "value"), "left arithmetic operand preserves later text");
    info = query("type Reading:\n  value:int\n" + combine +
                 "fn main():\n  reading=Reading {value:2}\n  combine(1,2+reading.va|Suffix\n");
    require(info.names.size() == 1 && has(info, "value") &&
                info.names[0].kind == ScopeNameKind::Field,
            "arithmetic field suffix requires checked record receiver");
    const std::string boolConsumer = "fn choose(value:Bool):\n  value\n";
    for (const auto *operation : {"==", "!=", "<", ">", "<=", ">="}) {
      info = query(boolConsumer + "fn main():\n  value=2\n  choose(1" + std::string(operation) +
                   "va|Suffix\n");
      require(has(info, "value"), "comparison argument operand completion");
    }
    require(query(combine + "fn main():\n  value=2\n  combine(\"bad\",2+va|Suffix\n").names.empty(),
            "invalid earlier call argument blocks arithmetic suffix completion");
    require(query(combine + "fn main():\n  combine(1,2+missing.va|Suffix\n").names.empty(),
            "arithmetic member still requires a valid receiver");
    require(query(combine + "fn main():\n  value=2\n  combine(1,2+*va|Suffix\n").names.empty(),
            "malformed arithmetic is not repaired");
    std::cout << "Native scope completion passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
