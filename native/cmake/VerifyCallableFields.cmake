include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
COMPILE(named [=[type Config:
  apply: fn(i64)->i64
fn double(value:i64)->i64:
  return value*2
fn main()->i64:
  config=Config {apply:double}
  return config.apply(21)
]=])
EXPECT(named 42)
file(READ "${SOURCE_DIR}/../docs/design/fixtures/callable-fields.k" GUIDE)
COMPILE(captured "${GUIDE}")
EXPECT(captured 42)
COMPILE(enum [=[enum Choice:
  Empty
  Some:
    callback:fn(i64)->i64
fn make(offset:i64)->Choice:
  return Choice.Some {callback:fn(value):return offset+value}
fn main()->i64:
  choice=make(20)
  return match choice:
    Choice.Empty:0
    Choice.Some {callback}:callback(22)
]=])
EXPECT(enum 42)
COMPILE(owned [=[type Config:
  apply: fn(i64)->i64
fn make()->Config:
  text="é"+"!"
  values:[i64]=[20,20]
  return Config {apply:fn(value):return values[0]+values[1]+length(text)+value}
fn main()->i64:
  config=make()
  return config.apply(0)
]=])
EXPECT(owned 42)
COMPILE(nested [=[type Config:
  apply:fn(i64)->i64
  label:String
type Outer:
  config:Config
fn make(offset:i64)->Outer:
  config=Config {apply:fn(value):return offset+value,label:"é"}
  return Outer {config:config}
fn main()->i64:
  outer=make(20)
  f=fn(value:i64):return outer.config.apply(value)
  return f(22)
]=])
EXPECT(nested 42)
COMPILE(updates [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  offset:i64=20
  first=Config {apply:fn(value):return offset+value}
  second=first with {apply:fn(value):return value*2}
  return first.apply(1)+second.apply(10)+1
]=])
EXPECT(updates 42)
COMPILE(fallible [=[error E:
  Bad:
    value:i64
type Config:
  apply:fn(i64)->i64 ! E
fn make(offset:i64)->Config:
  callback=fn(value:i64)->i64 ! E:
    if value==0:fail E.Bad {value:offset}
    return value
  return Config {apply:callback}
fn main()->i64:
  config=make(42)
  return config.apply(0) catch fn(error):
    return match error:
      E.Bad {value}:value
]=])
EXPECT(fallible 42)
COMPILE(error-payload [=[type Config:
  apply:fn(i64)->i64
error E:
  Bad:
    config:Config
fn raise(offset:i64)->i64 ! E:
  fail E.Bad {config:Config {apply:fn(value):return offset+value}}
fn main()->i64:
  return raise(20) catch fn(error):
    return match error:
      E.Bad {config}:config.apply(22)
]=])
EXPECT(error-payload 42)
COMPILE(array [=[type Config:
  apply:fn(i64)->i64
fn make(offset:i64)->[Config]:
  return [Config {apply:fn(value):return offset+value}]
fn main()->i64:
  configs=concat(make(20),make(21))
  selected=slice(configs,0,1)
  mapped=map(selected,fn(config):return config.apply(22))
  return mapped[0]
]=])
EXPECT(array 42)
COMPILE(worker-local [=[type Config:
  apply:fn(i64)->i64
fn worker(offset:i64)->i64:
  config=Config {apply:fn(value):return offset+value}
  return config.apply(22)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-local 42)
COMPILE(coordinator [=[type Config:
  apply:fn(i64)->i64
fn worker(value:i64)->i64:
  return value
fn main()->i64 ! TaskError:
  config=Config {apply:fn(value):return value*2}
  return try parallel:
    value=try await spawn worker(21)
    config.apply(value)
]=])
EXPECT(coordinator 42)
COMPILE(array-callback-result [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  maker=fn(offset:i64)->Config:
    return Config {apply:fn(value):return value+offset}
  configs=map([20],maker)
  return configs[0].apply(22)
]=])
EXPECT(array-callback-result 42)
COMPILE(higher [=[type Config:
  make:fn(i64)->fn(i64)->i64
fn main()->i64:
  config=Config {make:fn(offset):return fn(value:i64):return value+offset}
  return config.make(20)(22)
]=])
EXPECT(higher 42)
COMPILE(matrix [=[type Config:
  apply:fn(Tensor[i64,2,2])->Tensor[i64,2,2]
fn main()->i64:
  offset:i64=1
  config=Config {apply:fn(values):return values+offset}
  return reduceSum(config.apply(tensor([[10,10],[10,8]])))
]=])
EXPECT(matrix 42)
COMPILE(pipeline [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  config=Config {apply:fn(value):return value*2}
  first=10 -> config.apply()
  second=11 -> config.apply
  return first+second
]=])
EXPECT(pipeline 42)
COMPILE(empty-enum [=[enum Choice:
  Empty
  Some:
    callback:fn(i64)->i64
fn main()->i64:
  choice=Choice.Empty
  return match choice:
    Choice.Empty:42
    Choice.Some {callback}:callback(21)
]=])
EXPECT(empty-enum 42)
COMPILE(enum-signatures [=[enum Choice:
  Number:
    callback:fn(i64)->i64
  Text:
    callback:fn(String)->i64
fn select(flag:bool)->Choice:
  return if flag:Choice.Number {callback:fn(value):return value*2}
  else:Choice.Text {callback:fn(text):return length(text)+40}
fn main()->i64:
  return match select(false):
    Choice.Number {callback}:callback(21)
    Choice.Text {callback}:callback("é!")
]=])
EXPECT(enum-signatures 42)
COMPILE(error-union [=[type Config:
  apply:fn(i64)->i64
error A:
  Bad:
    config:Config
error B:
  Missing
type Both=B | A | B
fn raise(offset:i64)->i64 ! Both:
  fail A.Bad {config:Config {apply:fn(value):return offset+value}}
fn main()->i64:
  return raise(20) catch fn(error):
    return match error:
      A.Bad {config}:config.apply(22)
      B.Missing:0
]=])
EXPECT(error-union 42)
COMPILE(factory-field [=[error E:
  Bad:
    value:i64
error BuildError:
  Disabled
type Config:
  make:fn(bool)->(fn(i64)->i64 ! E) ! BuildError
fn main()->i64:
  offset:i64=42
  factory=fn(blocked:bool)->(fn(i64)->i64 ! E) ! BuildError:
    if blocked:fail BuildError.Disabled
    return fn(value:i64)->i64 ! E:
      if value==0:fail E.Bad {value:offset}
      return value
  config=Config {make:factory}
  f=config.make(false) catch fn(error):return fn(value:i64)->i64 ! E:return value
  return f(0) catch fn(error):
    return match error:
      E.Bad {value}:value
]=])
EXPECT(factory-field 42)
COMPILE(mixed-fields [=[type Config:
  label:String
  enabled:bool
  apply:fn(i64)->i64
  values:[i64]
  second:fn(bool)->i64
fn main()->i64:
  offset:i64=20
  second=fn(flag:bool)->i64:
    return if flag:1
    else:0
  config=Config {label:"é!",enabled:true,apply:fn(value):return offset+value,values:[19],second:second}
  return config.apply(config.values[0])+config.second(config.enabled)+length(config.label)
]=])
EXPECT(mixed-fields 42)
COMPILE(worker-recovery [=[type Config:
  apply:fn(i64)->i64
error E:
  Bad:
    config:Config
fn raise(offset:i64)->i64 ! E:
  fail E.Bad {config:Config {apply:fn(value):return offset+value}}
fn worker(offset:i64)->i64:
  return raise(offset) catch fn(error):
    return match error:
      E.Bad {config}:config.apply(22)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn worker(20)
]=])
EXPECT(worker-recovery 42)
COMPILE(recovery-result [=[type Config:
  apply:fn(i64)->i64
error E:
  Bad
fn make()->Config ! E:
  fail E.Bad
fn main()->i64:
  offset:i64=20
  config=make() catch fn(error):return Config {apply:fn(value):return value+offset}
  return config.apply(22)
]=])
EXPECT(recovery-result 42)
COMPILE(selected-field [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  offset:i64=20
  first=Config {apply:fn(value):return value+offset}
  second=Config {apply:fn(value):return value*2}
  config=if length("é")==1:first
  else:second
  alias=config.apply
  return alias(22)
]=])
EXPECT(selected-field 42)
COMPILE(string-result [=[type Config:
  apply:fn(String)->String
fn make(prefix:String)->Config:
  return Config {apply:fn(value):return prefix+value}
fn main()->i64:
  config=make("é"+"!")
  return length(config.apply("🙂"))+39
]=])
EXPECT(string-result 42)
COMPILE(direct-error-field [=[error A:
  Bad:
    callback:fn(i64)->i64
error B:
  Missing
type Both=B | A
fn raise(offset:i64)->i64 ! Both:
  fail A.Bad {callback:fn(value):return offset+value}
fn main()->i64:
  return raise(20) catch fn(error):
    return match error:
      A.Bad {callback}:callback(22)
      B.Missing:0
]=])
EXPECT(direct-error-field 42)
COMPILE(raw-owned [=[type Config:
  apply:fn(i64)->i64
fn make()->Config:
  text="é"+"!"
  values:[i64]=[20,20]
  return Config {apply:fn(value):return values[0]+values[1]+length(text)+value}
fn probe(input:i64)->i64:
  config=make()
  return config.apply(input-40)
fn main()->i64:
  return probe(40)
]=] --test-entry probe)
EXPECT(raw-owned 42)
COMPILE(returned-probe [=[type Row:
  text:String
  values:[i64]
type Config:
  apply:fn(i64)->i64
fn make(offset:i64)->Config:
  row=Row {text:"é"+"!",values:[10,20]}
  return Config {apply:fn(value):return row.values[0]+row.values[1]+length(row.text)+offset+value}
fn main()->i64:
  config=make(10)
  return config.apply(0)
]=] --test-entry make)
EXPECT(returned-probe 42)
COMPILE(raw-fault [=[fn make(base:i64)->Config:
  callback=fn(value:i64):
    adjusted=base+1
    return adjusted/value
  return Config {apply:callback}
type Config:
  apply:fn(i64)->i64
fn probe(input:i64)->i64:
  config=make(41)
  return config.apply(input)
fn main()->i64:
  return probe(1)
]=] --test-entry probe)
EXPECT(raw-fault 42)
foreach(TYPE IN ITEMS i8 u8 i16 u16 i32 u32 i64 u64 f32 f64)
  COMPILE("width-${TYPE}" "type Config:\n  apply:fn(${TYPE})->${TYPE}\nfn make(offset:${TYPE})->Config:\n  return Config {apply:fn(value):return value+offset}\nfn main()->${TYPE}:\n  config=make(${TYPE}(20))\n  return config.apply(${TYPE}(22))\n")
  EXPECT("width-${TYPE}" 42)
endforeach()
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ClosureRuntimeTests.cpp" "${OUTPUT_DIR}/raw-owned.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${OUTPUT_DIR}/raw-owned-harness")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/ReturnedClosureRuntimeTests.cpp" "${OUTPUT_DIR}/returned-probe.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/returned-probe-harness")
RUN_OK("${OUTPUT_DIR}/returned-probe-harness")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "${SOURCE_DIR}/tests/BlockClosureFaultRuntimeTests.cpp" "${OUTPUT_DIR}/raw-fault.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/raw-fault-harness")
RUN_OK("${OUTPUT_DIR}/raw-fault-harness")
file(WRITE "${OUTPUT_DIR}/reject-wrong-parameter.k" [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  config=Config {apply:fn(value:f64)->f64:return value}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-parameter.k" -o "${OUTPUT_DIR}/reject-wrong-parameter" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-parameter: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-result.k" [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  config=Config {apply:fn(value:i64)->f64:return f64(value)}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-result.k" -o "${OUTPUT_DIR}/reject-wrong-result" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-result: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-channel.k" [=[error E:
  Bad
type Config:
  apply:fn(i64)->i64
fn main()->i64:
  config=Config {apply:fn(value:i64)->i64 ! E:return value}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-channel.k" -o "${OUTPUT_DIR}/reject-wrong-channel" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-channel: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-update.k" [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  config=Config {apply:fn(value):return value}
  updated=config with {apply:fn(value:f64):return value}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-update.k" -o "${OUTPUT_DIR}/reject-wrong-update" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-update: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-missing-field.k" [=[type Config:
  apply:fn(i64)->i64
fn main()->i64:
  config=Config {}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-missing-field.k" -o "${OUTPUT_DIR}/reject-missing-field" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: missing-field: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-unhandled.k" [=[error E:
  Bad
type Config:
  apply:fn(i64)->i64 ! E
fn main()->i64:
  config=Config {apply:fn(value:i64)->i64 ! E:return value}
  return config.apply(42)+1
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-unhandled.k" -o "${OUTPUT_DIR}/reject-unhandled" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: unhandled: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-enum-outside-match.k" [=[enum Choice:
  Some:
    callback:fn(i64)->i64
fn main()->i64:
  choice=Choice.Some {callback:fn(value):return value}
  return choice.callback(42)
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-enum-outside-match.k" -o "${OUTPUT_DIR}/reject-enum-outside-match" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: enum-outside-match: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-enum-wrong-payload.k" [=[enum Choice:
  Some:
    callback:fn(i64)->i64
fn main()->i64:
  choice=Choice.Some {callback:0}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-enum-wrong-payload.k" -o "${OUTPUT_DIR}/reject-enum-wrong-payload" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: enum-wrong-payload: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-shape.k" [=[type Config:
  apply:fn(Tensor[i64,2])->i64
fn sum(values:Tensor[i64,3])->i64:
  return reduceSum(values)
fn main()->i64:
  config=Config {apply:sum}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-shape.k" -o "${OUTPUT_DIR}/reject-wrong-shape" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-shape: ${STATUS}: ${OUT}${ERR}")
endif()
file(WRITE "${OUTPUT_DIR}/reject-wrong-nominal.k" [=[type A:
  value:i64
type B:
  value:i64
type Config:
  apply:fn(A)->i64
fn accept(value:B)->i64:
  return value.value
fn main()->i64:
  config=Config {apply:accept}
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-wrong-nominal.k" -o "${OUTPUT_DIR}/reject-wrong-nominal" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: wrong-nominal: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(supported-coordination [=[type Config:
  apply:fn(i64)->i64 ! TaskError
fn worker(value:i64)->i64:
  return value
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn worker(value)
fn main()->i64:
  config=Config {apply:run}
  return 0
]=])
EXPECT(supported-coordination 0)
COMPILE(supported-recursive-signature [=[type Config:
  value:i64
  apply:fn(Config)->i64
fn read(config:Config)->i64:
  return config.value
fn main()->i64:
  config=Config {value:42,apply:read}
  return config.apply(config)
]=])
EXPECT(supported-recursive-signature 42)
file(WRITE "${OUTPUT_DIR}/reject-direct-functions-array.k" [=[fn main()->i64:
  values=[fn(value:i64):return value,fn(value:f64):return value]
  return 0
]=])
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/reject-direct-functions-array.k" -o "${OUTPUT_DIR}/reject-direct-functions-array" RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "1")
  message(FATAL_ERROR "Invalid callable field accepted/crashed: direct-functions-array: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(supported-worker-argument [=[type Config:
  apply:fn(i64)->i64
fn worker(config:Config)->i64:
  return config.apply(42)
fn main()->i64 ! TaskError:
  config=Config {apply:fn(value):return value}
  return try parallel:
    try await spawn worker(config)
]=])
EXPECT(supported-worker-argument 42)
COMPILE(supported-worker-nested-array [=[type Config:
  apply:fn(i64)->i64
type Outer:
  values:[Config]
fn worker(outer:Outer)->i64:
  return outer.values[0].apply(42)
fn main()->i64 ! TaskError:
  outer=Outer {values:[Config {apply:fn(value):return value}]}
  return try parallel:
    try await spawn worker(outer)
]=])
EXPECT(supported-worker-nested-array 42)
COMPILE(supported-worker-result [=[type Config:
  apply:fn(i64)->i64
fn worker(value:i64)->Config:
  return Config {apply:fn(input):return input+value}
fn main()->i64 ! TaskError:
  return try parallel:
    config=try await spawn worker(20)
    config.apply(22)
]=])
EXPECT(supported-worker-result 42)
COMPILE(supported-worker-error-payload [=[type Config:
  apply:fn(i64)->i64
error E:
  Bad:
    config:Config
fn worker(value:i64)->i64 ! E:
  return value
fn main()->i64 ! (E | TaskError):
  return try parallel:
    try await spawn worker(42)
]=])
EXPECT(supported-worker-error-payload 42)
COMPILE(supported-worker-map-capture [=[type Config:
  apply:fn(i64)->i64
fn main()->i64 ! TaskError:
  config=Config {apply:fn(value):return value*2}
  return try parallel:
    values=try await spawn map([21],fn(value):return config.apply(value))
    values[0]
]=])
EXPECT(supported-worker-map-capture 42)
COMPILE(supported-worker-map-input [=[type Config:
  apply:fn(i64)->i64
fn main()->i64 ! TaskError:
  configs=[Config {apply:fn(value):return value*2}]
  return try parallel:
    values=try await spawn map(configs,fn(config):return config.apply(21))
    values[0]
]=])
EXPECT(supported-worker-map-input 42)
COMPILE(supported-worker-query [=[type Config:
  apply:fn(i64)->i64
fn main()->i64 ! TaskError:
  configs=[Config {apply:fn(value):return value}]
  return try parallel:
    try await spawn length(configs)
]=])
EXPECT(supported-worker-query 1)
COMPILE(supported-worker-slice [=[type Config:
  apply:fn(i64)->i64
fn main()->i64 ! TaskError:
  configs=[Config {apply:fn(value):return value}]
  return try parallel:
    values=try await spawn slice(configs,0,1)
    length(values)
]=])
EXPECT(supported-worker-slice 1)
COMPILE(supported-worker-concat [=[type Config:
  apply:fn(i64)->i64
fn main()->i64 ! TaskError:
  configs=[Config {apply:fn(value):return value}]
  return try parallel:
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
file(WRITE "${OUTPUT_DIR}/helper.k" "export type Config:\n  apply:fn(i64)->i64\nexport fn make(offset:i64)->Config:\n  return Config {apply:fn(value):return value+offset}\n")
COMPILE(imported "from \"./helper.k\" import Config as RemoteConfig, make\ntype Config:\n  value:i64\nfn main()->i64:\n  config:RemoteConfig=make(20)\n  return config.apply(22)\n")
EXPECT(imported 42)
foreach(ARTIFACT IN ITEMS rejected rejected.ll rejected.mlir)
  file(WRITE "${OUTPUT_DIR}/${ARTIFACT}" "preserved")
endforeach()
file(WRITE "${OUTPUT_DIR}/rejected.k" "type Config:\n  apply:fn(i64)->i64\nfn main()->i64:\n  config=Config {apply:0}\n  return 0\n")
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
