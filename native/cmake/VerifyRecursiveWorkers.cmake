include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
function(WORKER NAME SOURCE EXPECTED)
  COMPILE("${NAME}" "${SOURCE}" --test-entry main)
  EXPECT("${NAME}" "${EXPECTED}")
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    "${SOURCE_DIR}/tests/RecursiveContinuationRuntimeTests.cpp" "${OUTPUT_DIR}/${NAME}.ll"
    ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/${NAME}-pools")
  RUN_OK("${OUTPUT_DIR}/${NAME}-pools" "${EXPECTED}")
endfunction()
set(PREFIX "enum List:\n  Nil\n  Cons:\n    head:int\n    tail:List\n")
set(SUM "fn sum(items:List)->int:\n  match items:\n    List.Nil:0\n    List.Cons {head,tail}:head+sum(tail)\n")
set(IDENTITY "fn identity(items:List)->List:\n  items\n")
set(CHILD "fn child(items:List)->List ! TaskError:\n  try parallel:\n    job=spawn identity(items)\n    first=try await job\n    again=try await job\n    again\n")
WORKER(nested "${PREFIX}${SUM}${IDENTITY}${CHILD}fn main()->int ! TaskError:\n  tail=List.Cons {head:2,tail:List.Nil}\n  p=try parallel:\n    job=spawn child(List.Cons {head:40,tail:tail})\n    first=try await job\n    again=try await job\n    again\n  sum(p)\n" 42)
WORKER(created "${PREFIX}${SUM}fn make(n:int)->List:\n  if n==0:List.Nil\n  else:List.Cons {head:n,tail:make(n-1)}\nfn main()->int ! TaskError:\n  p=try parallel:\n    try await spawn make(16)\n  sum(p)\n" 136)
WORKER(updated "${PREFIX}${SUM}${IDENTITY}fn update(items:List)->List:\n  match items:\n    List.Nil:List.Nil\n    List.Cons {tail}:List.Cons {head:40,tail:tail}\nfn main()->int ! TaskError:\n  original=List.Cons {head:1,tail:List.Cons {head:2,tail:List.Nil}}\n  changed=try parallel:\n    try await spawn update(original)\n  sum(changed)+sum(original)\n" 45)
WORKER(tree [=[type Tree:
  name:String
  children:[Tree]
fn echo(tree:Tree)->Tree:
  tree
fn make()->Tree:
  leaf=Tree {name:"é",children:[]}
  Tree {name:"root",children:[leaf,leaf]}
fn main()->int ! TaskError:
  tree=try parallel:
    job=spawn make()
    root=try await job
    try await spawn echo(root)
  length(tree.children)+length(tree.children[0].name)+length(tree.name)
]=] 7)
WORKER(collections [=[enum Chain:
  End
  More:
    text:String
    names:[String]
    flags:[Bool]
    values:[int]
    matrix:Tensor[float64,?,2]
    next:Chain
fn make()->Chain:
  tail=Chain.More {text:"é",names:["a","é"],flags:[true,false],values:[20,20],matrix:tensor([[1.0,2.0]]),next:Chain.End}
  tail
fn echo(chain:Chain)->Chain:
  chain
fn main()->int ! TaskError:
  chain=try parallel:
    value=try await spawn make()
    try await spawn echo(value)
  match chain:
    Chain.End:0
    Chain.More {text,names,flags,values,matrix}:
      if flags[0]:values[0]+values[1]+length(text)+length(names[1])+int(matrix[0,0])-1
      else:0
]=] 42)
WORKER(callable [=[enum Chain:
  End
  More:
    apply:fn(int)->int
    callbacks:[fn(int)->int]
    next:Chain
fn make(text:String)->Chain:
  add=fn(value:int):value+length(text)
  Chain.More {apply:add,callbacks:[add,add],next:Chain.End}
fn echo(chain:Chain)->Chain:
  chain
fn main()->int ! TaskError:
  chain=try parallel:
    made=try await spawn make("é")
    try await spawn echo(made)
  match chain:
    Chain.End:0
    Chain.More {apply,callbacks}:apply(19)+callbacks[0](10)+callbacks[1](10)
]=] 42)
WORKER(captured "${PREFIX}${SUM}fn main()->int ! TaskError:\n  items=List.Cons {head:42,tail:List.Nil}\n  callback=fn(n:int):sum(items)+n\n  try parallel:\n    try await spawn callback(0)\n" 42)
WORKER(map "${PREFIX}${SUM}${IDENTITY}fn main()->int ! TaskError:\n  values=[List.Nil,List.Cons {head:20,tail:List.Nil}]\n  result=try parallel:\n    try await spawn map(values,fn(items):List.Cons {head:11,tail:items})\n  sum(result[0])+sum(result[1])\n" 42)
WORKER(recovery "${PREFIX}${SUM}fn bad(items:List)->List:\n  divisor=sum(items)-sum(items)\n  List.Cons {head:1/divisor,tail:items}\nfn attempt(items:List)->List ! TaskError:\n  try parallel:\n    try await spawn bad(items)\nfn main()->int:\n  value=attempt(List.Nil) catch fn(error):\n    match error:\n      TaskError.Fault {kind}:\n        match kind:\n          FaultKind.Arithmetic:List.Cons {head:42,tail:List.Nil}\n          _:List.Nil\n      _:List.Nil\n  sum(value)\n" 42)
WORKER(application "${PREFIX}error Problem:\n  Bad:\n    items:List\nfn reject(items:List)->int ! Problem:\n  fail Problem.Bad {items:items}\nfn child(items:List)->int ! (Problem | TaskError):\n  try parallel:\n    try await spawn reject(items)\nfn main()->int:\n  child(List.Cons {head:42,tail:List.Nil}) catch fn(error):\n    match error:\n      Problem.Bad {items}:\n        match items:\n          List.Nil:0\n          List.Cons {head}:head\n      _:99\n" 42)
WORKER(mixed-errors "${PREFIX}error A:\n  Bad:\n    items:List\nerror B:\n  Bad:\n    text:String\nfn reject(items:List,flag:Bool)->int ! (A | B):\n  if flag:fail A.Bad {items:items}\n  fail B.Bad {text:\"é\"}\nfn child(items:List,flag:Bool)->int ! (A | B | TaskError):\n  try parallel:\n    try await spawn reject(items,flag)\nfn recover(flag:Bool)->int:\n  child(List.Cons {head:41,tail:List.Nil},flag) catch fn(error):\n    match error:\n      A.Bad {items}:\n        match items:\n          List.Nil:0\n          List.Cons {head}:head\n      B.Bad {text}:length(text)\n      _:99\nfn main()->int:\n  recover(true)+recover(false)\n" 42)
WORKER(rethrow "${PREFIX}${SUM}error E:\n  Bad:\n    items:List\nfn reject()->int ! E:\n  fail E.Bad {items:List.Cons {head:42,tail:List.Nil}}\nfn child()->int ! (E | TaskError):\n  try parallel:\n    try await spawn reject()\nfn relay()->int ! (E | TaskError):\n  child() catch fn(error):fail error\nfn main()->int:\n  relay() catch fn(error):\n    match error:\n      E.Bad {items}:sum(items)\n      _:99\n" 42)
WORKER(coordinate "${PREFIX}${SUM}${IDENTITY}fn walk(items:List)->List ! TaskError:\n  match items:\n    List.Nil:List.Nil\n    List.Cons {head,tail}:\n      p=try parallel:\n        try await spawn identity(tail)\n      next=try walk(p)\n      List.Cons {head:head+1,tail:next}\nfn main()->int ! TaskError:\n  value=try walk(List.Cons {head:20,tail:List.Cons {head:20,tail:List.Nil}})\n  sum(value)\n" 42)
WORKER(indirect-coordinate "${PREFIX}${SUM}${IDENTITY}fn walk(items:List)->List ! TaskError:\n  match items:\n    List.Nil:List.Nil\n    List.Cons {head,tail}:\n      p=try parallel:\n        try await spawn identity(tail)\n      recurse=walk\n      next=try recurse(p)\n      List.Cons {head:head+1,tail:next}\nfn main()->int ! TaskError:\n  recurse=walk\n  value=try recurse(List.Cons {head:20,tail:List.Cons {head:20,tail:List.Nil}})\n  sum(value)\n" 42)
WORKER(spawn-recursion "${PREFIX}${SUM}fn walk(items:List)->List ! TaskError:\n  match items:\n    List.Nil:List.Nil\n    List.Cons {head,tail}:\n      next=try parallel:\n        try await spawn walk(tail)\n      List.Cons {head:head+1,tail:next}\nfn main()->int ! TaskError:\n  value=try parallel:\n    try await spawn walk(List.Cons {head:20,tail:List.Cons {head:20,tail:List.Nil}})\n  sum(value)\n" 42)
WORKER(coordinate-infallible "${PREFIX}${SUM}${IDENTITY}fn walk(items:List)->List:\n  match items:\n    List.Nil:List.Nil\n    List.Cons {head,tail}:\n      result=parallel:\n        try await spawn identity(tail)\n      p=result catch fn(error):List.Nil\n      next=walk(p)\n      List.Cons {head:head+1,tail:next}\nfn main()->int ! TaskError:\n  value=try parallel:\n    try await spawn walk(List.Cons {head:20,tail:List.Cons {head:20,tail:List.Nil}})\n  sum(value)\n" 42)
WORKER(coordinate-field [=[enum Chain:
  End
  More:
    apply:fn(int)->int ! TaskError
    callbacks:[fn(int)->int ! TaskError]
    next:Chain
fn leaf(n:int)->int:
  n
fn walk(n:int)->int ! TaskError:
  if n==0:
    try parallel:
      try await spawn leaf(42)
  else:
    recurse=walk
    try recurse(n-1)
fn make()->Chain:
  f=walk
  Chain.More {apply:f,callbacks:[f,f],next:Chain.End}
fn main()->int ! TaskError:
  chain=try parallel:
    try await spawn make()
  match chain:
    Chain.End:0
    Chain.More {apply,callbacks}:
      a=try apply(3)
      b=try callbacks[0](3)
      c=try callbacks[1](3)
      a+b-c
]=] 42)
WORKER(mutual-coordinate "${PREFIX}${SUM}${IDENTITY}fn first(items:List)->List ! TaskError:\n  match items:\n    List.Nil:List.Nil\n    List.Cons {head,tail}:\n      p=try parallel:\n        try await spawn identity(tail)\n      next=try second(p)\n      List.Cons {head:head+1,tail:next}\nfn second(items:List)->List ! TaskError:\n  try first(items)\nfn main()->int ! TaskError:\n  p=try first(List.Cons {head:20,tail:List.Cons {head:20,tail:List.Nil}})\n  sum(p)\n" 42)
WORKER(narrow "${PREFIX}${SUM}error A:\n  Bad:\n    items:List\nerror B:\n  Other:\n    message:String\nfn reject()->int ! (A | B):\n  fail A.Bad {items:List.Cons {head:42,tail:List.Nil}}\nfn child()->int ! (A | B | TaskError):\n  try parallel:\n    try await spawn reject()\nfn narrow()->int ! A:\n  child() catch fn(error):\n    match error:\n      A.Bad:fail error\n      _:99\nfn main()->int:\n  narrow() catch fn(error):\n    match error:\n      A.Bad {items}:sum(items)\n" 42)
WORKER(coordinate-errors "${PREFIX}${SUM}${IDENTITY}error E:\n  Bad:\n    items:List\nfn descend(n:int)->int ! (E | TaskError):\n  if n==0:fail E.Bad {items:List.Cons {head:42,tail:List.Nil}}\n  empty=try parallel:\n    try await spawn identity(List.Nil)\n  try descend(n-1)\nfn main()->int:\n  descend(3) catch fn(error):\n    match error:\n      E.Bad {items}:sum(items)\n      _:99\n" 42)
WORKER(coordinate-limit "${PREFIX}${IDENTITY}fn descend(n:int)->int ! TaskError:\n  empty=try parallel:\n    try await spawn identity(List.Nil)\n  try descend(n+1)\nfn main()->int:\n  descend(0) catch fn(error):\n    match error:\n      TaskError.AdmissionRejected:42\n      TaskError.Fault {kind}:\n        match kind:\n          FaultKind.Allocation:42\n          _:99\n      _:99\n" 42)
# Full source trees settle after root cancellation, cancellation after a child
# was admitted, deadline expiry, byte admission rejection and allocation faults.
foreach(NAME IN ITEMS nested coordinate)
  foreach(MODE IN ITEMS 1 2 3 4)
    RUN_OK("${OUTPUT_DIR}/${NAME}-pools" 42 "${MODE}")
  endforeach()
endforeach()
RUN_OK("${OUTPUT_DIR}/nested-pools" 42 7)
foreach(ATTEMPT RANGE 1 24)
  RUN_OK("${OUTPUT_DIR}/nested-pools" 42 5 "${ATTEMPT}")
  RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${ATTEMPT}"
    "${OUTPUT_DIR}/nested-pools" 42 6)
endforeach()
WORKER(mutual-storage [=[type Node:
  value:int
  next:Link
enum Link:
  End
  More:
    node:Node
fn update(node:Node)->Node:
  node with {value:node.value+1}
fn main()->int ! TaskError:
  leaf=Node {value:40,next:Link.End}
  outer=Node {value:1,next:Link.More {node:leaf}}
  result=try parallel:
    try await spawn update(outer)
  match result.next:
    Link.End:0
    Link.More {node}:result.value+node.value
]=] 42)
WORKER(recursive-error [=[error Problem:
  End
  More:
    text:String
    next:Problem
fn reject()->int ! Problem:
  fail Problem.More {text:"é",next:Problem.More {text:"!",next:Problem.End}}
fn child()->int ! (Problem | TaskError):
  try parallel:
    try await spawn reject()
fn main()->int:
  child() catch fn(error):
    match error:
      Problem.More {text,next}:
        match next:
          Problem.More {text:tail}:length(text)+length(tail)+40
          _:0
      _:99
]=] 42)
file(WRITE "${OUTPUT_DIR}/model.k" "export enum Shared:\n  End\n  More:\n    value:int\n    next:Shared\nexport fn echo(value:Shared)->Shared:\n  value\n")
WORKER(modules [=[from "./model.k" import Shared,echo
fn child(value:Shared)->Shared ! TaskError:
  try parallel:
    try await spawn echo(value)
fn main()->int ! TaskError:
  value=try parallel:
    try await spawn child(Shared.More {value:42,next:Shared.End})
  match value:
    Shared.End:0
    Shared.More {value}:value
]=] 42)
WORKER(recursive-union [=[type Box:
  cause:Errors
type Errors=A | B
error A:
  Bad:
    box:Box
error B:
  End
fn end()->Box ! Errors:
  fail B.End
fn leaf()->Box:
  end() catch fn(error):Box {cause:error}
fn wrapped(box:Box)->Box ! Errors:
  fail A.Bad {box:box}
fn wrap(box:Box)->Box:
  wrapped(box) catch fn(error):Box {cause:error}
fn reject()->int ! Errors:
  fail A.Bad {box:wrap(leaf())}
fn child()->int ! (Errors | TaskError):
  try parallel:
    try await spawn reject()
fn main()->int:
  child() catch fn(error):
    match error:
      A.Bad {box}:
        match box.cause:
          A.Bad {box:tail}:
            match tail.cause:
              B.End:42
              _:0
          _:0
      _:99
]=] 42)
WORKER(recursive-union-arrays [=[type Box:
  causes:[Errors]
type Errors=A | B
error A:
  Bad:
    boxes:[Box]
error B:
  End
fn end()->Box ! Errors:
  fail B.End
fn leaf()->Box:
  end() catch fn(error):Box {causes:[error]}
fn parent(box:Box)->Box ! Errors:
  fail A.Bad {boxes:[box,box]}
fn make()->Box:
  parent(leaf()) catch fn(error):Box {causes:[error]}
fn main()->int ! TaskError:
  value=try parallel:
    try await spawn make()
  match value.causes[0]:
    A.Bad {boxes}:length(boxes)+40
    _:0
]=] 42)
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  WORKER("width-${TYPE}" "enum Cell:\n  End\n  More:\n    value:${TYPE}\n    values:[${TYPE}]\n    next:Cell\nfn make()->Cell:\n  Cell.More {value:${TYPE}(40),values:[${TYPE}(1),${TYPE}(1)],next:Cell.End}\nfn echo(value:Cell)->Cell:\n  value\nfn main()->int ! TaskError:\n  value=try parallel:\n    made=try await spawn make()\n    try await spawn echo(made)\n  match value:\n    Cell.End:0\n    Cell.More {value,values}:int(value)+int(values[0])+int(values[1])\n" 42)
endforeach()
COMPILE(empty-output "${PREFIX}${IDENTITY}fn values(items:List)->[int8]:\n  []\nfn main()->[int8] ! TaskError:\n  try parallel:\n    try await spawn values(List.Nil)\n")
EXPECT(empty-output "[]")
# An infallible recursive coordinator cannot invent a typed TaskError channel.
# Its hidden child admission failure must remain an execution failure, with no
# output publication and no stranded lexical groups.
COMPILE(infallible-limit "${PREFIX}fn leaf()->int:\n  0\nfn descend()->int:\n  result=parallel:\n    try await spawn leaf()\n  ignored=result catch fn(error):0\n  descend()\nfn main()->int:\n  descend()\n" --test-entry main)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveContinuationRuntimeTests.cpp" "${OUTPUT_DIR}/infallible-limit.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/infallible-limit-pools")
RUN_OK("${OUTPUT_DIR}/infallible-limit-pools" 0 8)
file(READ "${SOURCE_DIR}/../docs/design/fixtures/recursive-workers.k" GUIDE)
WORKER(guide "${GUIDE}" 42)
WORKER(task-error-field [=[enum Chain:
  End
  More:
    cause:TaskError
    next:Chain
fn divide(value:int)->Chain:
  ignored=1/value
  Chain.End
fn capture()->Chain:
  result=parallel:
    try await spawn divide(0)
  result catch fn(error):
    leaf=Chain.More {cause:error,next:Chain.End}
    Chain.More {cause:error,next:leaf}
fn echo(value:Chain)->Chain:
  value
fn score(value:Chain)->int:
  match value:
    Chain.End:0
    Chain.More {next}:
      match next:
        Chain.End:0
        Chain.More {cause}:
          match cause:
            TaskError.Fault {kind}:
              match kind:
                FaultKind.Arithmetic:42
                _:0
            _:0
fn main()->int ! TaskError:
  value=try parallel:
    try await spawn echo(capture())
  callback=fn(offset:int):score(value)+offset
  try parallel:
    try await spawn callback(0)
]=] 42)
message(STATUS "Recursive CPU source workers: values, errors, coordinating calls and bounded cleanup passed")
