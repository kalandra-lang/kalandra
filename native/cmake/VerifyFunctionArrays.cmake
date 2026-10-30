include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
COMPILE(named [=[fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn main()->int:
  callbacks=[double,triple]
  callbacks[0](12)+callbacks[1](6)
]=])
EXPECT(named 42)
file(READ "${SOURCE_DIR}/../docs/design/fixtures/function-arrays.k" GUIDE)
COMPILE(captured "${GUIDE}")
EXPECT(captured 42)
COMPILE(owned [=[fn make()->[fn(int)->int]:
  text="é"+"!"
  values:[int]=[20,20]
  [fn(value):values[0]+values[1]+length(text)+value]
fn main()->int:
  callbacks=make()
  callbacks[0](0)
]=])
EXPECT(owned 42)
COMPILE(empty [=[fn main()->int:
  callbacks:[fn(int)->int]=[]
  length(callbacks)+42
]=])
EXPECT(empty 42)
COMPILE(slice-concat [=[fn make(offset:int)->[fn(int)->int]:
  [fn(value):value+offset]
fn main()->int:
  callbacks=concat(make(10),make(20))
  selected=slice(callbacks,1,2)
  selected[0](22)
]=])
EXPECT(slice-concat 42)
COMPILE(map-input [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value*2,fn(value):value*3]
  values=map(callbacks,fn(callback):callback(8))
  values[0]+values[1]+2
]=])
EXPECT(map-input 42)
COMPILE(map-output [=[fn main()->int:
  callbacks=map([20,21],fn(offset):fn(value:int):value+offset)
  callbacks[0](1)+callbacks[1](0)
]=])
EXPECT(map-output 42)
COMPILE(map-bound [=[fn main()->int:
  maker=fn(offset:int)->fn(int)->int:fn(value:int):value+offset
  callbacks=map([20,21],maker)
  evaluate=fn(callback:fn(int)->int)->int:callback(0)
  values=map(callbacks,evaluate)
  values[0]+values[1]+1
]=])
EXPECT(map-bound 42)
COMPILE(map-named [=[fn make(offset:int)->fn(int)->int:
  fn(value):value+offset
fn evaluate(callback:fn(int)->int)->int:
  callback(0)
fn main()->int:
  callbacks=map([20,21],make)
  values=map(callbacks,evaluate)
  values[0]+values[1]+1
]=])
EXPECT(map-named 42)
COMPILE(fields [=[type Config:
  callbacks:[fn(int)->int]
fn make(offset:int)->Config:
  Config {callbacks:[fn(value):value+offset]}
fn main()->int:
  config=make(20)
  config.callbacks[0](22)
]=])
EXPECT(fields 42)
COMPILE(enum [=[enum Choice:
  Empty
  Some:
    callbacks:[fn(int)->int]
fn main()->int:
  offset:int=20
  choice=Choice.Some {callbacks:[fn(value):value+offset]}
  match choice:
    Choice.Empty:0
    Choice.Some {callbacks}:callbacks[0](22)
]=])
EXPECT(enum 42)
COMPILE(fallible [=[error E:
  Bad:
    value:int
fn make(offset:int)->[fn(int)->int ! E]:
  validate=fn(value:int)->int ! E:
    if value==0:fail E.Bad {value:offset}
    value
  [validate]
fn main()->int:
  callbacks=make(42)
  callbacks[0](0) catch fn(error):
    match error:
      E.Bad {value}:value
]=])
EXPECT(fallible 42)
COMPILE(error-array [=[error E:
  Bad:
    callbacks:[fn(int)->int]
fn raise(offset:int)->int ! E:
  fail E.Bad {callbacks:[fn(value):value+offset]}
fn main()->int:
  raise(20) catch fn(error):
    match error:
      E.Bad {callbacks}:callbacks[0](22)
]=])
EXPECT(error-array 42)
COMPILE(indirect [=[fn evaluate(callbacks:[fn(int)->int])->int:
  callbacks[0](22)
fn make(offset:int)->[fn(int)->int]:
  [fn(value):value+offset]
fn main()->int:
  factory=make
  apply=evaluate
  apply(factory(20))
]=])
EXPECT(indirect 42)
COMPILE(capture-array [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value*2]
  run=fn(value:int):callbacks[0](value)
  run(21)
]=])
EXPECT(capture-array 42)
COMPILE(conditional [=[fn main()->int:
  first:[fn(int)->int]=[fn(value):value*2]
  second:[fn(int)->int]=[fn(value):value,fn(value):value*3]
  callbacks=if true:first
  else:second
  callbacks[0](21)
]=])
EXPECT(conditional 42)
COMPILE(worker-local [=[fn worker(offset:int)->int:
  callbacks:[fn(int)->int]=[fn(value):value+offset]
  callbacks[0](22)
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-local 42)
COMPILE(coordinator [=[fn worker(value:int)->int:
  value
fn main()->int ! TaskError:
  callbacks:[fn(int)->int]=[fn(value):value*2]
  try parallel:
    value=try await spawn worker(21)
    callbacks[0](value)
]=])
EXPECT(coordinator 42)
COMPILE(pipeline [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value*2]
  first=10 -> callbacks[0]
  second=11 -> callbacks[0]()
  first+second
]=])
EXPECT(pipeline 42)
COMPILE(empty-map [=[fn main()->int:
  values:[int]=[]
  maker=fn(offset:int)->fn(int)->int:
    bad=1/offset
    fn(value:int):value+bad
  callbacks=map(values,maker)
  length(callbacks)+42
]=])
EXPECT(empty-map 42)
COMPILE(factory-error [=[error E:
  Bad
fn make(offset:int)->[fn(int)->int] ! E:
  if offset==0:fail E.Bad
  [fn(value):value+offset]
fn main()->int:
  factory=make
  callbacks=factory(0) catch fn(error):[fn(value:int):value*2]
  callbacks[0](21)
]=])
EXPECT(factory-error 42)
COMPILE(worker-recovery [=[error E:
  Bad:
    callbacks:[fn(int)->int]
fn raise(offset:int)->int ! E:
  fail E.Bad {callbacks:[fn(value):value+offset]}
fn worker(offset:int)->int:
  raise(offset) catch fn(error):
    match error:
      E.Bad {callbacks}:callbacks[0](22)
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-recovery 42)
COMPILE(bounds [=[fn worker(index:int)->int:
  callbacks:[fn(int)->int]=[fn(value):1/value]
  callbacks[index](0)
fn attempt()->int ! TaskError:
  try parallel:
    try await spawn worker(1)
fn main()->int:
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
    value:int
fn main()->int:
  offset:int=21
  failer=fn(value:int)->int ! E:fail E.Bad {value:offset}
  callbacks:[fn(int)->int ! E]=[failer,failer]
  evaluate=fn(callback:fn(int)->int ! E)->int:
    callback(0) catch fn(error):
      match error:
        E.Bad {value}:value
  values=map(callbacks,evaluate)
  values[0]+values[1]
]=])
EXPECT(fallible-map 42)
COMPILE(separate-errors [=[error InputError:
  Bad:
    value:int
error BuildError:
  Disabled
fn make(offset:int)->[fn(int)->int ! InputError] ! BuildError:
  if offset==0:fail BuildError.Disabled
  callback=fn(value:int)->int ! InputError:
    if value==0:fail InputError.Bad {value:offset}
    value
  [callback]
fn main()->int:
  factory=make
  callbacks=factory(42) catch fn(error):[fn(value:int)->int ! InputError:value]
  callbacks[0](0) catch fn(error):
    match error:
      InputError.Bad {value}:value
]=])
EXPECT(separate-errors 42)
COMPILE(raw-returned [=[type Row:
  text:String
  values:[int]
fn make(offset:int)->[fn(int)->int]:
  row=Row {text:"é"+"!",values:[10,20]}
  [fn(value):row.values[0]+row.values[1]+length(row.text)+offset+value,fn(value):offset+value]
fn main()->int:
  callbacks=make(10)
  callbacks[0](0)
]=] --test-entry make)
EXPECT(raw-returned 42)
COMPILE(raw-owned [=[fn make()->[fn(int)->int]:
  text="é"+"!"
  values:[int]=[20,20]
  [fn(value):values[0]+values[1]+length(text)+value]
fn probe(input:int)->int:
  callbacks=make()
  callbacks[0](input-40)
fn main()->int:
  probe(40)
]=] --test-entry probe)
EXPECT(raw-owned 42)
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  COMPILE("width-${TYPE}" "fn make(offset:${TYPE})->[fn(${TYPE})->${TYPE}]:\n  [fn(value):value+offset]\nfn main()->${TYPE}:\n  callbacks=make(${TYPE}(20))\n  callbacks[0](${TYPE}(22))\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ClosureRuntimeTests.cpp" "${OUTPUT_DIR}/raw-owned.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/FunctionArrayRuntimeTests.cpp" "${OUTPUT_DIR}/raw-returned.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-returned-harness")
RUN_OK("${OUTPUT_DIR}/raw-returned-harness")
file(WRITE "${OUTPUT_DIR}/reject-missing-parameter.k" [=[fn main()->int:
  callbacks=[fn(value):value]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-missing-parameter.k" -o "${OUTPUT_DIR}/reject-missing-parameter" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: missing-parameter: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-empty-context.k" [=[fn main()->int:
  callbacks=[]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-empty-context.k" -o "${OUTPUT_DIR}/reject-empty-context" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: empty-context: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-mixed-parameters.k" [=[fn main()->int:
  callbacks=[fn(value:int):value,fn(value:float):value]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-mixed-parameters.k" -o "${OUTPUT_DIR}/reject-mixed-parameters" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: mixed-parameters: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-mixed-results.k" [=[fn main()->int:
  callbacks=[fn(value:int):value,fn(value:int)->float:float(value)]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-mixed-results.k" -o "${OUTPUT_DIR}/reject-mixed-results" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: mixed-results: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-context.k" [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value:float):value]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-context.k" -o "${OUTPUT_DIR}/reject-wrong-context" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: wrong-context: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-mixed-errors.k" [=[error A:
  Bad
error B:
  Bad
fn main()->int:
  first=fn(value:int)->int ! A:value
  second=fn(value:int)->int ! B:value
  callbacks=[first,second]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-mixed-errors.k" -o "${OUTPUT_DIR}/reject-mixed-errors" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: mixed-errors: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-call-array.k" [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value]
  callbacks(42)
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-call-array.k" -o "${OUTPUT_DIR}/reject-call-array" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: call-array: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-call.k" [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value]
  callbacks[0]("bad")
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-call.k" -o "${OUTPUT_DIR}/reject-wrong-call" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: wrong-call: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-concat.k" [=[fn main()->int:
  first=[fn(value:int):value]
  second=[fn(value:float):value]
  callbacks=concat(first,second)
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-concat.k" -o "${OUTPUT_DIR}/reject-wrong-concat" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: wrong-concat: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-constant-bounds.k" [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value]
  callbacks[1](42)
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-constant-bounds.k" -o "${OUTPUT_DIR}/reject-constant-bounds" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: constant-bounds: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-unhandled.k" [=[error E:
  Bad
fn main()->int:
  callback=fn(value:int)->int ! E:value
  callbacks=[callback]
  callbacks[0](42)+1
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-unhandled.k" -o "${OUTPUT_DIR}/reject-unhandled" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: unhandled: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-fallible-map-aggregation.k" [=[error E:
  Bad
fn main()->int:
  callback=fn(value:int)->int ! E:value
  callbacks=[callback]
  evaluate=fn(fnValue:fn(int)->int ! E)->int ! E:try fnValue(0)
  values=map(callbacks,evaluate)
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-fallible-map-aggregation.k" -o "${OUTPUT_DIR}/reject-fallible-map-aggregation" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: fallible-map-aggregation: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(supported-worker-argument [=[fn worker(callbacks:[fn(int)->int])->int:
  callbacks[0](42)
fn main()->int ! TaskError:
  callbacks:[fn(int)->int]=[fn(value):value]
  try parallel:
    try await spawn worker(callbacks)
]=])
EXPECT(supported-worker-argument 42)
COMPILE(supported-worker-result [=[fn worker(offset:int)->[fn(int)->int]:
  [fn(value):value+offset]
fn main()->int ! TaskError:
  try parallel:
    callbacks=try await spawn worker(20)
    callbacks[0](22)
]=])
EXPECT(supported-worker-result 42)
COMPILE(supported-worker-nested-field [=[type Config:
  callbacks:[fn(int)->int]
fn worker(config:Config)->int:
  config.callbacks[0](42)
fn main()->int ! TaskError:
  config=Config {callbacks:[fn(value):value]}
  try parallel:
    try await spawn worker(config)
]=])
EXPECT(supported-worker-nested-field 42)
COMPILE(supported-worker-error-payload [=[error E:
  Bad:
    callbacks:[fn(int)->int]
fn worker(value:int)->int ! E:
  value
fn main()->int ! (E | TaskError):
  try parallel:
    try await spawn worker(42)
]=])
EXPECT(supported-worker-error-payload 42)
COMPILE(supported-worker-map-input [=[fn main()->int ! TaskError:
  callbacks:[fn(int)->int]=[fn(value):value]
  try parallel:
    values=try await spawn map(callbacks,fn(callback):callback(42))
    values[0]
]=])
EXPECT(supported-worker-map-input 42)
COMPILE(supported-worker-map-result [=[fn main()->int ! TaskError:
  try parallel:
    values=try await spawn map([20],fn(offset):fn(value:int):offset+value)
    length(values)
]=])
EXPECT(supported-worker-map-result 1)
COMPILE(supported-worker-query [=[fn main()->int ! TaskError:
  callbacks:[fn(int)->int]=[fn(value):value]
  try parallel:
    try await spawn length(callbacks)
]=])
EXPECT(supported-worker-query 1)
COMPILE(supported-worker-slice [=[fn main()->int ! TaskError:
  callbacks:[fn(int)->int]=[fn(value):value]
  try parallel:
    values=try await spawn slice(callbacks,0,1)
    length(values)
]=])
EXPECT(supported-worker-slice 1)
COMPILE(supported-coordination [=[fn worker(value:int)->int:
  value
fn coordinate(value:int)->int ! TaskError:
  try parallel:
    try await spawn worker(value)
fn main()->int:
  callbacks=[coordinate]
  0
]=])
EXPECT(supported-coordination 0)
file(WRITE "${OUTPUT_DIR}/reject-nested-array-type.k" [=[fn main()->int:
  callbacks:[[fn(int)->int]]=[]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-nested-array-type.k" -o "${OUTPUT_DIR}/reject-nested-array-type" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: nested-array-type: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-function-tensor.k" [=[fn main()->int:
  callbacks:[fn(int)->int]=[fn(value):value]
  value=tensor(callbacks)
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-function-tensor.k" -o "${OUTPUT_DIR}/reject-function-tensor" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid function array accepted/crashed: function-tensor: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-nominal.k" [=[type A:
  value:int
type B:
  value:int
fn main()->int:
  callbacks:[fn(A)->int]=[fn(value:B):value.value]
  0
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
file(WRITE "${OUTPUT_DIR}/helper.k" "export fn make(offset:int)->[fn(int)->int]:\n  [fn(value):value+offset]\n")
COMPILE(imported "from \"./helper.k\" import make\nfn main()->int:\n  callbacks=make(20)\n  callbacks[0](22)\n")
EXPECT(imported 42)
foreach(ARTIFACT IN ITEMS rejected rejected.ll rejected.mlir)
  file(WRITE "${OUTPUT_DIR}/${ARTIFACT}" "preserved")
endforeach()
file(WRITE "${OUTPUT_DIR}/rejected.k" "fn main()->int:\n  callbacks:[fn(int)->int]=[fn(value:float):value]\n  0\n")
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
