include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX [=[enum Chain:
  End
  Link:
    apply:fn(int)->int
    next:Chain
fn run(chain:Chain,value:int)->int:
  match chain:
    Chain.End:0
    Chain.Link {apply,next}:apply(value)+run(next,value)
]=])
COMPILE(named "${PREFIX}fn double(x:int)->int:\n  x*2\nfn main()->int:\n  inner=Chain.Link {apply:double,next:Chain.End}\n  root=Chain.Link {apply:double,next:inner}\n  run(root,7)\n")
EXPECT(named 28)
COMPILE(returned "${PREFIX}fn make(offset:int)->Chain:\n  Chain.Link {apply:fn(value):value+offset,next:Chain.End}\nfn main()->int:\n  inner=make(20)\n  root=Chain.Link {apply:fn(value):value,next:inner}\n  run(root,11)\n")
EXPECT(returned 42)
COMPILE(owned "${PREFIX}fn make(text:String,values:[int],names:[String],flags:[Bool])->Chain:\n  Chain.Link {apply:fn(value):\n    if flags[0]:value+length(text)+values[0]+length(names[0])\n    else:0\n  ,next:Chain.End}\nfn main()->int:\n  inner=make(\"é\",[20],[\"👩\"],[true])\n  root=Chain.Link {apply:fn(value):value,next:inner}\n  run(root,10)\n")
EXPECT(owned 42)
COMPILE(nested "${PREFIX}fn adder(offset:int)->fn(int)->int:\n  fn(value):value+offset\nfn make(offset:int)->Chain:\n  add=adder(offset)\n  Chain.Link {apply:fn(value):add(value)+1,next:Chain.End}\nfn main()->int:\n  inner=make(20)\n  root=Chain.Link {apply:fn(value):value,next:inner}\n  run(root,10)\n")
EXPECT(nested 41)
COMPILE(record-capture "${PREFIX}enum Values:\n  Nil\n  Cons:\n    value:int\n    next:Values\nfn sum(values:Values)->int:\n  match values:\n    Values.Nil:0\n    Values.Cons {value,next}:value+sum(next)\nfn make(values:Values)->Chain:\n  Chain.Link {apply:fn(value):value+sum(values),next:Chain.End}\nfn main()->int:\n  inner=make(Values.Cons {value:20,next:Values.Cons {value:2,next:Values.Nil}})\n  root=Chain.Link {apply:fn(value):value,next:inner}\n  run(root,10)\n")
EXPECT(record-capture 42)
COMPILE(update [=[type Node:
  apply:fn(int)->int
  next:Link
enum Link:
  End
  More:
    node:Node
fn make(offset:int)->Node:
  Node {apply:fn(value):value+offset,next:Link.End}
fn main()->int:
  original=make(20)
  changed=original with {apply:fn(value):value*2}
  root=Node {apply:fn(value):value,next:Link.More {node:changed}}
  match root.next:
    Link.End:0
    Link.More {node}:original.apply(10)+node.apply(6)
]=])
EXPECT(update 42)
COMPILE(record-array "${PREFIX}type Box:\n  apply:fn(int)->int\n  name:String\nenum Boxes:\n  End\n  More:\n    values:[Box]\n    next:Boxes\nfn make(offset:int)->Boxes:\n  Boxes.More {values:[Box {apply:fn(value):value+offset,name:\"é\"}],next:Boxes.End}\nfn main()->int:\n  inner=make(20)\n  root=Boxes.More {values:[],next:inner}\n  match root:\n    Boxes.End:0\n    Boxes.More {next}:\n      match next:\n        Boxes.End:0\n        Boxes.More {values}:values[0].apply(21)+length(values[0].name)\n")
EXPECT(record-array 42)
COMPILE(nominal [=[type Node:
  value:int
  apply:fn(Node)->int
  next:Link
enum Link:
  End
  More:
    node:Node
fn read(node:Node)->int:
  node.value
fn main()->int:
  inner=Node {value:42,apply:read,next:Link.End}
  root=Node {value:0,apply:read,next:Link.More {node:inner}}
  match root.next:
    Link.End:0
    Link.More {node}:node.apply(node)
]=])
EXPECT(nominal 42)
COMPILE(tensor-capture "${PREFIX}fn make(weights:Tensor[int,2,2])->Chain:\n  Chain.Link {apply:fn(value):value+reduceSum(weights),next:Chain.End}\nfn main()->int:\n  inner=make(tensor([[5,6],[5,6]]))\n  root=Chain.Link {apply:fn(value):value,next:inner}\n  run(root,10)\n")
EXPECT(tensor-capture 42)
COMPILE(record-result [=[enum Values:
  Nil
  Cons:
    value:int
    next:Values
enum Chain:
  End
  More:
    get:fn()->Values
    next:Chain
fn getter(values:Values)->fn()->Values:
  fn():values
fn sum(values:Values)->int:
  match values:
    Values.Nil:0
    Values.Cons {value,next}:value+sum(next)
fn main()->int:
  values=Values.Cons {value:20,next:Values.Cons {value:22,next:Values.Nil}}
  inner=Chain.More {get:getter(values),next:Chain.End}
  outer=Chain.More {get:getter(Values.Nil),next:inner}
  match outer:
    Chain.End:0
    Chain.More {next}:
      match next:
        Chain.End:0
        Chain.More {get}:sum(get())
]=])
EXPECT(record-result 42)
COMPILE(fault "${PREFIX}fn make(zero:int)->Chain:\n  Chain.Link {apply:fn(value):value/zero,next:Chain.End}\nfn main()->int:\n  root=Chain.Link {apply:fn(value):value,next:make(0)}\n  run(root,1)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/fault"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
    NOT ERR MATCHES "ArithmeticFault" OR NOT ERR MATCHES "cleanup: live=0")
  message(FATAL_ERROR "Recursive callable fault cleanup: ${STATUS}: ${OUT}${ERR}")
endif()
foreach(FAIL_AT RANGE 1 35)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/owned"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "42\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive callable result changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive callable allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
