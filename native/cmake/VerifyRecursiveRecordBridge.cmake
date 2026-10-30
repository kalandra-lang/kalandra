function(RUN_OK)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "0" OR NOT ERR STREQUAL "")
    message(FATAL_ERROR "Recursive record bridge: ${STATUS}: ${OUT}${ERR}")
  endif()
endfunction()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(FLAGS -O0)
if(VARIANT STREQUAL "optimized")
  set(FLAGS -O2)
elseif(VARIANT STREQUAL "sanitized")
  set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(VARIANT STREQUAL "thread-sanitized")
  set(FLAGS -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
endif()
RUN_OK("${EMITTER}" "${OUTPUT_DIR}/bridge.ll")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveRecordBridgeTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/bridge")
RUN_OK("${OUTPUT_DIR}/bridge")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "-I${SOURCE_DIR}/include" "${SOURCE_DIR}/tests/RecursiveCollectionBridgeTests.cpp"
  "${OUTPUT_DIR}/bridge.ll" ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/collections")
RUN_OK("${OUTPUT_DIR}/collections")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveStringArrayBridgeTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/string-arrays")
RUN_OK("${OUTPUT_DIR}/string-arrays")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveRecordArrayBridgeTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/record-arrays")
RUN_OK("${OUTPUT_DIR}/record-arrays")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveCallableBridgeTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/callables")
RUN_OK("${OUTPUT_DIR}/callables")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveFunctionArrayBridgeTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/function-arrays")
RUN_OK("${OUTPUT_DIR}/function-arrays")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursivePacketBridgeTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/packets")
RUN_OK("${OUTPUT_DIR}/packets")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveTaskFieldTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/task-fields")
RUN_OK("${OUTPUT_DIR}/task-fields")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveTaskArrayTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/task-arrays")
RUN_OK("${OUTPUT_DIR}/task-arrays")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveTaskCallableTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/task-callables")
RUN_OK("${OUTPUT_DIR}/task-callables")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveTaskCallableAggregateTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/task-callable-aggregates")
RUN_OK("${OUTPUT_DIR}/task-callable-aggregates")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveTaskEntryTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/task-entries")
RUN_OK("${OUTPUT_DIR}/task-entries")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/RecursiveTaskErrorTests.cpp" "${OUTPUT_DIR}/bridge.ll"
  ${TASK_ABI_OBJECTS} "${RUNTIME}" -o "${OUTPUT_DIR}/task-errors")
RUN_OK("${OUTPUT_DIR}/task-errors")
