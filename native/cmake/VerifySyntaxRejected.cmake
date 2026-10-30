file(WRITE "${OUTPUT_DIR}/invalid.k" "fn first():\n  a =\n  b =\n  1\nfn last():\n  2\n")
execute_process(COMMAND "${PARSER}" "${OUTPUT_DIR}/invalid.k"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT "${STATUS}" STREQUAL "1" OR NOT STDOUT STREQUAL "" OR
   NOT STDERR MATCHES "invalid.k:2:6: error\\[P012\\]" OR
   NOT STDERR MATCHES "invalid.k:3:6: error\\[P012\\]")
  message(FATAL_ERROR "Expected source diagnostics and failure exit: ${STATUS}: ${STDOUT}${STDERR}")
endif()
execute_process(COMMAND "${PARSER}" "${OUTPUT_DIR}/does-not-exist.k"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT "${STATUS}" STREQUAL "1" OR NOT STDERR MATCHES "cannot open source file")
  message(FATAL_ERROR "Expected file error: ${STATUS}: ${STDERR}")
endif()
