file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(EXTRA)
set(FLAGS -O0)
if(VARIANT STREQUAL "optimized")
  list(APPEND EXTRA -O2)
  set(FLAGS -O2)
elseif(VARIANT STREQUAL "sanitized")
  list(APPEND EXTRA --sanitize)
  set(FLAGS -O1 -fsanitize=address,undefined -fno-sanitize-recover=all)
elseif(VARIANT STREQUAL "thread-sanitized")
  list(APPEND EXTRA --thread-sanitize)
  set(FLAGS -O1 -g -fsanitize=thread)
endif()
function(RUN_OK)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE STATUS OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
  if(NOT "${STATUS}" STREQUAL "0" OR NOT STDERR STREQUAL "")
    message(FATAL_ERROR "Command failed: ${ARGN}: ${STATUS}: ${STDOUT}${STDERR}")
  endif()
  set(OUTPUT "${STDOUT}" PARENT_SCOPE)
endfunction()
set(IR "${OUTPUT_DIR}/kernels.ll")
RUN_OK("${COMPILER}" "${SOURCE_ROOT}/native/tests/fixtures/dynamic-projection.k"
  -o "${OUTPUT_DIR}/projection" --emit-llvm "${IR}" --test-entry reference ${EXTRA})
file(READ "${IR}" LLVM)
if(NOT LLVM MATCHES "load atomic i8" OR NOT LLVM MATCHES "@kalandra_poll_slow_v1")
  message(FATAL_ERROR "CPU loop cancellation guards missing")
endif()
# Confirm the LLVM memory accesses, not just the C++ harness/runtime, receive
# real instrumentation when Clang consumes the compiler's IR.
if(VARIANT STREQUAL "sanitized" OR VARIANT STREQUAL "thread-sanitized")
  RUN_OK("${CLANGXX}" ${FLAGS} -Wno-override-module -S -emit-llvm "${IR}" -o "${OUTPUT_DIR}/instrumented.ll")
  file(READ "${OUTPUT_DIR}/instrumented.ll" INSTRUMENTED)
  if(VARIANT STREQUAL "sanitized" AND NOT INSTRUMENTED MATCHES "call [^\n]*@__asan_report_(load|store)")
    message(FATAL_ERROR "Generated kernel ASan instrumentation missing")
  endif()
  if(VARIANT STREQUAL "thread-sanitized" AND NOT INSTRUMENTED MATCHES "call [^\n]*@__tsan_(read|write)")
    message(FATAL_ERROR "Generated kernel TSan instrumentation missing")
  endif()
endif()
# Compile the generated LLVM and runtime with the same sanitizer instrumentation.
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror ${FLAGS} -ffp-contract=off -Wno-override-module
  "${SOURCE_ROOT}/native/tests/CompiledWorkerTests.cpp" "${IR}"
  "${SOURCE_ROOT}/native/runtime/Runtime.cpp" "${SOURCE_ROOT}/native/runtime/Tasks.cpp"
  "${SOURCE_ROOT}/native/runtime/OrdinaryEntry.cpp"
  -o "${OUTPUT_DIR}/compiled-workers")
RUN_OK("${OUTPUT_DIR}/compiled-workers")
if(NOT OUTPUT MATCHES "PASS generated CPU kernels")
  message(FATAL_ERROR "Compiled worker conformance missing: ${OUTPUT}")
endif()

if(VARIANT STREQUAL "thread-sanitized")
  file(WRITE "${OUTPUT_DIR}/preserved" "preserve existing output")
  execute_process(COMMAND "${COMPILER}" "${SOURCE_ROOT}/native/tests/fixtures/task-nested.k"
    -o "${OUTPUT_DIR}/preserved" --sanitize --thread-sanitize
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
  file(READ "${OUTPUT_DIR}/preserved" PRESERVED)
  if(NOT "${STATUS}" STREQUAL "2" OR NOT STDERR MATCHES "separate builds" OR
      NOT PRESERVED STREQUAL "preserve existing output")
    message(FATAL_ERROR "Mixed sanitizer modes were not rejected safely: ${STATUS}: ${STDOUT}${STDERR}")
  endif()
endif()
