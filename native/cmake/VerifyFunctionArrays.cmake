include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
COMPILE(named [=[fn double(value:i64)->i64:
  return value*2
fn triple(value:i64)->i64:
  return value*3
fn main()->i64:
  callbacks=[double,triple]
  return callbacks[0](12)+callbacks[1](6)
]=])
EXPECT(named 42)
file(READ "${SOURCE_DIR}/../docs/design/fixtures/function-arrays.k" GUIDE)
COMPILE(captured "${GUIDE}")
EXPECT(captured 42)
COMPILE(owned [=[fn make()->[fn(i64)->i64]:
  text="é"+"!"
  values:[i64]=[20,20]
  return [fn(value):return values[0]+values[1]+length(text)+value]
fn main()->i64:
  callbacks=make()
  return callbacks[0](0)
]=])
EXPECT(owned 42)
COMPILE(empty [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[]
  return length(callbacks)+42
]=])
EXPECT(empty 42)
COMPILE(slice-concat [=[fn make(offset:i64)->[fn(i64)->i64]:
  return [fn(value):return value+offset]
fn main()->i64:
  callbacks=concat(make(10),make(20))
  selected=slice(callbacks,1,2)
  return selected[0](22)
]=])
EXPECT(slice-concat 42)
COMPILE(map-input [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value*2,fn(value):return value*3]
  values=map(callbacks,fn(callback):return callback(8))
  return values[0]+values[1]+2
]=])
EXPECT(map-input 42)
COMPILE(map-output [=[fn main()->i64:
  callbacks=map([20,21],fn(offset):return fn(value:i64):return value+offset)
  return callbacks[0](1)+callbacks[1](0)
]=])
EXPECT(map-output 42)
COMPILE(map-bound [=[fn main()->i64:
  maker=fn(offset:i64)->fn(i64)->i64:return fn(value:i64):return value+offset
  callbacks=map([20,21],maker)
  evaluate=fn(callback:fn(i64)->i64)->i64:return callback(0)
  values=map(callbacks,evaluate)
  return values[0]+values[1]+1
]=])
EXPECT(map-bound 42)
COMPILE(map-named [=[fn make(offset:i64)->fn(i64)->i64:
  return fn(value):return value+offset
fn evaluate(callback:fn(i64)->i64)->i64:
  return callback(0)
fn main()->i64:
  callbacks=map([20,21],make)
  values=map(callbacks,evaluate)
  return values[0]+values[1]+1
]=])
EXPECT(map-named 42)
COMPILE(fields [=[type Config:
  callbacks:[fn(i64)->i64]
fn make(offset:i64)->Config:
  Config {callbacks:[fn(value):value+offset]}
fn main()->i64:
  config=make(20)
  config.callbacks[0](22)
]=])
EXPECT(fields 42)
COMPILE(enum [=[enum Choice:
  Empty
  Some:
    callbacks:[fn(i64)->i64]
fn main()->i64:
  offset:i64=20
  choice=Choice.Some {callbacks:[fn(value):value+offset]}
  match choice:
    Choice.Empty:0
    Choice.Some {callbacks}:callbacks[0](22)
]=])
EXPECT(enum 42)
COMPILE(fallible [=[error E:
  Bad:
    value:i64
fn make(offset:i64)->[fn(i64)->i64 ! E]:
  validate=fn(value:i64)->i64 ! E:
    if value==0:fail E.Bad {value:offset}
    value
  [validate]
fn main()->i64:
  callbacks=make(42)
  callbacks[0](0) catch fn(error):
    match error:
      E.Bad {value}:value
]=])
EXPECT(fallible 42)
COMPILE(error-array [=[error E:
  Bad:
    callbacks:[fn(i64)->i64]
fn raise(offset:i64)->i64 ! E:
  fail E.Bad {callbacks:[fn(value):value+offset]}
fn main()->i64:
  raise(20) catch fn(error):
    match error:
      E.Bad {callbacks}:callbacks[0](22)
]=])
EXPECT(error-array 42)
COMPILE(indirect [=[fn evaluate(callbacks:[fn(i64)->i64])->i64:
  return callbacks[0](22)
fn make(offset:i64)->[fn(i64)->i64]:
  return [fn(value):return value+offset]
fn main()->i64:
  factory=make
  apply=evaluate
  return apply(factory(20))
]=])
EXPECT(indirect 42)
COMPILE(capture-array [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value*2]
  run=fn(value:i64):return callbacks[0](value)
  run(21)
]=])
EXPECT(capture-array 42)
COMPILE(conditional [=[fn main()->i64:
  first:[fn(i64)->i64]=[fn(value):return value*2]
  second:[fn(i64)->i64]=[fn(value):return value,fn(value):return value*3]
  callbacks=if true:first
  else:second
  return callbacks[0](21)
]=])
EXPECT(conditional 42)
COMPILE(worker-local [=[fn worker(offset:i64)->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value+offset]
  return callbacks[0](22)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-local 42)
COMPILE(coordinator [=[fn worker(value:i64)->i64:
  return value
fn main()->i64 ! TaskError:
  callbacks:[fn(i64)->i64]=[fn(value):return value*2]
  return try parallel:
    value=try await spawn worker(21)
    callbacks[0](value)
]=])
EXPECT(coordinator 42)
COMPILE(pipeline [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value*2]
  first=10 -> callbacks[0]
  second=11 -> callbacks[0]()
  return first+second
]=])
EXPECT(pipeline 42)
COMPILE(empty-map [=[fn main()->i64:
  values:[i64]=[]
  maker=fn(offset:i64)->fn(i64)->i64:
    bad=1/offset
    return fn(value:i64):return value+bad
  callbacks=map(values,maker)
  return length(callbacks)+42
]=])
EXPECT(empty-map 42)
COMPILE(factory-error [=[error E:
  Bad
fn make(offset:i64)->[fn(i64)->i64] ! E:
  if offset==0:fail E.Bad
  return [fn(value):return value+offset]
fn main()->i64:
  factory=make
  callbacks=factory(0) catch fn(error):return [fn(value:i64):return value*2]
  return callbacks[0](21)
]=])
EXPECT(factory-error 42)
COMPILE(worker-recovery [=[error E:
  Bad:
    callbacks:[fn(i64)->i64]
fn raise(offset:i64)->i64 ! E:
  fail E.Bad {callbacks:[fn(value):value+offset]}
fn worker(offset:i64)->i64:
  raise(offset) catch fn(error):
    match error:
      E.Bad {callbacks}:callbacks[0](22)
fn main()->i64 ! TaskError:
  try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-recovery 42)
COMPILE(bounds [=[fn worker(index:i64)->i64:
  callbacks:[fn(i64)->i64]=[fn(value):1/value]
  callbacks[index](0)
fn attempt()->i64 ! TaskError:
  try parallel:
    try await spawn worker(1)
fn main()->i64:
  attempt() catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Bounds:42
          _:0
      _:0
]=])
EXPECT(bounds 42)
COMPILE(fallible-map [=[error E:
  Bad:
    value:i64
fn main()->i64:
  offset:i64=21
  failer=fn(value:i64)->i64 ! E:fail E.Bad {value:offset}
  callbacks:[fn(i64)->i64 ! E]=[failer,failer]
  evaluate=fn(callback:fn(i64)->i64 ! E)->i64:
    callback(0) catch fn(error):
      match error:
        E.Bad {value}:value
  values=map(callbacks,evaluate)
  values[0]+values[1]
]=])
EXPECT(fallible-map 42)
COMPILE(separate-errors [=[error InputError:
  Bad:
    value:i64
error BuildError:
  Disabled
fn make(offset:i64)->[fn(i64)->i64 ! InputError] ! BuildError:
  if offset==0:fail BuildError.Disabled
  callback=fn(value:i64)->i64 ! InputError:
    if value==0:fail InputError.Bad {value:offset}
    value
  [callback]
fn main()->i64:
  factory=make
  callbacks=factory(42) catch fn(error):[fn(value:i64)->i64 ! InputError:value]
  callbacks[0](0) catch fn(error):
    match error:
      InputError.Bad {value}:value
]=])
EXPECT(separate-errors 42)
COMPILE(raw-returned [=[type Row:
  text:String
  values:[i64]
fn make(offset:i64)->[fn(i64)->i64]:
  row=Row {text:"é"+"!",values:[10,20]}
  [fn(value):row.values[0]+row.values[1]+length(row.text)+offset+value,fn(value):offset+value]
fn main()->i64:
  callbacks=make(10)
  callbacks[0](0)
]=] --test-entry make)
EXPECT(raw-returned 42)
COMPILE(raw-owned [=[fn make()->[fn(i64)->i64]:
  text="é"+"!"
  values:[i64]=[20,20]
  return [fn(value):return values[0]+values[1]+length(text)+value]
fn probe(input:i64)->i64:
  callbacks=make()
  return callbacks[0](input-40)
fn main()->i64:
  return probe(40)
]=] --test-entry probe)
EXPECT(raw-owned 42)
foreach(TYPE IN ITEMS i8 u8 i16 u16 i32 u32 i64 u64 f32 f64)
  COMPILE("width-${TYPE}" "fn make(offset:${TYPE})->[fn(${TYPE})->${TYPE}]:\n  return [fn(value):return value+offset]\nfn main()->${TYPE}:\n  callbacks=make(${TYPE}(20))\n  return callbacks[0](${TYPE}(22))\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ClosureRuntimeTests.cpp" "${OUTPUT_DIR}/raw-owned.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/FunctionArrayRuntimeTests.cpp" "${OUTPUT_DIR}/raw-returned.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-returned-harness")
RUN_OK("${OUTPUT_DIR}/raw-returned-harness")
file(WRITE "${OUTPUT_DIR}/reject-missing-parameter.k" [=[fn main()->i64:
  callbacks=[fn(value):return value]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-missing-parameter.k" -o "${OUTPUT_DIR}/reject-missing-parameter" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: missing-parameter: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-empty-context.k" [=[fn main()->i64:
  callbacks=[]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-empty-context.k" -o "${OUTPUT_DIR}/reject-empty-context" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: empty-context: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-mixed-parameters.k" [=[fn main()->i64:
  callbacks=[fn(value:i64):return value,fn(value:f64):return value]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-mixed-parameters.k" -o "${OUTPUT_DIR}/reject-mixed-parameters" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: mixed-parameters: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-mixed-results.k" [=[fn main()->i64:
  callbacks=[fn(value:i64):return value,fn(value:i64)->f64:return f64(value)]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-mixed-results.k" -o "${OUTPUT_DIR}/reject-mixed-results" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: mixed-results: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-context.k" [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value:f64):return value]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-context.k" -o "${OUTPUT_DIR}/reject-wrong-context" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: wrong-context: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-mixed-errors.k" [=[error A:
  Bad
error B:
  Bad
fn main()->i64:
  first=fn(value:i64)->i64 ! A:return value
  second=fn(value:i64)->i64 ! B:return value
  callbacks=[first,second]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-mixed-errors.k" -o "${OUTPUT_DIR}/reject-mixed-errors" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: mixed-errors: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-call-array.k" [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  return callbacks(42)
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-call-array.k" -o "${OUTPUT_DIR}/reject-call-array" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: call-array: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-call.k" [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  return callbacks[0]("bad")
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-call.k" -o "${OUTPUT_DIR}/reject-wrong-call" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: wrong-call: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-concat.k" [=[fn main()->i64:
  first=[fn(value:i64):return value]
  second=[fn(value:f64):return value]
  callbacks=concat(first,second)
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-concat.k" -o "${OUTPUT_DIR}/reject-wrong-concat" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: wrong-concat: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-constant-bounds.k" [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  return callbacks[1](42)
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-constant-bounds.k" -o "${OUTPUT_DIR}/reject-constant-bounds" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: constant-bounds: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-unhandled.k" [=[error E:
  Bad
fn main()->i64:
  callback=fn(value:i64)->i64 ! E:return value
  callbacks=[callback]
  return callbacks[0](42)+1
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-unhandled.k" -o "${OUTPUT_DIR}/reject-unhandled" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: unhandled: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-fallible-map-aggregation.k" [=[error E:
  Bad
fn main()->i64:
  callback=fn(value:i64)->i64 ! E:return value
  callbacks=[callback]
  evaluate=fn(fnValue:fn(i64)->i64 ! E)->i64 ! E:return try fnValue(0)
  values=map(callbacks,evaluate)
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-fallible-map-aggregation.k" -o "${OUTPUT_DIR}/reject-fallible-map-aggregation" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: fallible-map-aggregation: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(supported-worker-argument [=[fn worker(callbacks:[fn(i64)->i64])->i64:
  return callbacks[0](42)
fn main()->i64 ! TaskError:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  return try parallel:
    try await spawn worker(callbacks)
]=])
EXPECT(supported-worker-argument 42)
COMPILE(supported-worker-result [=[fn worker(offset:i64)->[fn(i64)->i64]:
  return [fn(value):return value+offset]
fn main()->i64 ! TaskError:
  return try parallel:
    callbacks=try await spawn worker(20)
    callbacks[0](22)
]=])
EXPECT(supported-worker-result 42)
COMPILE(supported-worker-nested-field [=[type Config:
  callbacks:[fn(i64)->i64]
fn worker(config:Config)->i64:
  config.callbacks[0](42)
fn main()->i64 ! TaskError:
  config=Config {callbacks:[fn(value):value]}
  try parallel:
    try await spawn worker(config)
]=])
EXPECT(supported-worker-nested-field 42)
COMPILE(supported-worker-error-payload [=[error E:
  Bad:
    callbacks:[fn(i64)->i64]
fn worker(value:i64)->i64 ! E:
  return value
fn main()->i64 ! (E | TaskError):
  return try parallel:
    try await spawn worker(42)
]=])
EXPECT(supported-worker-error-payload 42)
COMPILE(supported-worker-map-input [=[fn main()->i64 ! TaskError:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  return try parallel:
    values=try await spawn map(callbacks,fn(callback):return callback(42))
    values[0]
]=])
EXPECT(supported-worker-map-input 42)
COMPILE(supported-worker-map-result [=[fn main()->i64 ! TaskError:
  return try parallel:
    values=try await spawn map([20],fn(offset):return fn(value:i64):return offset+value)
    length(values)
]=])
EXPECT(supported-worker-map-result 1)
COMPILE(supported-worker-query [=[fn main()->i64 ! TaskError:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  return try parallel:
    try await spawn length(callbacks)
]=])
EXPECT(supported-worker-query 1)
COMPILE(supported-worker-slice [=[fn main()->i64 ! TaskError:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  return try parallel:
    values=try await spawn slice(callbacks,0,1)
    length(values)
]=])
EXPECT(supported-worker-slice 1)
COMPILE(supported-coordination [=[fn worker(value:i64)->i64:
  return value
fn coordinate(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn worker(value)
fn main()->i64:
  callbacks=[coordinate]
  return 0
]=])
EXPECT(supported-coordination 0)
file(WRITE "${OUTPUT_DIR}/reject-nested-array-type.k" [=[fn main()->i64:
  callbacks:[[fn(i64)->i64]]=[]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-nested-array-type.k" -o "${OUTPUT_DIR}/reject-nested-array-type" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: nested-array-type: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-function-tensor.k" [=[fn main()->i64:
  callbacks:[fn(i64)->i64]=[fn(value):return value]
  value=tensor(callbacks)
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-function-tensor.k" -o "${OUTPUT_DIR}/reject-function-tensor" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: function-tensor: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-nominal.k" [=[type A:
  value:i64
type B:
  value:i64
fn main()->i64:
  callbacks:[fn(A)->i64]=[fn(value:B):return value.value]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-nominal.k" -o "${OUTPUT_DIR}/reject-wrong-nominal" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: wrong-nominal: ${STATUS}: ${OUT}${ERR}")
endif()
foreach(NAME IN ITEMS owned fields error-array map-output map-bound indirect)
  foreach(ATTEMPT RANGE 1 40)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${ATTEMPT}" KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/${NAME}" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
    if(NOT "${STATUS}" MATCHES "^(0|86|88)$" OR ERR MATCHES "AddressSanitizer|runtime error:|ThreadSanitizer")
      message(FATAL_ERROR "Function array allocation probe failed: ${NAME} ${ATTEMPT}: ${STATUS}: ${OUT}${ERR}")
    endif()
    if("${STATUS}" STREQUAL "0")
      if(NOT OUT STREQUAL "42\n")
        message(FATAL_ERROR "Function array output changed: ${NAME}: ${OUT}${ERR}")
      endif()
    elseif(NOT OUT STREQUAL "" OR NOT ERR MATCHES "cleanup: live=0 allocations=([0-9]+) releases=([0-9]+)" OR NOT CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
      message(FATAL_ERROR "Function array failure leaked or published data: ${NAME}: ${OUT}${ERR}")
    endif()
  endforeach()
endforeach()
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(NAME IN ITEMS worker-local worker-recovery coordinator bounds)
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_WORKERS=${WORKERS}" "${OUTPUT_DIR}/${NAME}")
    if(NOT OUTPUT STREQUAL "42\n")
      message(FATAL_ERROR "Worker-local function array changed: ${NAME}: ${OUTPUT}")
    endif()
  endforeach()
endforeach()
file(WRITE "${OUTPUT_DIR}/helper.k" "export fn make(offset:i64)->[fn(i64)->i64]:\n  return [fn(value):return value+offset]\n")
COMPILE(imported "from \"./helper.k\" import make\nfn main()->i64:\n  callbacks=make(20)\n  return callbacks[0](22)\n")
EXPECT(imported 42)
foreach(ARTIFACT IN ITEMS rejected rejected.ll rejected.mlir)
  file(WRITE "${OUTPUT_DIR}/${ARTIFACT}" "preserved")
endforeach()
file(WRITE "${OUTPUT_DIR}/rejected.k" "fn main()->i64:\n  callbacks:[fn(i64)->i64]=[fn(value:f64):return value]\n  return 0\n")
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/rejected.k" -o "${OUTPUT_DIR}/rejected" --emit-llvm "${OUTPUT_DIR}/rejected.ll" --emit-mlir "${OUTPUT_DIR}/rejected.mlir" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1" OR NOT ERR MATCHES "S060")
  message(FATAL_ERROR "Invalid function array signature diagnostic changed: ${OUT}${ERR}")
endif()
foreach(ARTIFACT IN ITEMS rejected rejected.ll rejected.mlir)
  file(READ "${OUTPUT_DIR}/${ARTIFACT}" CONTENT)
  if(NOT CONTENT STREQUAL "preserved")
    message(FATAL_ERROR "Rejected function array replaced output artifacts")
  endif()
endforeach()
message(STATUS "Function arrays: signatures, checked indexing, maps, owned captures and worker boundaries passed")
