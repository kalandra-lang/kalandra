function(RUN_PROOF)
  execute_process(COMMAND "${PROOF_BINARY}" ${ARGN}
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
  if(NOT "${STATUS}" STREQUAL "0" OR NOT STDERR STREQUAL "")
    message(FATAL_ERROR "Native proof failed: ${STATUS}: ${STDOUT}${STDERR}")
  endif()
  set(PROOF_OUTPUT "${STDOUT}" PARENT_SCOPE)
endfunction()
file(READ "${PROOF_DIR}/projection.ll" IR)
file(READ "${PROOF_DIR}/projection.s" ASSEMBLY)
if(NOT IR MATCHES "call [^\n]*@project\\(" OR
   NOT IR MATCHES "call void @_mlir_memref_to_llvm_free\\(" OR
   NOT IR MATCHES "@_mlir_memref_to_llvm_alloc" OR
   IR MATCHES "[ \t](fast|reassoc|contract|afn|nnan|ninf|nsz|arcp)[ \t]" OR
   IR MATCHES "llvm\\.(fma|fmuladd)" OR
   ASSEMBLY MATCHES "[ \t](fmadd|fmsub|fnmadd|fnmsub|fmla|fmls)[ \t]")
  message(FATAL_ERROR "IR/assembly violates call, ownership, or numerical contract")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PATH=/usr/bin:/bin" "${PROOF_BINARY}"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT "${STATUS}" STREQUAL "0" OR NOT STDOUT STREQUAL "163\n" OR NOT STDERR STREQUAL "")
  message(FATAL_ERROR "Canonical executable failed: ${STATUS}: ${STDOUT}${STDERR}")
endif()
execute_process(COMMAND /usr/bin/otool -L "${PROOF_BINARY}" RESULT_VARIABLE STATUS OUTPUT_VARIABLE DEPS)
if(NOT "${STATUS}" STREQUAL "0" OR DEPS MATCHES "libLLVM|libMLIR|JavaScriptCore|libnode|libbun")
  message(FATAL_ERROR "Unexpected native runtime dependency: ${DEPS}")
endif()
file(STRINGS "${PROOF_DIR}/cases/cases.txt" CASES)
list(LENGTH CASES COUNT)
if(NOT COUNT EQUAL 49)
  message(FATAL_ERROR "Expected 49 independent reference cases")
endif()
foreach(CASE IN LISTS CASES)
  string(REPLACE "|" ";" FIELDS "${CASE}")
  list(GET FIELDS 0 NAME)
  list(GET FIELDS 1 EXPECTED)
  list(GET FIELDS 2 IS_NAN)
  RUN_PROOF(--bits --input "${PROOF_DIR}/cases/${NAME}.bin")
  string(JSON ACTUAL GET "${PROOF_OUTPUT}" bits)
  string(JSON ALLOCATIONS GET "${PROOF_OUTPUT}" allocations)
  string(JSON RELEASES GET "${PROOF_OUTPUT}" releases)
  if(IS_NAN)
    math(EXPR EXPONENT "${ACTUAL} & 0x7f800000")
    math(EXPR PAYLOAD "${ACTUAL} & 0x007fffff")
    if(NOT EXPONENT EQUAL 2139095040 OR PAYLOAD EQUAL 0)
      message(FATAL_ERROR "Expected NaN for ${NAME}: ${PROOF_OUTPUT}")
    endif()
  elseif(NOT ACTUAL EQUAL EXPECTED)
    message(FATAL_ERROR "Numerical mismatch for ${NAME}: expected ${EXPECTED}: ${PROOF_OUTPUT}")
  endif()
  if(NOT ALLOCATIONS EQUAL 1 OR NOT RELEASES EQUAL 1)
    message(FATAL_ERROR "Ownership mismatch for ${NAME}: ${PROOF_OUTPUT}")
  endif()
endforeach()
RUN_PROOF(--bits --repeat 10000)
string(JSON ALLOCATIONS GET "${PROOF_OUTPUT}" allocations)
string(JSON RELEASES GET "${PROOF_OUTPUT}" releases)
string(JSON BITS GET "${PROOF_OUTPUT}" bits)
if(NOT ALLOCATIONS EQUAL 10000 OR NOT RELEASES EQUAL 10000 OR NOT BITS EQUAL 1126367232)
  message(FATAL_ERROR "Repeated ownership check failed: ${PROOF_OUTPUT}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_PROOF_FAIL_ALLOC=1 "${PROOF_BINARY}"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT STDOUT STREQUAL "" OR NOT STDERR MATCHES "AllocationFault")
  message(FATAL_ERROR "Injected allocation failure did not fault safely: ${STATUS}: ${STDERR}")
endif()
message(STATUS "49 float32 cases, 10000 allocations/releases, and allocation-failure check passed")

get_filename_component(VARIANT "${PROOF_BINARY}" NAME)
set(REPORT [=[{
  "variant": "@VARIANT@",
  "cases": 49,
  "repeatedCalls": 10000,
  "canonicalOutput": 163,
  "scope": "hand-written fixed-shape MLIR; no Kalandra frontend yet"
}
]=])
string(CONFIGURE "${REPORT}" REPORT @ONLY)
file(WRITE "${PROOF_DIR}/${VARIANT}-report.json" "${REPORT}")
