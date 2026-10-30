include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
file(GLOB SOURCES "${SOURCE_DIR}/tests/fixtures/delimited-closures/*.k")
foreach(PATH IN LISTS SOURCES)
  get_filename_component(NAME "${PATH}" NAME_WE)
  file(READ "${PATH}" TEXT)
  COMPILE("${NAME}" "${TEXT}")
  if(NAME STREQUAL "hero")
    EXPECT("${NAME}" "[0, 10, 15]")
  else()
    EXPECT("${NAME}" 42)
  endif()
endforeach()
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  COMPILE("width-${TYPE}" "fn main()->${TYPE}:\n  offset:${TYPE}=${TYPE}(20)\n  values=map([${TYPE}(1),${TYPE}(1)],fn(value):\n    adjusted=value+offset\n    adjusted\n  )\n  values[0]+values[1]\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(NAME IN ITEMS worker direct-worker)
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_WORKERS=${WORKERS}" "${OUTPUT_DIR}/${NAME}")
    if(NOT OUTPUT STREQUAL "42\n")
      message(FATAL_ERROR "Delimited worker callback returned ${OUTPUT}")
    endif()
  endforeach()
endforeach()
file(READ "${SOURCE_DIR}/../docs/design/fixtures/delimited-closures.k" GUIDE)
COMPILE(guide "${GUIDE}")
EXPECT(guide "[0, 10, 15]")
COMPILE(raw-owned [=[type Row:
  text:String
  values:[int]
error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn probe()->int:
  input:int=40
  row=Row {text:"é"+"!",values:[input]}
  values=map([0,1],fn(value):
    checked(value) catch fn(error):
      adjusted=row.values[0]+length(row.text)-1
      adjusted
  )
  values[0]+values[1]
fn main()->int:
  f=probe
  probe()
]=] --test-entry probe)
EXPECT(raw-owned 42)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ControlMapRuntimeTests.cpp" "${OUTPUT_DIR}/raw-owned.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/allocation-harness")
RUN_OK("${OUTPUT_DIR}/allocation-harness")
COMPILE(raw-fault [=[error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn recover(value:int)->int:
  checked(value) catch fn(error):42/value
fn source()->int:
  0
fn probe()->int:
  input=source()
  values=map([input,1],fn(value):
    adjusted=recover(value)
    adjusted
  )
  later=1/(input-input)
  values[0]+later
fn main()->int:
  f=probe
  probe()
]=] --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ControlMapFaultRuntimeTests.cpp" "${OUTPUT_DIR}/raw-fault.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/fault-harness")
RUN_OK("${OUTPUT_DIR}/fault-harness")
function(REJECT NAME SOURCE CODE)
  file(WRITE "${OUTPUT_DIR}/reject-${NAME}.k" "${SOURCE}")
  foreach(KIND IN ITEMS binary ll mlir)
    file(WRITE "${OUTPUT_DIR}/reject-${NAME}.${KIND}" "preserve-${KIND}")
  endforeach()
  execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-${NAME}.k"
    -o "${OUTPUT_DIR}/reject-${NAME}.binary" --emit-llvm "${OUTPUT_DIR}/reject-${NAME}.ll"
    --emit-mlir "${OUTPUT_DIR}/reject-${NAME}.mlir" ${EXTRA}
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "1" OR NOT ERR MATCHES "error\\[${CODE}\\]")
    message(FATAL_ERROR "Invalid delimited suite ${NAME}: ${STATUS}: ${OUT}${ERR}")
  endif()
  foreach(KIND IN ITEMS binary ll mlir)
    file(READ "${OUTPUT_DIR}/reject-${NAME}.${KIND}" KEPT)
    if(NOT KEPT STREQUAL "preserve-${KIND}")
      message(FATAL_ERROR "Invalid suite overwrote ${NAME}.${KIND}")
    endif()
  endforeach()
endfunction()
REJECT(empty "fn main():\n  map([1],fn(v):\n  )\n" P002)
REJECT(binding "fn main():\n  map([1],fn(v):\n    result=v+1\n  )\n" P007)
REJECT(else "fn main():\n  map([1],fn(v):\n    if v>0:\n      v\n  )\n" P035)
REJECT(inline-close "fn main():\n  map([1],fn(v):\n    v)\n" L016)
REJECT(indented-close "fn main():\n  map([1],fn(v):\n    v\n    )\n" L016)
REJECT(dedent "fn main():\n  map([1],fn(v):\n    x=v+1\n   x\n  )\n" L008)
REJECT(tab "fn main():\n  map([1],fn(v):\n\tv\n  )\n" L003)
REJECT(mismatch "fn main():\n  map([1],fn(v):\n    v\n  ]\n" L011)
REJECT(unclosed "fn main():\n  map([1],fn(v):\n    v\n" L005)
