set(EXTRA)
if(VARIANT STREQUAL "optimized")
  set(EXTRA -O2)
elseif(VARIANT STREQUAL "sanitized")
  set(EXTRA --sanitize)
elseif(VARIANT STREQUAL "thread-sanitized")
  set(EXTRA --thread-sanitize)
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
function(COMPILE NAME SOURCE)
  file(WRITE "${OUTPUT_DIR}/${NAME}.k" "${SOURCE}")
  execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/${NAME}.k" -o "${OUTPUT_DIR}/${NAME}" ${EXTRA}
    --emit-llvm "${OUTPUT_DIR}/${NAME}.ll" --emit-mlir "${OUTPUT_DIR}/${NAME}.mlir"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "0" OR NOT ERR STREQUAL "")
    message(FATAL_ERROR "Enum compile ${NAME}: ${STATUS}: ${OUT}${ERR}")
  endif()
endfunction()
function(EXPECT NAME EXPECTED)
  foreach(WORKERS IN ITEMS 1 2 4)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_WORKERS=${WORKERS}" "${OUTPUT_DIR}/${NAME}"
      RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
    if(NOT "${STATUS}" STREQUAL "0" OR NOT OUT STREQUAL "${EXPECTED}\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Enum ${NAME}/${WORKERS}: ${STATUS}: ${OUT}${ERR}; expected ${EXPECTED}")
    endif()
  endforeach()
endfunction()
set(TYPES i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 bool)
set(VALUES -128 255 -32768 65535 -2147483648 4294967295 -9223372036854775808 18446744073709551615 1.5 1.0000000000000002 true)
foreach(INDEX RANGE 0 10)
  list(GET TYPES ${INDEX} TYPE)
  list(GET VALUES ${INDEX} VALUE)
  COMPILE("payload-${TYPE}" "enum Sample:\n  Empty\n  Value:\n    item:${TYPE}\nfn valid(x:Sample)->bool:\n  return match x:\n    Sample.Empty:false\n    Sample.Value {item}:item==${VALUE}\nfn echo(x:Sample)->Sample:\n  return x\nfn child(x:Sample)->Sample ! TaskError:\n  return try parallel:\n    work=spawn echo(x)\n    first=try await work\n    again=try await work\n    again\nfn main()->i64 ! TaskError:\n  value=Sample.Value {item:${VALUE}}\n  return try parallel:\n    work=spawn child(value)\n    empty=spawn echo(Sample.Empty)\n    result=try await work\n    missing=try await empty\n    if valid(result):\n      match missing:\n        Sample.Empty:1\n        Sample.Value:0\n    else:0\n")
  EXPECT("payload-${TYPE}" 1)
endforeach()
COMPILE(shared-field "enum Number:\n  Small:\n    item:i8\n  Large:\n    item:u64\nfn extract(x:Number)->u64:\n  return match x:\n    Number.Small {item}:u64(item)\n    Number.Large {item}:item\nfn main()->i64:\n  return if extract(Number.Large {item:18446744073709551615})==18446744073709551615:1\n  else:0\n")
EXPECT(shared-field 1)
set(PREFIX "enum Reading:\n  Missing\n  Value:\n    measurement:i16\n  Invalid:\n    reason:String\nfn echo(x:Reading)->Reading:\n  return x\nfn child(x:Reading)->Reading ! TaskError:\n  return try parallel:\n    work=spawn echo(x)\n    first=try await work\n    again=try await work\n    again\n")
COMPILE(strings "${PREFIX}fn main()->i64 ! TaskError:\n  original=Reading.Invalid {reason:\"é\"+\"🙂\"}\n  result=try parallel:\n    work=spawn child(original)\n    try await work\n  return match result:\n    Reading.Invalid {reason:text}:length(text)+byteLength(text)\n    _:0\n")
EXPECT(strings 8)
COMPILE(child-string "${PREFIX}fn make()->Reading:\n  return Reading.Invalid {reason:\"é\"+\"🙂\"}\nfn main()->i64 ! TaskError:\n  result=try parallel:\n    work=spawn make()\n    first=try await work\n    again=try await work\n    again\n  return match result:\n    Reading.Invalid {reason}:length(reason)+byteLength(reason)\n    _:0\n")
EXPECT(child-string 8)
COMPILE(records "${PREFIX}type Box:\n  reading:Reading\n  enabled:bool\nfn change(x:Box)->Box:\n  return x with {reading:Reading.Value {measurement:42}}\nfn main()->i64 ! TaskError:\n  original=Box {reading:Reading.Invalid {reason:\"hello\"+\"!\"},enabled:true}\n  result=try parallel:\n    work=spawn change(original)\n    try await work\n  previous=match original.reading:\n    Reading.Invalid {reason}:length(reason)\n    _:0\n  return match result.reading:\n    Reading.Value {measurement}:i64(measurement)+previous\n    _:0\n")
EXPECT(records 48)
COMPILE(arrays "${PREFIX}fn score(x:Reading)->i64:\n  return match x:\n    Reading.Missing:0\n    Reading.Value {measurement}:i64(measurement)\n    Reading.Invalid {reason}:length(reason)\nfn copy(values:[Reading])->[Reading]:\n  return values\nfn main()->i64 ! TaskError:\n  original:[Reading]=[Reading.Missing,Reading.Value {measurement:42},Reading.Invalid {reason:\"hi\"}]\n  values=try parallel:\n    work=spawn copy(original)\n    try await work\n  return reduceSum(tensor(map(values,score)))\n")
EXPECT(arrays 44)
COMPILE(only "enum Single:\n  Item:\n    value:i64\nfn select(x:Single)->i64:\n  return match x:\n    Single.Item {value}:\n      next=value+1\n      next\nfn main()->i64 ! TaskError:\n  return try parallel:\n    work=spawn select(Single.Item {value:41})\n    try await work\n")
EXPECT(only 42)
COMPILE(fallback "${PREFIX}fn main()->i64:\n  return match Reading.Missing:\n    _:\n      answer=42\n      answer\n")
EXPECT(fallback 42)
COMPILE(selected-only "${PREFIX}fn bad(x:i64)->i64:\n  return 1/x\nfn main()->i64:\n  return match Reading.Missing:\n    Reading.Missing:42\n    Reading.Value:bad(0)\n    Reading.Invalid:bad(0)\n")
EXPECT(selected-only 42)
COMPILE(subject-once "fn bad(x:i64)->i64:\n  return 1/x\nenum Choice:\n  Value:\n    item:i64\nfn main()->i64:\n  return match Choice.Value {item:bad(0)}:\n    _:42\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/subject-once"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR NOT ERR MATCHES "ArithmeticFault at source 2:10" OR NOT ERR MATCHES "cleanup: live=0")
  message(FATAL_ERROR "Unused enum payload/subject was skipped or leaked: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/model.k" "export enum Reading:\n  Missing\n  Value:\n    item:i64\n")
file(WRITE "${OUTPUT_DIR}/facade.k" "export from \"./model.k\" import Reading as Result\n")
COMPILE(imported "from \"./facade.k\" import Result as R\nfn main()->i64:\n  return match R.Value {item:42}:\n    R.Missing:0\n    R.Value {item}:item\n")
EXPECT(imported 42)
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(ATTEMPT RANGE 1 24)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_WORKERS=${WORKERS}" "KALANDRA_TEST_FAIL_ALLOC_AT=${ATTEMPT}"
      KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/strings" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
    if("${STATUS}" STREQUAL "0")
      if(NOT OUT STREQUAL "8\n" OR NOT ERR STREQUAL "")
        message(FATAL_ERROR "Enum allocation success changed output: ${OUT}${ERR}")
      endif()
    elseif(("${STATUS}" STREQUAL "86" AND ERR MATCHES "AllocationFault") OR ("${STATUS}" STREQUAL "88" AND ERR MATCHES "TaskError.Fault"))
      if(NOT OUT STREQUAL "" OR NOT ERR MATCHES "cleanup: live=0")
        message(FATAL_ERROR "Enum allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
      endif()
    else()
      message(FATAL_ERROR "Unexpected enum allocation outcome: ${STATUS}: ${OUT}${ERR}")
    endif()
  endforeach()
endforeach()
file(WRITE "${OUTPUT_DIR}/preserved" "existing output")
file(WRITE "${OUTPUT_DIR}/preserved.ll" "existing IR")
file(WRITE "${OUTPUT_DIR}/bad.k" "${PREFIX}fn main()->i64:\n  return match Reading.Missing:\n    Reading.Missing:0\n")
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/bad.k" -o "${OUTPUT_DIR}/preserved" --emit-llvm "${OUTPUT_DIR}/preserved.ll"
  RESULT_VARIABLE STATUS ERROR_VARIABLE ERR)
file(READ "${OUTPUT_DIR}/preserved" OUT)
file(READ "${OUTPUT_DIR}/preserved.ll" IR)
if(NOT "${STATUS}" STREQUAL "1" OR NOT ERR MATCHES "S044" OR NOT OUT STREQUAL "existing output" OR NOT IR STREQUAL "existing IR")
  message(FATAL_ERROR "Non-exhaustive enum match did not preserve output files: ${STATUS}: ${ERR}")
endif()
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/strings.k" --target metal -o "${OUTPUT_DIR}/unsupported-metal"
  RESULT_VARIABLE STATUS ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1" OR EXISTS "${OUTPUT_DIR}/unsupported-metal")
  message(FATAL_ERROR "GPU enum unexpectedly enabled")
endif()
message(STATUS "CPU enums: 21 generated programs, ten numeric widths/Bool/String, exhaustive branches, module aliases, records/arrays, nested/cached workers, Unicode ownership and 72 allocation sweeps passed")

# A callback signature names a nominal type without embedding its storage.
COMPILE(callback-contract [=[type Node:
  visit:fn(Node)->i64
fn visit(node:Node)->i64:
  return 42
fn main()->i64:
  node=Node {visit:visit}
  return node.visit(node)
]=])
EXPECT(callback-contract 42)
COMPILE(worker-callback-contract [=[type Node:
  visit:fn(Node)->i64
fn visit(node:Node)->i64:
  return 42
fn run(node:Node)->i64:
  return node.visit(node)
fn main()->i64 ! TaskError:
  node=Node {visit:visit}
  return try parallel:
    work=spawn run(node)
    try await work
]=])
EXPECT(worker-callback-contract 42)
