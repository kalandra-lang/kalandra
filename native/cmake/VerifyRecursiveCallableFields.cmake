include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX [=[enum Chain:
  End
  Link:
    apply:fn(i64)->i64
    next:Chain
fn run(chain:Chain,value:i64)->i64:
  return match chain:
    Chain.End:0
    Chain.Link {apply,next}:apply(value)+run(next,value)
]=])
COMPILE(named "${PREFIX}fn double(x:i64)->i64:\n  return x*2\nfn main()->i64:\n  inner=Chain.Link {apply:double,next:Chain.End}\n  root=Chain.Link {apply:double,next:inner}\n  return run(root,7)\n")
EXPECT(named 28)
COMPILE(returned "${PREFIX}fn make(offset:i64)->Chain:\n  return Chain.Link {apply:fn(value):return value+offset,next:Chain.End}\nfn main()->i64:\n  inner=make(20)\n  root=Chain.Link {apply:fn(value):return value,next:inner}\n  return run(root,11)\n")
EXPECT(returned 42)
COMPILE(owned "${PREFIX}fn make(text:String,values:[i64],names:[String],flags:[bool])->Chain:\n  return Chain.Link {apply:fn(value):\n    return if flags[0]:value+length(text)+values[0]+length(names[0])\n    else:0\n  ,next:Chain.End}\nfn main()->i64:\n  inner=make(\"é\",[20],[\"👩\"],[true])\n  root=Chain.Link {apply:fn(value):return value,next:inner}\n  return run(root,10)\n")
EXPECT(owned 42)
COMPILE(nested "${PREFIX}fn adder(offset:i64)->fn(i64)->i64:\n  return fn(value):return value+offset\nfn make(offset:i64)->Chain:\n  add=adder(offset)\n  return Chain.Link {apply:fn(value):return add(value)+1,next:Chain.End}\nfn main()->i64:\n  inner=make(20)\n  root=Chain.Link {apply:fn(value):return value,next:inner}\n  return run(root,10)\n")
EXPECT(nested 41)
COMPILE(record-capture "${PREFIX}enum Values:\n  Nil\n  Cons:\n    value:i64\n    next:Values\nfn sum(values:Values)->i64:\n  return match values:\n    Values.Nil:0\n    Values.Cons {value,next}:value+sum(next)\nfn make(values:Values)->Chain:\n  return Chain.Link {apply:fn(value):return value+sum(values),next:Chain.End}\nfn main()->i64:\n  inner=make(Values.Cons {value:20,next:Values.Cons {value:2,next:Values.Nil}})\n  root=Chain.Link {apply:fn(value):return value,next:inner}\n  return run(root,10)\n")
EXPECT(record-capture 42)
COMPILE(update [=[type Node:
  apply:fn(i64)->i64
  next:Link
enum Link:
  End
  More:
    node:Node
fn make(offset:i64)->Node:
  return Node {apply:fn(value):return value+offset,next:Link.End}
fn main()->i64:
  original=make(20)
  changed=original with {apply:fn(value):return value*2}
  root=Node {apply:fn(value):return value,next:Link.More {node:changed}}
  return match root.next:
    Link.End:0
    Link.More {node}:original.apply(10)+node.apply(6)
]=])
EXPECT(update 42)
COMPILE(record-array "${PREFIX}type Box:\n  apply:fn(i64)->i64\n  name:String\nenum Boxes:\n  End\n  More:\n    values:[Box]\n    next:Boxes\nfn make(offset:i64)->Boxes:\n  return Boxes.More {values:[Box {apply:fn(value):return value+offset,name:\"é\"}],next:Boxes.End}\nfn main()->i64:\n  inner=make(20)\n  root=Boxes.More {values:[],next:inner}\n  return match root:\n    Boxes.End:0\n    Boxes.More {next}:\n      match next:\n        Boxes.End:0\n        Boxes.More {values}:values[0].apply(21)+length(values[0].name)\n")
EXPECT(record-array 42)
COMPILE(nominal [=[type Node:
  value:i64
  apply:fn(Node)->i64
  next:Link
enum Link:
  End
  More:
    node:Node
fn read(node:Node)->i64:
  return node.value
fn main()->i64:
  inner=Node {value:42,apply:read,next:Link.End}
  root=Node {value:0,apply:read,next:Link.More {node:inner}}
  return match root.next:
    Link.End:0
    Link.More {node}:node.apply(node)
]=])
EXPECT(nominal 42)
COMPILE(tensor-capture "${PREFIX}fn make(weights:Tensor[i64,2,2])->Chain:\n  return Chain.Link {apply:fn(value):return value+reduceSum(weights),next:Chain.End}\nfn main()->i64:\n  inner=make(tensor([[5,6],[5,6]]))\n  root=Chain.Link {apply:fn(value):return value,next:inner}\n  return run(root,10)\n")
EXPECT(tensor-capture 42)
COMPILE(record-result [=[enum Values:
  Nil
  Cons:
    value:i64
    next:Values
enum Chain:
  End
  More:
    get:fn()->Values
    next:Chain
fn getter(values:Values)->fn()->Values:
  return fn():return values
fn sum(values:Values)->i64:
  return match values:
    Values.Nil:0
    Values.Cons {value,next}:value+sum(next)
fn main()->i64:
  values=Values.Cons {value:20,next:Values.Cons {value:22,next:Values.Nil}}
  inner=Chain.More {get:getter(values),next:Chain.End}
  outer=Chain.More {get:getter(Values.Nil),next:inner}
  return match outer:
    Chain.End:0
    Chain.More {next}:
      match next:
        Chain.End:0
        Chain.More {get}:sum(get())
]=])
EXPECT(record-result 42)
COMPILE(fault "${PREFIX}fn make(zero:i64)->Chain:\n  return Chain.Link {apply:fn(value):return value/zero,next:Chain.End}\nfn main()->i64:\n  root=Chain.Link {apply:fn(value):return value,next:make(0)}\n  return run(root,1)\n")
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
