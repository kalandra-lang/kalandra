include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
file(READ "${SOURCE_DIR}/../docs/design/fixtures/control-maps.k" GUIDE)
COMPILE(named "${GUIDE}")
EXPECT(named 42)
COMPILE(inline-capture [=[error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn main()->int:
  offset:int=21
  result=map([0,21],fn(value):checked(value) catch fn(error):offset)
  result[0]+result[1]
]=])
EXPECT(inline-capture 42)
COMPILE(enum [=[enum Choice:
  Empty
  Some:
    value:int
fn select(choice:Choice)->int:
  match choice:
    Choice.Empty:20
    Choice.Some {value}:value
fn main()->int:
  result=map([Choice.Empty,Choice.Some {value:22}],select)
  result[0]+result[1]
]=])
EXPECT(enum 42)
COMPILE(bool-string [=[enum Choice:
  Empty
  Some:
    value:int
fn select(choice:Choice)->Bool:
  match choice:
    Choice.Empty:false
    Choice.Some {value}:value==22
fn label(value:Bool)->String:
  if value:"é"
  else:"!"
fn main()->int:
  values=map([Choice.Empty,Choice.Some {value:22}],select)
  labels=map(values,label)
  length(labels[0])+length(labels[1])+40
]=])
EXPECT(bool-string 42)
COMPILE(owned-record [=[type Row:
  text:String
  values:[int]
error E:
  Bad:
    row:Row
fn checked(value:int)->Row ! E:
  if value==0:fail E.Bad {row:Row {text:"é"+"!",values:[20,20]}}
  Row {text:"!",values:[value]}
fn recover(value:int)->Row:
  checked(value) catch fn(error):
    match error:
      E.Bad {row}:row
fn main()->int:
  rows=map([0,1],recover)
  rows[0].values[0]+rows[0].values[1]+length(rows[0].text)
]=])
EXPECT(owned-record 42)
COMPILE(nested [=[enum Choice:
  Empty
  Some:
    value:int
fn select(choice:Choice)->int:
  match choice:
    Choice.Empty:10
    Choice.Some {value}:value
fn batch(value:int)->int:
  results=map([Choice.Empty,Choice.Some {value:value}],select)
  results[0]+results[1]
fn main()->int:
  results=map([10,12],batch)
  results[0]+results[1]
]=])
EXPECT(nested 42)
COMPILE(function-input-output [=[enum Choice:
  Empty
  Some:
    value:int
fn make(choice:Choice)->fn(int)->int:
  match choice:
    Choice.Empty:fn(value:int):value+20
    Choice.Some {value}:fn(extra:int):value+extra
fn apply(callback:fn(int)->int)->int:
  match Choice.Empty:
    Choice.Empty:callback(1)
    Choice.Some {value}:value
fn main()->int:
  callbacks=map([Choice.Empty,Choice.Some {value:20}],make)
  results=map(callbacks,apply)
  results[0]+results[1]
]=])
EXPECT(function-input-output 42)
COMPILE(empty [=[error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn recover(value:int)->int:
  checked(value) catch fn(error):1/(value-value)
fn main()->int:
  values:[int]=[]
  result=map(values,recover)
  length(result)+42
]=])
EXPECT(empty 42)
COMPILE(worker [=[error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn worker(offset:int)->[int]:
  map([0,21],fn(value):checked(value) catch fn(error):offset)
fn main()->int ! TaskError:
  try parallel:
    task=spawn worker(21)
    first=try await task
    second=try await task
    first[0]+second[1]
]=])
EXPECT(worker 42)
COMPILE(direct-spawn [=[error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn recover(value:int)->int:
  checked(value) catch fn(error):21
fn main()->int ! TaskError:
  try parallel:
    task=spawn map([0,21],recover)
    values=try await task
    values[0]+values[1]
]=])
EXPECT(direct-spawn 42)
COMPILE(owned-capture [=[type Row:
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
  result=map([0,1],fn(value):checked(value) catch fn(error):row.values[0]+length(row.text)-1)
  result[0]+result[1]
fn main()->int:
  f=probe
  probe()
]=] --test-entry probe)
EXPECT(owned-capture 42)
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  COMPILE("width-${TYPE}" "error E:\n  Bad\nfn checked(value:${TYPE})->${TYPE} ! E:\n  if value==${TYPE}(0):fail E.Bad\n  value\nfn recover(value:${TYPE})->${TYPE}:\n  checked(value) catch fn(error):${TYPE}(21)\nfn main()->${TYPE}:\n  values=map([${TYPE}(0),${TYPE}(21)],recover)\n  values[0]+values[1]\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ControlMapRuntimeTests.cpp" "${OUTPUT_DIR}/owned-capture.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/allocation-harness")
RUN_OK("${OUTPUT_DIR}/allocation-harness")
COMPILE(fault-order [=[error E:
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
  values=map([input,1],recover)
  later=1/(input-input)
  values[0]+later
fn main()->int:
  f=probe
  probe()
]=] --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ControlMapFaultRuntimeTests.cpp" "${OUTPUT_DIR}/fault-order.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/fault-harness")
RUN_OK("${OUTPUT_DIR}/fault-harness")

COMPILE(direct-spawn-capture [=[error E:
  Bad
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad
  value
fn main()->int ! TaskError:
  offset:int=21
  try parallel:
    task=spawn map([0,21],fn(value):checked(value) catch fn(error):offset)
    first=try await task
    second=try await task
    first[0]+second[1]
]=])
EXPECT(direct-spawn-capture 42)
COMPILE(owned-worker [=[type Row:
  text:String
  values:[int]
error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn worker(input:int)->[Row]:
  map([input],fn(value):Row {text:"é"+"!",values:[checked(value) catch fn(error):value]})
fn main()->int ! TaskError:
  try parallel:
    task=spawn worker(40)
    first=try await task
    second=try await task
    first[0].values[0]+length(second[0].text)
]=])
EXPECT(owned-worker 42)
COMPILE(try-helper [=[error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:21}
  value
fn forwarded(value:int)->int ! E:
  try checked(value)
fn recover(value:int)->int:
  forwarded(value) catch fn(error):
    match error:
      E.Bad {value}:value
fn main()->int:
  values=map([0,21],recover)
  values[0]+values[1]
]=])
EXPECT(try-helper 42)
COMPILE(captured-functions [=[error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value+20]
  values=map([1,1],fn(value):checked(value) catch fn(error):callbacks[0](value))
  values[0]+values[1]
]=])
EXPECT(captured-functions 42)
file(WRITE "${OUTPUT_DIR}/service.k" [=[error E:
  Bad:
    value:int
fn checked(value:int)->int ! E:
  if value==0:fail E.Bad {value:21}
  value
export fn recover(value:int)->int:
  checked(value) catch fn(error):
    match error:
      E.Bad {value}:value
]=])
COMPILE(imported [=[from "./service.k" import recover
fn main()->int:
  values=map([0,21],recover)
  values[0]+values[1]
]=])
EXPECT(imported 42)
function(REJECT NAME SOURCE)
  file(WRITE "${OUTPUT_DIR}/reject-${NAME}.k" "${SOURCE}")
  foreach(KIND IN ITEMS binary ll mlir)
    file(WRITE "${OUTPUT_DIR}/reject-${NAME}.${KIND}" "preserve-${KIND}")
  endforeach()
  execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-${NAME}.k"
    -o "${OUTPUT_DIR}/reject-${NAME}.binary" --emit-llvm "${OUTPUT_DIR}/reject-${NAME}.ll"
    --emit-mlir "${OUTPUT_DIR}/reject-${NAME}.mlir" ${EXTRA}
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "1" OR NOT ERR MATCHES "error\\[S[0-9]+\\]")
    message(FATAL_ERROR "Invalid map rejection ${NAME}: ${STATUS}: ${OUT}${ERR}")
  endif()
  foreach(KIND IN ITEMS binary ll mlir)
    file(READ "${OUTPUT_DIR}/reject-${NAME}.${KIND}" KEPT)
    if(NOT KEPT STREQUAL "preserve-${KIND}")
      message(FATAL_ERROR "Invalid map overwrote ${NAME}.${KIND}")
    endif()
  endforeach()
endfunction()
REJECT(fallible [=[error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn main()->int:
  values=map([1],checked)
  values[0]
]=])
REJECT(unhandled [=[error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn main()->int:
  values=map([1],fn(value):try checked(value))
  values[0]
]=])
REJECT(coordination [=[fn identity(value:int)->int:
  value
fn attempted(value:int)->int ! TaskError:
  try parallel:
    try await spawn identity(value)
fn recover(value:int)->int:
  attempted(value) catch fn(error):0
fn main()->int:
  values=map([1],recover)
  values[0]
]=])
COMPILE(transport [=[error E:
  Bad
fn checked(value:int)->int ! E:
  fail E.Bad
fn recover(callback:fn(int)->int)->int:
  checked(callback(0)) catch fn(error):42
fn main()->int ! TaskError:
  callbacks=[fn(value:int):value]
  try parallel:
    values=try await spawn map(callbacks,recover)
    values[0]
]=])
EXPECT(transport 42)
foreach(POOL IN ITEMS 1 2 4)
  foreach(NAME IN ITEMS worker direct-spawn direct-spawn-capture owned-worker)
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_WORKERS=${POOL}" "${OUTPUT_DIR}/${NAME}")
    if(NOT OUTPUT STREQUAL "42\n")
      message(FATAL_ERROR "Worker map returned ${OUTPUT}")
    endif()
  endforeach()
endforeach()
