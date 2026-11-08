file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(FLAGS -O0)
if(VARIANT STREQUAL "optimized")
  set(FLAGS -O2)
elseif(VARIANT STREQUAL "sanitized")
  set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(VARIANT STREQUAL "thread-sanitized")
  set(FLAGS -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
endif()
function(RUN_OK)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "0" OR NOT ERR STREQUAL "")
    message(FATAL_ERROR "Recursive continuation: ${STATUS}: ${OUT}${ERR}")
  endif()
  set(OUTPUT "${OUT}" PARENT_SCOPE)
endfunction()
function(COMPILE NAME SOURCE)
  file(WRITE "${OUTPUT_DIR}/${NAME}.k" "${SOURCE}")
  RUN_OK("${EMITTER}" "${OUTPUT_DIR}/${NAME}.k" "${OUTPUT_DIR}/${NAME}.ll")
  file(READ "${OUTPUT_DIR}/${NAME}.ll" IR)
  if(NOT IR MATCHES "kalandra_run_task_v1" OR NOT IR MATCHES "task.entry")
    message(FATAL_ERROR "Fixture bypassed the continuation task backend: ${NAME}")
  endif()
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    "${SOURCE_DIR}/runtime/Int64Entry.cpp" "${OUTPUT_DIR}/${NAME}.ll" ${TASK_ABI_OBJECTS} "${RUNTIME}"
    -o "${OUTPUT_DIR}/${NAME}")
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    "${SOURCE_DIR}/tests/RecursiveContinuationRuntimeTests.cpp" "${OUTPUT_DIR}/${NAME}.ll"
    ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/${NAME}-pools")
endfunction()
function(EXPECT NAME EXPECTED)
  RUN_OK("${OUTPUT_DIR}/${NAME}-pools" "${EXPECTED}")
  RUN_OK("${OUTPUT_DIR}/${NAME}")
  if(NOT OUTPUT STREQUAL "${EXPECTED}\n")
    message(FATAL_ERROR "${NAME}: expected ${EXPECTED}, got ${OUTPUT}")
  endif()
endfunction()
set(PREFIX "enum List:\n  Nil\n  Cons:\n    head:i64\n    tail:List\n")
set(SUM "fn sum(items:List)->i64:\n  return match items:\n    List.Nil:0\n    List.Cons {head,tail}:head+sum(tail)\n")
COMPILE(shared "${PREFIX}${SUM}fn main()->i64:\n  tail=List.Cons {head:3,tail:List.Nil}\n  left=List.Cons {head:10,tail:tail}\n  right=List.Cons {head:20,tail:tail}\n  return sum(left)+sum(right)+sum(tail)\n")
EXPECT(shared 39)
COMPILE(returned "${PREFIX}${SUM}fn make(n:i64)->List:\n  return if n==0:List.Nil\n  else:List.Cons {head:n,tail:make(n-1)}\nfn main()->i64:\n  return sum(make(32))\n")
EXPECT(returned 528)
COMPILE(match-return "${PREFIX}${SUM}fn tail(items:List)->List:\n  return match items:\n    List.Nil:List.Nil\n    List.Cons {tail}:tail\nfn main()->i64:\n  return sum(tail(List.Cons {head:10,tail:List.Cons {head:42,tail:List.Nil}}))\n")
EXPECT(match-return 42)
COMPILE(owned [=[enum Chain:
  End
  More:
    text:String
    values:[i64]
    next:Chain
fn inspect(chain:Chain)->i64:
  return match chain:
    Chain.End:0
    Chain.More {text,values,next}:length(text)+values[0]+inspect(next)
fn main()->i64:
  tail=Chain.More {text:"é",values:[20],next:Chain.End}
  root=Chain.More {text:"a",values:[20],next:tail}
  return inspect(root)
]=])
EXPECT(owned 42)
COMPILE(tree [=[type Tree:
  name:String
  children:[Tree]
fn main()->i64:
  leaf=Tree {name:"é",children:[]}
  root=Tree {name:"root",children:[leaf,leaf]}
  return length(root.children)+length(root.children[0].name)+length(root.name)
]=])
EXPECT(tree 7)
COMPILE(callable [=[enum Chain:
  End
  More:
    apply:fn(i64)->i64
    next:Chain
fn make(text:String)->Chain:
  return Chain.More {apply:fn(value:i64):return value+length(text),next:Chain.End}
fn main()->i64:
  chain=make("é")
  return match chain:
    Chain.End:0
    Chain.More {apply}:apply(41)
]=])
EXPECT(callable 42)
COMPILE(function-array [=[enum Chain:
  End
  More:
    callbacks:[fn(i64)->i64]
    next:Chain
fn make(offset:i64)->Chain:
  add=fn(value:i64):return value+offset
  return Chain.More {callbacks:[add,add],next:Chain.End}
fn main()->i64:
  return match make(20):
    Chain.End:0
    Chain.More {callbacks}:callbacks[0](1)+callbacks[1](1)
]=])
EXPECT(function-array 42)
COMPILE(fault "${PREFIX}fn bad(x:i64)->i64:\n  return 1/x\nfn main()->i64:\n  item=List.Cons {head:1,tail:List.Nil}\n  return match item:\n    List.Nil:0\n    List.Cons:bad(0)\n")
COMPILE(call-limit "${PREFIX}fn loop(n:i64)->i64:\n  return loop(n+1)+n\nfn main()->i64:\n  return loop(0)\n")
foreach(NAME IN ITEMS fault call-limit)
  if(NAME STREQUAL "fault")
    set(EXPECTED_FAULT ArithmeticFault)
  else()
    # Continuation frames are heap-owned; their allocation budget may stop
    # growth before a native stack reserve is reached. Both paths must settle.
    set(EXPECTED_FAULT "(StackExhaustionFault|AllocationFault)")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1
    "${OUTPUT_DIR}/${NAME}" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "${EXPECTED_FAULT}" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive continuation fault cleanup ${NAME}: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
foreach(FAIL_AT RANGE 1 35)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/owned" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "42\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive continuation allocation success changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive continuation allocation cleanup: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
