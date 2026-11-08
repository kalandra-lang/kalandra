include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
file(READ "${SOURCE_DIR}/../docs/design/fixtures/control-maps.k" GUIDE)
COMPILE(named "${GUIDE}")
EXPECT(named 42)
COMPILE(inline-capture [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn main()->i64:
  offset:i64=21
  result=map([0,21],fn(value):return checked(value) catch fn(error):return offset)
  return result[0]+result[1]
]=])
EXPECT(inline-capture 42)
COMPILE(enum [=[enum Choice:
  Empty
  Some:
    value:i64
fn select(choice:Choice)->i64:
  return match choice:
    Choice.Empty:20
    Choice.Some {value}:value
fn main()->i64:
  result=map([Choice.Empty,Choice.Some {value:22}],select)
  return result[0]+result[1]
]=])
EXPECT(enum 42)
COMPILE(bool-string [=[enum Choice:
  Empty
  Some:
    value:i64
fn select(choice:Choice)->bool:
  return match choice:
    Choice.Empty:false
    Choice.Some {value}:value==22
fn label(value:bool)->String:
  return if value:"é"
  else:"!"
fn main()->i64:
  values=map([Choice.Empty,Choice.Some {value:22}],select)
  labels=map(values,label)
  return length(labels[0])+length(labels[1])+40
]=])
EXPECT(bool-string 42)
COMPILE(owned-record [=[type Row:
  text:String
  values:[i64]
error E:
  Bad:
    row:Row
fn checked(value:i64)->Row ! E:
  if value==0:fail E.Bad {row:Row {text:"é"+"!",values:[20,20]}}
  return Row {text:"!",values:[value]}
fn recover(value:i64)->Row:
  return checked(value) catch fn(error):
    return match error:
      E.Bad {row}:row
fn main()->i64:
  rows=map([0,1],recover)
  return rows[0].values[0]+rows[0].values[1]+length(rows[0].text)
]=])
EXPECT(owned-record 42)
COMPILE(nested [=[enum Choice:
  Empty
  Some:
    value:i64
fn select(choice:Choice)->i64:
  return match choice:
    Choice.Empty:10
    Choice.Some {value}:value
fn batch(value:i64)->i64:
  results=map([Choice.Empty,Choice.Some {value:value}],select)
  return results[0]+results[1]
fn main()->i64:
  results=map([10,12],batch)
  return results[0]+results[1]
]=])
EXPECT(nested 42)
COMPILE(function-input-output [=[enum Choice:
  Empty
  Some:
    value:i64
fn make(choice:Choice)->fn(i64)->i64:
  return match choice:
    Choice.Empty:fn(value:i64):return value+20
    Choice.Some {value}:fn(extra:i64):return value+extra
fn apply(callback:fn(i64)->i64)->i64:
  return match Choice.Empty:
    Choice.Empty:callback(1)
    Choice.Some {value}:value
fn main()->i64:
  callbacks=map([Choice.Empty,Choice.Some {value:20}],make)
  results=map(callbacks,apply)
  return results[0]+results[1]
]=])
EXPECT(function-input-output 42)
COMPILE(empty [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):return 1/(value-value)
fn main()->i64:
  values:[i64]=[]
  result=map(values,recover)
  return length(result)+42
]=])
EXPECT(empty 42)
COMPILE(worker [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn worker(offset:i64)->[i64]:
  return map([0,21],fn(value):return checked(value) catch fn(error):return offset)
fn main()->i64 ! TaskError:
  return try parallel:
    task=spawn worker(21)
    first=try await task
    second=try await task
    first[0]+second[1]
]=])
EXPECT(worker 42)
COMPILE(direct-spawn [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):return 21
fn main()->i64 ! TaskError:
  return try parallel:
    task=spawn map([0,21],recover)
    values=try await task
    values[0]+values[1]
]=])
EXPECT(direct-spawn 42)
COMPILE(owned-capture [=[type Row:
  text:String
  values:[i64]
error E:
  Bad
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn probe()->i64:
  input:i64=40
  row=Row {text:"é"+"!",values:[input]}
  result=map([0,1],fn(value):return checked(value) catch fn(error):return row.values[0]+length(row.text)-1)
  return result[0]+result[1]
fn main()->i64:
  f=probe
  return probe()
]=] --test-entry probe)
EXPECT(owned-capture 42)
foreach(TYPE IN ITEMS i8 u8 i16 u16 i32 u32 i64 u64 f32 f64)
  COMPILE("width-${TYPE}" "error E:\n  Bad\nfn checked(value:${TYPE})->${TYPE} ! E:\n  if value==${TYPE}(0):fail E.Bad\n  return value\nfn recover(value:${TYPE})->${TYPE}:\n  return checked(value) catch fn(error):return ${TYPE}(21)\nfn main()->${TYPE}:\n  values=map([${TYPE}(0),${TYPE}(21)],recover)\n  return values[0]+values[1]\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ControlMapRuntimeTests.cpp" "${OUTPUT_DIR}/owned-capture.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/allocation-harness")
RUN_OK("${OUTPUT_DIR}/allocation-harness")
COMPILE(fault-order [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn recover(value:i64)->i64:
  return checked(value) catch fn(error):return 42/value
fn source()->i64:
  return 0
fn probe()->i64:
  input=source()
  values=map([input,1],recover)
  later=1/(input-input)
  return values[0]+later
fn main()->i64:
  f=probe
  return probe()
]=] --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ControlMapFaultRuntimeTests.cpp" "${OUTPUT_DIR}/fault-order.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/fault-harness")
RUN_OK("${OUTPUT_DIR}/fault-harness")

COMPILE(direct-spawn-capture [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad
  return value
fn main()->i64 ! TaskError:
  offset:i64=21
  return try parallel:
    task=spawn map([0,21],fn(value):return checked(value) catch fn(error):return offset)
    first=try await task
    second=try await task
    first[0]+second[1]
]=])
EXPECT(direct-spawn-capture 42)
COMPILE(owned-worker [=[type Row:
  text:String
  values:[i64]
error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn worker(input:i64)->[Row]:
  return map([input],fn(value):return Row {text:"é"+"!",values:[checked(value) catch fn(error):return value]})
fn main()->i64 ! TaskError:
  return try parallel:
    task=spawn worker(40)
    first=try await task
    second=try await task
    first[0].values[0]+length(second[0].text)
]=])
EXPECT(owned-worker 42)
COMPILE(try-helper [=[error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:21}
  return value
fn forwarded(value:i64)->i64 ! E:
  return try checked(value)
fn recover(value:i64)->i64:
  return forwarded(value) catch fn(error):
    return match error:
      E.Bad {value}:value
fn main()->i64:
  values=map([0,21],recover)
  return values[0]+values[1]
]=])
EXPECT(try-helper 42)
COMPILE(captured-functions [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value+20]
  values=map([1,1],fn(value):return checked(value) catch fn(error):return callbacks[0](value))
  return values[0]+values[1]
]=])
EXPECT(captured-functions 42)
file(WRITE "${OUTPUT_DIR}/service.k" [=[error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:21}
  return value
export fn recover(value:i64)->i64:
  return checked(value) catch fn(error):
    return match error:
      E.Bad {value}:value
]=])
COMPILE(imported [=[from "./service.k" import recover
fn main()->i64:
  values=map([0,21],recover)
  return values[0]+values[1]
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
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn main()->i64:
  values=map([1],checked)
  return values[0]
]=])
REJECT(unhandled [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn main()->i64:
  values=map([1],fn(value):return try checked(value))
  return values[0]
]=])
REJECT(coordination [=[fn identity(value:i64)->i64:
  return value
fn attempted(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn identity(value)
fn recover(value:i64)->i64:
  return attempted(value) catch fn(error):return 0
fn main()->i64:
  values=map([1],recover)
  return values[0]
]=])
COMPILE(transport [=[error E:
  Bad
fn checked(value:i64)->i64 ! E:
  fail E.Bad
fn recover(callback:fn(i64)->i64)->i64:
  return checked(callback(0)) catch fn(error):return 42
fn main()->i64 ! TaskError:
  callbacks=[fn(value:i64):return value]
  return try parallel:
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
