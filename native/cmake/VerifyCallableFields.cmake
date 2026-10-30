include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
COMPILE(named [=[type Config:
  apply: fn(int)->int
fn double(value:int)->int:
  value*2
fn main()->int:
  config=Config {apply:double}
  config.apply(21)
]=])
EXPECT(named 42)
file(READ "${SOURCE_DIR}/../docs/design/fixtures/callable-fields.k" GUIDE)
COMPILE(captured "${GUIDE}")
EXPECT(captured 42)
COMPILE(enum [=[enum Choice:
  Empty
  Some:
    callback:fn(int)->int
fn make(offset:int)->Choice:
  Choice.Some {callback:fn(value):offset+value}
fn main()->int:
  choice=make(20)
  match choice:
    Choice.Empty:0
    Choice.Some {callback}:callback(22)
]=])
EXPECT(enum 42)
COMPILE(owned [=[type Config:
  apply: fn(int)->int
fn make()->Config:
  text="é"+"!"
  values:[int]=[20,20]
  Config {apply:fn(value):values[0]+values[1]+length(text)+value}
fn main()->int:
  config=make()
  config.apply(0)
]=])
EXPECT(owned 42)
COMPILE(nested [=[type Config:
  apply:fn(int)->int
  label:String
type Outer:
  config:Config
fn make(offset:int)->Outer:
  config=Config {apply:fn(value):offset+value,label:"é"}
  Outer {config:config}
fn main()->int:
  outer=make(20)
  f=fn(value:int):outer.config.apply(value)
  f(22)
]=])
EXPECT(nested 42)
COMPILE(updates [=[type Config:
  apply:fn(int)->int
fn main()->int:
  offset:int=20
  first=Config {apply:fn(value):offset+value}
  second=first with {apply:fn(value):value*2}
  first.apply(1)+second.apply(10)+1
]=])
EXPECT(updates 42)
COMPILE(fallible [=[error E:
  Bad:
    value:int
type Config:
  apply:fn(int)->int ! E
fn make(offset:int)->Config:
  callback=fn(value:int)->int ! E:
    if value==0:fail E.Bad {value:offset}
    value
  Config {apply:callback}
fn main()->int:
  config=make(42)
  config.apply(0) catch fn(error):
    match error:
      E.Bad {value}:value
]=])
EXPECT(fallible 42)
COMPILE(error-payload [=[type Config:
  apply:fn(int)->int
error E:
  Bad:
    config:Config
fn raise(offset:int)->int ! E:
  fail E.Bad {config:Config {apply:fn(value):offset+value}}
fn main()->int:
  raise(20) catch fn(error):
    match error:
      E.Bad {config}:config.apply(22)
]=])
EXPECT(error-payload 42)
COMPILE(array [=[type Config:
  apply:fn(int)->int
fn make(offset:int)->[Config]:
  [Config {apply:fn(value):offset+value}]
fn main()->int:
  configs=concat(make(20),make(21))
  selected=slice(configs,0,1)
  mapped=map(selected,fn(config):config.apply(22))
  mapped[0]
]=])
EXPECT(array 42)
COMPILE(worker-local [=[type Config:
  apply:fn(int)->int
fn worker(offset:int)->int:
  config=Config {apply:fn(value):offset+value}
  config.apply(22)
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-local 42)
COMPILE(coordinator [=[type Config:
  apply:fn(int)->int
fn worker(value:int)->int:
  value
fn main()->int ! TaskError:
  config=Config {apply:fn(value):value*2}
  try parallel:
    value=try await spawn worker(21)
    config.apply(value)
]=])
EXPECT(coordinator 42)
COMPILE(array-callback-result [=[type Config:
  apply:fn(int)->int
fn main()->int:
  maker=fn(offset:int)->Config:
    Config {apply:fn(value):value+offset}
  configs=map([20],maker)
  configs[0].apply(22)
]=])
EXPECT(array-callback-result 42)
COMPILE(higher [=[type Config:
  make:fn(int)->fn(int)->int
fn main()->int:
  config=Config {make:fn(offset):fn(value:int):value+offset}
  config.make(20)(22)
]=])
EXPECT(higher 42)
COMPILE(matrix [=[type Config:
  apply:fn(Tensor[int,2,2])->Tensor[int,2,2]
fn main()->int:
  offset:int=1
  config=Config {apply:fn(values):values+offset}
  reduceSum(config.apply(tensor([[10,10],[10,8]])))
]=])
EXPECT(matrix 42)
COMPILE(pipeline [=[type Config:
  apply:fn(int)->int
fn main()->int:
  config=Config {apply:fn(value):value*2}
  first=10 -> config.apply()
  second=11 -> config.apply
  first+second
]=])
EXPECT(pipeline 42)
COMPILE(empty-enum [=[enum Choice:
  Empty
  Some:
    callback:fn(int)->int
fn main()->int:
  choice=Choice.Empty
  match choice:
    Choice.Empty:42
    Choice.Some {callback}:callback(21)
]=])
EXPECT(empty-enum 42)
COMPILE(enum-signatures [=[enum Choice:
  Number:
    callback:fn(int)->int
  Text:
    callback:fn(String)->int
fn select(flag:Bool)->Choice:
  if flag:Choice.Number {callback:fn(value):value*2}
  else:Choice.Text {callback:fn(text):length(text)+40}
fn main()->int:
  match select(false):
    Choice.Number {callback}:callback(21)
    Choice.Text {callback}:callback("é!")
]=])
EXPECT(enum-signatures 42)
COMPILE(error-union [=[type Config:
  apply:fn(int)->int
error A:
  Bad:
    config:Config
error B:
  Missing
type Both=B | A | B
fn raise(offset:int)->int ! Both:
  fail A.Bad {config:Config {apply:fn(value):offset+value}}
fn main()->int:
  raise(20) catch fn(error):
    match error:
      A.Bad {config}:config.apply(22)
      B.Missing:0
]=])
EXPECT(error-union 42)
COMPILE(factory-field [=[error E:
  Bad:
    value:int
error BuildError:
  Disabled
type Config:
  make:fn(Bool)->(fn(int)->int ! E) ! BuildError
fn main()->int:
  offset:int=42
  factory=fn(blocked:Bool)->(fn(int)->int ! E) ! BuildError:
    if blocked:fail BuildError.Disabled
    fn(value:int)->int ! E:
      if value==0:fail E.Bad {value:offset}
      value
  config=Config {make:factory}
  f=config.make(false) catch fn(error):fn(value:int)->int ! E:value
  f(0) catch fn(error):
    match error:
      E.Bad {value}:value
]=])
EXPECT(factory-field 42)
COMPILE(mixed-fields [=[type Config:
  label:String
  enabled:Bool
  apply:fn(int)->int
  values:[int]
  second:fn(Bool)->int
fn main()->int:
  offset:int=20
  second=fn(flag:Bool)->int:
    if flag:1
    else:0
  config=Config {label:"é!",enabled:true,apply:fn(value):offset+value,values:[19],second:second}
  config.apply(config.values[0])+config.second(config.enabled)+length(config.label)
]=])
EXPECT(mixed-fields 42)
COMPILE(worker-recovery [=[type Config:
  apply:fn(int)->int
error E:
  Bad:
    config:Config
fn raise(offset:int)->int ! E:
  fail E.Bad {config:Config {apply:fn(value):offset+value}}
fn worker(offset:int)->int:
  raise(offset) catch fn(error):
    match error:
      E.Bad {config}:config.apply(22)
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-recovery 42)
COMPILE(recovery-result [=[type Config:
  apply:fn(int)->int
error E:
  Bad
fn make()->Config ! E:
  fail E.Bad
fn main()->int:
  offset:int=20
  config=make() catch fn(error):Config {apply:fn(value):value+offset}
  config.apply(22)
]=])
EXPECT(recovery-result 42)
COMPILE(selected-field [=[type Config:
  apply:fn(int)->int
fn main()->int:
  offset:int=20
  first=Config {apply:fn(value):value+offset}
  second=Config {apply:fn(value):value*2}
  config=if length("é")==1:first
  else:second
  alias=config.apply
  alias(22)
]=])
EXPECT(selected-field 42)
COMPILE(string-result [=[type Config:
  apply:fn(String)->String
fn make(prefix:String)->Config:
  Config {apply:fn(value):prefix+value}
fn main()->int:
  config=make("é"+"!")
  length(config.apply("🙂"))+39
]=])
EXPECT(string-result 42)
COMPILE(direct-error-field [=[error A:
  Bad:
    callback:fn(int)->int
error B:
  Missing
type Both=B | A
fn raise(offset:int)->int ! Both:
  fail A.Bad {callback:fn(value):offset+value}
fn main()->int:
  raise(20) catch fn(error):
    match error:
      A.Bad {callback}:callback(22)
      B.Missing:0
]=])
EXPECT(direct-error-field 42)
COMPILE(raw-owned [=[type Config:
  apply:fn(int)->int
fn make()->Config:
  text="é"+"!"
  values:[int]=[20,20]
  Config {apply:fn(value):values[0]+values[1]+length(text)+value}
fn probe(input:int)->int:
  config=make()
  config.apply(input-40)
fn main()->int:
  probe(40)
]=] --test-entry probe)
EXPECT(raw-owned 42)
COMPILE(returned-probe [=[type Row:
  text:String
  values:[int]
type Config:
  apply:fn(int)->int
fn make(offset:int)->Config:
  row=Row {text:"é"+"!",values:[10,20]}
  Config {apply:fn(value):row.values[0]+row.values[1]+length(row.text)+offset+value}
fn main()->int:
  config=make(10)
  config.apply(0)
]=] --test-entry make)
EXPECT(returned-probe 42)
COMPILE(raw-fault [=[fn make(base:int)->Config:
  callback=fn(value:int):
    adjusted=base+1
    adjusted/value
  Config {apply:callback}
type Config:
  apply:fn(int)->int
fn probe(input:int)->int:
  config=make(41)
  config.apply(input)
fn main()->int:
  probe(1)
]=] --test-entry probe)
EXPECT(raw-fault 42)
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  COMPILE("width-${TYPE}" "type Config:\n  apply:fn(${TYPE})->${TYPE}\nfn make(offset:${TYPE})->Config:\n  Config {apply:fn(value):value+offset}\nfn main()->${TYPE}:\n  config=make(${TYPE}(20))\n  config.apply(${TYPE}(22))\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ClosureRuntimeTests.cpp" "${OUTPUT_DIR}/raw-owned.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ReturnedClosureRuntimeTests.cpp" "${OUTPUT_DIR}/returned-probe.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/returned-probe-harness")
RUN_OK("${OUTPUT_DIR}/returned-probe-harness")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/BlockClosureFaultRuntimeTests.cpp" "${OUTPUT_DIR}/raw-fault.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-fault-harness")
RUN_OK("${OUTPUT_DIR}/raw-fault-harness")
file(WRITE "${OUTPUT_DIR}/reject-wrong-parameter.k" [=[type Config:
  apply:fn(int)->int
fn main()->int:
  config=Config {apply:fn(value:float)->float:value}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-parameter.k" -o "${OUTPUT_DIR}/reject-wrong-parameter" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-parameter: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-result.k" [=[type Config:
  apply:fn(int)->int
fn main()->int:
  config=Config {apply:fn(value:int)->float:float(value)}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-result.k" -o "${OUTPUT_DIR}/reject-wrong-result" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-result: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-channel.k" [=[error E:
  Bad
type Config:
  apply:fn(int)->int
fn main()->int:
  config=Config {apply:fn(value:int)->int ! E:value}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-channel.k" -o "${OUTPUT_DIR}/reject-wrong-channel" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-channel: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-update.k" [=[type Config:
  apply:fn(int)->int
fn main()->int:
  config=Config {apply:fn(value):value}
  updated=config with {apply:fn(value:float):value}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-update.k" -o "${OUTPUT_DIR}/reject-wrong-update" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-update: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-missing-field.k" [=[type Config:
  apply:fn(int)->int
fn main()->int:
  config=Config {}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-missing-field.k" -o "${OUTPUT_DIR}/reject-missing-field" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: missing-field: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-unhandled.k" [=[error E:
  Bad
type Config:
  apply:fn(int)->int ! E
fn main()->int:
  config=Config {apply:fn(value:int)->int ! E:value}
  config.apply(42)+1
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-unhandled.k" -o "${OUTPUT_DIR}/reject-unhandled" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: unhandled: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-enum-outside-match.k" [=[enum Choice:
  Some:
    callback:fn(int)->int
fn main()->int:
  choice=Choice.Some {callback:fn(value):value}
  choice.callback(42)
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-enum-outside-match.k" -o "${OUTPUT_DIR}/reject-enum-outside-match" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: enum-outside-match: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-enum-wrong-payload.k" [=[enum Choice:
  Some:
    callback:fn(int)->int
fn main()->int:
  choice=Choice.Some {callback:0}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-enum-wrong-payload.k" -o "${OUTPUT_DIR}/reject-enum-wrong-payload" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: enum-wrong-payload: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-shape.k" [=[type Config:
  apply:fn(Tensor[int,2])->int
fn sum(values:Tensor[int,3])->int:
  reduceSum(values)
fn main()->int:
  config=Config {apply:sum}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-shape.k" -o "${OUTPUT_DIR}/reject-wrong-shape" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-shape: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-nominal.k" [=[type A:
  value:int
type B:
  value:int
type Config:
  apply:fn(A)->int
fn accept(value:B)->int:
  value.value
fn main()->int:
  config=Config {apply:accept}
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-nominal.k" -o "${OUTPUT_DIR}/reject-wrong-nominal" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-nominal: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(supported-coordination [=[type Config:
  apply:fn(int)->int ! TaskError
fn worker(value:int)->int:
  value
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn worker(value)
fn main()->int:
  config=Config {apply:run}
  0
]=])
EXPECT(supported-coordination 0)
COMPILE(supported-recursive-signature [=[type Config:
  value:int
  apply:fn(Config)->int
fn read(config:Config)->int:
  config.value
fn main()->int:
  config=Config {value:42,apply:read}
  config.apply(config)
]=])
EXPECT(supported-recursive-signature 42)
file(WRITE "${OUTPUT_DIR}/reject-direct-functions-array.k" [=[fn main()->int:
  values=[fn(value:int):value,fn(value:float):value]
  0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-direct-functions-array.k" -o "${OUTPUT_DIR}/reject-direct-functions-array" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: direct-functions-array: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(supported-worker-argument [=[type Config:
  apply:fn(int)->int
fn worker(config:Config)->int:
  config.apply(42)
fn main()->int ! TaskError:
  config=Config {apply:fn(value):value}
  try parallel:
    try await spawn worker(config)
]=])
EXPECT(supported-worker-argument 42)
COMPILE(supported-worker-nested-array [=[type Config:
  apply:fn(int)->int
type Outer:
  values:[Config]
fn worker(outer:Outer)->int:
  outer.values[0].apply(42)
fn main()->int ! TaskError:
  outer=Outer {values:[Config {apply:fn(value):value}]}
  try parallel:
    try await spawn worker(outer)
]=])
EXPECT(supported-worker-nested-array 42)
COMPILE(supported-worker-result [=[type Config:
  apply:fn(int)->int
fn worker(value:int)->Config:
  Config {apply:fn(input):input+value}
fn main()->int ! TaskError:
  try parallel:
    config=try await spawn worker(20)
    config.apply(22)
]=])
EXPECT(supported-worker-result 42)
COMPILE(supported-worker-error-payload [=[type Config:
  apply:fn(int)->int
error E:
  Bad:
    config:Config
fn worker(value:int)->int ! E:
  value
fn main()->int ! (E | TaskError):
  try parallel:
    try await spawn worker(42)
]=])
EXPECT(supported-worker-error-payload 42)
COMPILE(supported-worker-map-capture [=[type Config:
  apply:fn(int)->int
fn main()->int ! TaskError:
  config=Config {apply:fn(value):value*2}
  try parallel:
    values=try await spawn map([21],fn(value):config.apply(value))
    values[0]
]=])
EXPECT(supported-worker-map-capture 42)
COMPILE(supported-worker-map-input [=[type Config:
  apply:fn(int)->int
fn main()->int ! TaskError:
  configs=[Config {apply:fn(value):value*2}]
  try parallel:
    values=try await spawn map(configs,fn(config):config.apply(21))
    values[0]
]=])
EXPECT(supported-worker-map-input 42)
COMPILE(supported-worker-query [=[type Config:
  apply:fn(int)->int
fn main()->int ! TaskError:
  configs=[Config {apply:fn(value):value}]
  try parallel:
    try await spawn length(configs)
]=])
EXPECT(supported-worker-query 1)
COMPILE(supported-worker-slice [=[type Config:
  apply:fn(int)->int
fn main()->int ! TaskError:
  configs=[Config {apply:fn(value):value}]
  try parallel:
    values=try await spawn slice(configs,0,1)
    length(values)
]=])
EXPECT(supported-worker-slice 1)
COMPILE(supported-worker-concat [=[type Config:
  apply:fn(int)->int
fn main()->int ! TaskError:
  configs=[Config {apply:fn(value):value}]
  try parallel:
    values=try await spawn concat(configs,configs)
    length(values)
]=])
EXPECT(supported-worker-concat 2)
foreach(NAME IN ITEMS owned nested array error-union factory-field array-callback-result)
  foreach(ATTEMPT RANGE 1 40)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${ATTEMPT}" KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/${NAME}" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
    if(NOT "${STATUS}" MATCHES "^(0|86|88)$" OR ERR MATCHES "AddressSanitizer|runtime error:|ThreadSanitizer")
      message(FATAL_ERROR "Callable field allocation probe failed: ${NAME} ${ATTEMPT}: ${STATUS}: ${OUT}${ERR}")
    endif()
    if("${STATUS}" STREQUAL "0")
      if(NOT OUT STREQUAL "42\n")
        message(FATAL_ERROR "Callable field output changed: ${NAME}: ${OUT}${ERR}")
      endif()
    elseif(NOT OUT STREQUAL "" OR NOT ERR MATCHES "cleanup: live=0 allocations=([0-9]+) releases=([0-9]+)" OR NOT CMAKE_MATCH_1 STREQUAL CMAKE_MATCH_2)
      message(FATAL_ERROR "Callable field failure leaked or published data: ${NAME}: ${OUT}${ERR}")
    endif()
  endforeach()
endforeach()
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(NAME IN ITEMS worker-local worker-recovery coordinator)
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_WORKERS=${WORKERS}" "${OUTPUT_DIR}/${NAME}")
    if(NOT OUTPUT STREQUAL "42\n")
      message(FATAL_ERROR "Worker-local callable field changed: ${NAME}: ${OUTPUT}")
    endif()
  endforeach()
endforeach()
file(WRITE "${OUTPUT_DIR}/helper.k" "export type Config:\n  apply:fn(int)->int\nexport fn make(offset:int)->Config:\n  Config {apply:fn(value):value+offset}\n")
COMPILE(imported "from \"./helper.k\" import Config as RemoteConfig, make\ntype Config:\n  value:int\nfn main()->int:\n  config:RemoteConfig=make(20)\n  config.apply(22)\n")
EXPECT(imported 42)
foreach(ARTIFACT IN ITEMS rejected rejected.ll rejected.mlir)
  file(WRITE "${OUTPUT_DIR}/${ARTIFACT}" "preserved")
endforeach()
file(WRITE "${OUTPUT_DIR}/rejected.k" "type Config:\n  apply:fn(int)->int\nfn main()->int:\n  config=Config {apply:0}\n  0\n")
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/rejected.k" -o "${OUTPUT_DIR}/rejected" --emit-llvm "${OUTPUT_DIR}/rejected.ll" --emit-mlir "${OUTPUT_DIR}/rejected.mlir" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1" OR NOT ERR MATCHES "S013")
  message(FATAL_ERROR "Invalid field signature diagnostic changed: ${OUT}${ERR}")
endif()
foreach(ARTIFACT IN ITEMS rejected rejected.ll rejected.mlir)
  file(READ "${OUTPUT_DIR}/${ARTIFACT}" CONTENT)
  if(NOT CONTENT STREQUAL "preserved")
    message(FATAL_ERROR "Rejected callable field replaced output artifacts")
  endif()
endforeach()
message(STATUS "Callable fields: records, variants, captured ownership, exact signatures and worker boundaries passed")
