include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX [=[enum Chain:
  End
  More:
    callbacks:[fn(i64)->i64]
    next:Chain
fn sum(values:[i64],index:i64)->i64:
  return if index<length(values):values[index]+sum(values,index+1)
  else:0
fn total(chain:Chain)->i64:
  return match chain:
    Chain.End:0
    Chain.More {callbacks,next}:
      values=map(callbacks,fn(callback):return callback(1))
      sum(values,0)+total(next)
]=])
COMPILE(named "${PREFIX}fn double(value:i64)->i64:\n  return value*2\nfn main()->i64:\n  return total(Chain.More {callbacks:[double,double],next:Chain.End})+38\n")
EXPECT(named 42)
COMPILE(shared "${PREFIX}fn make(offset:i64)->Chain:\n  add=fn(value:i64):return value+offset\n  return Chain.More {callbacks:[add,add],next:Chain.End}\nfn main()->i64:\n  return total(Chain.More {callbacks:[],next:make(20)})\n")
EXPECT(shared 42)
COMPILE(owned "${PREFIX}fn make(text:String,values:[i64])->Chain:\n  add=fn(value:i64):return value+length(text)+values[0]\n  return Chain.More {callbacks:[add,add],next:Chain.End}\nfn main()->i64:\n  return total(make(\"é\",[19]))\n")
EXPECT(owned 42)
COMPILE(returned "${PREFIX}fn make(offset:i64)->[fn(i64)->i64]:\n  return [fn(value):return value+offset]\nfn main()->i64:\n  callbacks=concat(make(10),make(20))\n  selected=slice(callbacks,1,2)\n  return total(Chain.More {callbacks:selected,next:Chain.End})+21\n")
EXPECT(returned 42)
COMPILE(captured "${PREFIX}fn wrap(callbacks:[fn(i64)->i64])->fn(i64)->i64:\n  return fn(value):return callbacks[0](value)\nfn main()->i64:\n  offset=20\n  callbacks:[fn(i64)->i64]=[fn(value):return value+offset]\n  return total(Chain.More {callbacks:[wrap(callbacks),wrap(callbacks)],next:Chain.End})\n")
EXPECT(captured 42)
COMPILE(mapped "${PREFIX}fn main()->i64:\n  callbacks=map([20,20],fn(offset):return fn(value:i64):return value+offset)\n  return total(Chain.More {callbacks:callbacks,next:Chain.End})\n")
EXPECT(mapped 42)
COMPILE(updated [=[type Node:
  callbacks:[fn(i64)->i64]
  next:Link
enum Link:
  End
  More:
    node:Node
fn make(offset:i64)->Node:
  return Node {callbacks:[fn(value):return value+offset],next:Link.End}
fn main()->i64:
  original=make(10)
  changed=original with {callbacks:concat(original.callbacks,[fn(value:i64):return value+20])}
  root=Node {callbacks:[],next:Link.More {node:changed}}
  return match root.next:
    Link.End:0
    Link.More {node}:original.callbacks[0](1)+node.callbacks[1](11)
]=])
EXPECT(updated 42)
COMPILE(record-array "${PREFIX}type Box:\n  callbacks:[fn(i64)->i64]\nfn make(offset:i64)->[Box]:\n  return [Box {callbacks:[fn(value):return value+offset]}]\nfn main()->i64:\n  boxes=make(20)\n  node=Chain.More {callbacks:boxes[0].callbacks,next:Chain.End}\n  return total(node)+21\n")
EXPECT(record-array 42)
COMPILE(factory "${PREFIX}fn make(offset:i64)->[fn(i64)->i64]:\n  return [fn(value):return value+offset]\nfn apply(factory:fn(i64)->[fn(i64)->i64])->i64:\n  return total(Chain.More {callbacks:factory(20),next:Chain.End})\nfn main()->i64:\n  return apply(make)+21\n")
EXPECT(factory 42)
file(READ "${SOURCE_DIR}/../docs/design/fixtures/recursive-function-arrays.k" GUIDE)
COMPILE(guide "${GUIDE}")
EXPECT(guide 42)
COMPILE(fault "${PREFIX}fn make(zero:i64)->Chain:\n  return Chain.More {callbacks:[fn(value):return value/zero],next:Chain.End}\nfn main()->i64:\n  return total(make(0))\n")
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
