include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX "enum List:\n  Nil\n  Cons:\n    head:int\n    tail:List\n")
set(SUM "fn sum(items:List)->int:\n  match items:\n    List.Nil:0\n    List.Cons {head,tail}:head+sum(tail)\n")
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  COMPILE("list-${TYPE}" "enum List:\n  Nil\n  Cons:\n    head:${TYPE}\n    tail:List\nfn sum(items:List)->${TYPE}:\n  match items:\n    List.Nil:0\n    List.Cons {head,tail}:head+sum(tail)\nfn main()->int:\n  items=List.Cons {head:1,tail:List.Cons {head:2,tail:List.Nil}}\n  int(sum(items))\n")
  EXPECT("list-${TYPE}" 3)
endforeach()
COMPILE(bool-list "enum List:\n  Nil\n  Cons:\n    head:Bool\n    tail:List\nfn any(items:List)->Bool:\n  match items:\n    List.Nil:false\n    List.Cons {head,tail}:head or any(tail)\nfn main()->Bool:\n  any(List.Cons {head:false,tail:List.Cons {head:true,tail:List.Nil}})\n")
EXPECT(bool-list true)
COMPILE(returned "${PREFIX}${SUM}fn make(n:int)->List:\n  if n==0:List.Nil\n  else:List.Cons {head:n,tail:make(n-1)}\nfn main()->int:\n  items=make(32)\n  sum(items)\n")
EXPECT(returned 528)
COMPILE(mutual [=[type Node:
  value:int
  next:Link
enum Link:
  End
  More:
    node:Node
fn main()->int:
  original=Node {value:7,next:Link.End}
  outer=Node {value:1,next:Link.More {node:original}}
  changed=outer with {value:20,next:Link.More {node:original with {value:99}}}
  a=match outer.next:
    Link.End:0
    Link.More {node}:node.value
  b=match changed.next:
    Link.End:0
    Link.More {node}:node.value
  a+b+outer.value+changed.value
]=])
EXPECT(mutual 127)
COMPILE(shared "${PREFIX}${SUM}fn main()->int:\n  tail=List.Cons {head:3,tail:List.Nil}\n  left=List.Cons {head:10,tail:tail}\n  right=List.Cons {head:20,tail:tail}\n  sum(left)+sum(right)+sum(tail)\n")
EXPECT(shared 39)
COMPILE(fallback "${PREFIX}fn bad(x:int)->int:\n  1/x\nfn main()->int:\n  item=List.Cons {head:42,tail:List.Nil}\n  match item:\n    List.Nil:bad(0)\n    _:42\n")
EXPECT(fallback 42)
COMPILE(branch-return "${PREFIX}${SUM}fn select(n:int)->List:\n  if n==0:List.Nil\n  else:List.Cons {head:42,tail:List.Nil}\nfn main()->int:\n  sum(select(1))+sum(select(0))\n")
EXPECT(branch-return 42)
COMPILE(uint64-bits "enum List:\n  Nil\n  Cons:\n    head:uint64\n    tail:List\nfn main()->uint64:\n  match List.Cons {head:18446744073709551615,tail:List.Nil}:\n    List.Nil:0\n    List.Cons {head}:head\n")
EXPECT(uint64-bits 18446744073709551615)
COMPILE(float64-bits "enum List:\n  Nil\n  Cons:\n    head:float64\n    tail:List\nfn head(items:List)->float64:\n  match items:\n    List.Nil:0\n    List.Cons {head}:head\nfn main()->Bool:\n  head(List.Cons {head:1.0000000000000002,tail:List.Nil})==1.0000000000000002\n")
EXPECT(float64-bits true)
file(WRITE "${OUTPUT_DIR}/model.k" "export enum List:\n  Nil\n  Cons:\n    head:int\n    tail:List\n")
COMPILE(imported "from \"./model.k\" import List as L\nfn main()->int:\n  match L.Cons {head:42,tail:L.Nil}:\n    L.Nil:0\n    L.Cons {head}:head\n")
EXPECT(imported 42)
COMPILE(match-return "${PREFIX}${SUM}fn tail(items:List)->List:
  match items:
    List.Nil:List.Nil
    List.Cons {tail}:tail
fn main()->int:
  sum(tail(List.Cons {head:10,tail:List.Cons {head:42,tail:List.Nil}}))
")
EXPECT(match-return 42)
COMPILE(match-array "${PREFIX}fn values(items:List)->[int]:
  match items:
    List.Nil:[]
    List.Cons {head}:[head,head+1]
fn main()->[int]:
  values(List.Cons {head:42,tail:List.Nil})
")
EXPECT(match-array "[42, 43]")
COMPILE(single-case [=[enum Item:
  Value:
    next:Maybe
enum Maybe:
  None
  Some:
    item:Item
fn score(item:Item)->int:
  match item:
    Item.Value {next}:
      match next:
        Maybe.None:42
        Maybe.Some {item}:score(item)
fn main()->int:
  score(Item.Value {next:Maybe.None})
]=])
EXPECT(single-case 42)
COMPILE(binary-tree [=[enum Tree:
  Empty
  Leaf:
    value:int
  Branch:
    left:Tree
    right:Tree
fn sum(tree:Tree)->int:
  match tree:
    Tree.Empty:0
    Tree.Leaf {value}:value
    Tree.Branch {left,right}:sum(left)+sum(right)
fn main()->int:
  shared=Tree.Branch {left:Tree.Leaf {value:1},right:Tree.Leaf {value:2}}
  root=Tree.Branch {left:shared,right:shared}
  sum(root)
]=])
EXPECT(binary-tree 6)
COMPILE(fault "${PREFIX}fn bad(x:int)->int:\n  1/x\nfn main()->int:\n  item=List.Cons {head:1,tail:List.Nil}\n  match item:\n    List.Nil:0\n    List.Cons:bad(0)\n")
COMPILE(call-limit "${PREFIX}fn make(n:int)->List:\n  if n==0:List.Nil\n  else:List.Cons {head:n,tail:make(n-1)}\nfn main()->int:\n  item=make(1000000)\n  0\n")
foreach(NAME IN ITEMS fault call-limit)
  if(NAME STREQUAL "fault")
    set(EXPECTED_FAULT ArithmeticFault)
  else()
    set(EXPECTED_FAULT "(StackExhaustionFault|AllocationFault)")
  endif()
  set(RESOURCE_ENV "")
  if(NAME STREQUAL "call-limit")
    # This constructor may use stackless continuations. Inject a backing
    # allocation failure after admission beyond the retired 128-call cap.
    set(RESOURCE_ENV KALANDRA_TEST_FAIL_ALLOC_AT=512)
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${RESOURCE_ENV} KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/${NAME}"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR NOT ERR MATCHES "cleanup: live=0" OR NOT ERR MATCHES "${EXPECTED_FAULT}")
    message(FATAL_ERROR "Recursive fault cleanup ${NAME}: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
foreach(FAIL_AT RANGE 1 35)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/returned"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "528\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive allocation success changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
