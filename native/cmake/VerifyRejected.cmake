execute_process(COMMAND "${CHECKER}" "${INPUT}" RESULT_VARIABLE STATUS ERROR_VARIABLE STDERR)
if(NOT "${STATUS}" STREQUAL "1" OR NOT STDERR MATCHES "type of return operand")
  message(FATAL_ERROR "Invalid MLIR did not produce the expected type diagnostic: ${STATUS}: ${STDERR}")
endif()
