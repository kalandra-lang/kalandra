include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
file(GLOB SOURCES "${SOURCE_DIR}/tests/fixtures/recursive-callables/*.k")
foreach(PATH IN LISTS SOURCES)
  get_filename_component(NAME "${PATH}" NAME_WE)
  file(READ "${PATH}" TEXT)
  COMPILE("${NAME}" "${TEXT}")
  if(NAME STREQUAL "factorial")
    EXPECT("${NAME}" 120)
  else()
    EXPECT("${NAME}" 42)
  endif()
endforeach()
file(READ "${SOURCE_DIR}/../docs/design/fixtures/recursive-callables.k" GUIDE)
COMPILE(guide "${GUIDE}")
EXPECT(guide 42)
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  COMPILE("width-${TYPE}" "error E:\n  Bad:\n    value:${TYPE}\nfn descend(n:int)->${TYPE} ! E:\n  if n==0:fail E.Bad {value:${TYPE}(42)}\n  try descend(n-1)\nfn main()->${TYPE}:\n  callback=descend\n  callback(20) catch fn(error):\n    match error:\n      E.Bad {value}:value\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
COMPILE(raw [=[error E:
  Bad:
    text:String
    values:[int]
fn descend(n:int)->int ! E:
  if n==0:fail E.Bad {text:"é"+"!",values:[20,20]}
  if n<0:
    divisor=n-n
    n/divisor
  else:
    try descend(n-1)
fn probe(n:int)->int:
  callback=descend
  callback(n) catch fn(error):
    match error:
      E.Bad {text,values}:values[0]+values[1]+length(text)
fn main()->int:
  callback=probe
  probe(20)
]=] --test-entry probe)
EXPECT(raw 42)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/RecursiveCallableRuntimeTests.cpp" "${OUTPUT_DIR}/raw.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/context-harness")
RUN_OK("${OUTPUT_DIR}/context-harness")
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(NAME IN ITEMS worker pure-worker limit-recovery)
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_WORKERS=${WORKERS}" "${OUTPUT_DIR}/${NAME}")
    if(NOT OUTPUT STREQUAL "42\n")
      message(FATAL_ERROR "Recursive worker returned ${OUTPUT}")
    endif()
  endforeach()
endforeach()
# Build a source-valid value whose deliberately large inline native temporaries
# exceed stack admission. Rejection must preserve all existing output files.
set(FIELDS "")
set(VALUES "")
foreach(INDEX RANGE 0 127)
  string(APPEND FIELDS "  f${INDEX}:int\n")
  string(APPEND VALUES "f${INDEX}:input+${INDEX},")
endforeach()
file(WRITE "${OUTPUT_DIR}/reject-stack.k" "type Large:\n${FIELDS}fn make(input:int)->Large:\n  Large {${VALUES}}\nfn main()->int:\n  row=make(0)\n  row.f0\n")
foreach(KIND IN ITEMS binary ll mlir)
  file(WRITE "${OUTPUT_DIR}/reject-stack.${KIND}" "preserve-${KIND}")
endforeach()
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-stack.k"
  -o "${OUTPUT_DIR}/reject-stack.binary" --emit-llvm "${OUTPUT_DIR}/reject-stack.ll"
  --emit-mlir "${OUTPUT_DIR}/reject-stack.mlir" ${EXTRA}
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1" OR NOT ERR MATCHES "native stack budget")
  message(FATAL_ERROR "Oversized native stack not rejected: ${STATUS}: ${OUT}${ERR}")
endif()
foreach(KIND IN ITEMS binary ll mlir)
  file(READ "${OUTPUT_DIR}/reject-stack.${KIND}" KEPT)
  if(NOT KEPT STREQUAL "preserve-${KIND}")
    message(FATAL_ERROR "Stack rejection overwrote ${KIND}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/tests/fixtures/recursive-callables/self.k" DEEP)
string(REPLACE "callback(20)" "callback(1000000)" DEEP "${DEEP}")
COMPILE(deep "${DEEP}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/deep"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR NOT ERR MATCHES "StackExhaustionFault" OR NOT ERR MATCHES "cleanup: live=0 allocations=([0-9]+) releases=([0-9]+)")
  message(FATAL_ERROR "Deep recursion did not fail cleanly: ${STATUS}: ${OUT}${ERR}")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
  message(FATAL_ERROR "Deep recursive CLI leaked owners: ${ERR}")
endif()
