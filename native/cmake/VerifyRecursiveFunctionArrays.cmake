include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX [=[enum Chain:
  End
  More:
    callbacks:[fn(int)->int]
    next:Chain
fn sum(values:[int],index:int)->int:
  if index<length(values):values[index]+sum(values,index+1)
  else:0
fn total(chain:Chain)->int:
  match chain:
    Chain.End:0
    Chain.More {callbacks,next}:
      values=map(callbacks,fn(callback):callback(1))
      sum(values,0)+total(next)
]=])
COMPILE(named "${PREFIX}fn double(value:int)->int:\n  value*2\nfn main()->int:\n  total(Chain.More {callbacks:[double,double],next:Chain.End})+38\n")
EXPECT(named 42)
COMPILE(shared "${PREFIX}fn make(offset:int)->Chain:\n  add=fn(value:int):value+offset\n  Chain.More {callbacks:[add,add],next:Chain.End}\nfn main()->int:\n  total(Chain.More {callbacks:[],next:make(20)})\n")
EXPECT(shared 42)
COMPILE(owned "${PREFIX}fn make(text:String,values:[int])->Chain:\n  add=fn(value:int):value+length(text)+values[0]\n  Chain.More {callbacks:[add,add],next:Chain.End}\nfn main()->int:\n  total(make(\"é\",[19]))\n")
EXPECT(owned 42)
COMPILE(returned "${PREFIX}fn make(offset:int)->[fn(int)->int]:\n  [fn(value):value+offset]\nfn main()->int:\n  callbacks=concat(make(10),make(20))\n  selected=slice(callbacks,1,2)\n  total(Chain.More {callbacks:selected,next:Chain.End})+21\n")
EXPECT(returned 42)
COMPILE(captured "${PREFIX}fn wrap(callbacks:[fn(int)->int])->fn(int)->int:\n  fn(value):callbacks[0](value)\nfn main()->int:\n  offset=20\n  callbacks:[fn(int)->int]=[fn(value):value+offset]\n  total(Chain.More {callbacks:[wrap(callbacks),wrap(callbacks)],next:Chain.End})\n")
EXPECT(captured 42)
COMPILE(mapped "${PREFIX}fn main()->int:\n  callbacks=map([20,20],fn(offset):fn(value:int):value+offset)\n  total(Chain.More {callbacks:callbacks,next:Chain.End})\n")
EXPECT(mapped 42)
COMPILE(updated [=[type Node:
  callbacks:[fn(int)->int]
  next:Link
enum Link:
  End
  More:
    node:Node
fn make(offset:int)->Node:
  Node {callbacks:[fn(value):value+offset],next:Link.End}
fn main()->int:
  original=make(10)
  changed=original with {callbacks:concat(original.callbacks,[fn(value:int):value+20])}
  root=Node {callbacks:[],next:Link.More {node:changed}}
  match root.next:
    Link.End:0
    Link.More {node}:original.callbacks[0](1)+node.callbacks[1](11)
]=])
EXPECT(updated 42)
COMPILE(record-array "${PREFIX}type Box:\n  callbacks:[fn(int)->int]\nfn make(offset:int)->[Box]:\n  [Box {callbacks:[fn(value):value+offset]}]\nfn main()->int:\n  boxes=make(20)\n  node=Chain.More {callbacks:boxes[0].callbacks,next:Chain.End}\n  total(node)+21\n")
EXPECT(record-array 42)
COMPILE(factory "${PREFIX}fn make(offset:int)->[fn(int)->int]:\n  [fn(value):value+offset]\nfn apply(factory:fn(int)->[fn(int)->int])->int:\n  total(Chain.More {callbacks:factory(20),next:Chain.End})\nfn main()->int:\n  apply(make)+21\n")
EXPECT(factory 42)
file(READ "${SOURCE_DIR}/../docs/design/fixtures/recursive-function-arrays.k" GUIDE)
COMPILE(guide "${GUIDE}")
EXPECT(guide 42)
COMPILE(fault "${PREFIX}fn make(zero:int)->Chain:\n  Chain.More {callbacks:[fn(value):value/zero],next:Chain.End}\nfn main()->int:\n  total(make(0))\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/fault"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
    NOT ERR MATCHES "ArithmeticFault" OR NOT ERR MATCHES "cleanup: live=0")
  message(FATAL_ERROR "Recursive function array fault cleanup: ${STATUS}: ${OUT}${ERR}")
endif()
foreach(FAIL_AT RANGE 1 45)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/owned"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "42\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive function array result changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive function array allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
