execute_process(COMMAND "${COMPILER}" "${SOURCE}" --target metal -O2 -o "${OUTPUT}"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostics)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "GPU text compiler failure: ${diagnostics}")
endif()
execute_process(COMMAND "${OUTPUT}/program" RESULT_VARIABLE status
  OUTPUT_VARIABLE result ERROR_VARIABLE diagnostics)
# The digits encode the UTF-8 byte count, scalar count, and two independent
# checks of the first scalar. The fixture also exercises empty and padded lanes.
if(NOT status EQUAL 0 OR NOT result STREQUAL "11411 0 3211 3200 4100\n")
  message(FATAL_ERROR "GPU text output failure: ${status} ${result} ${diagnostics}")
endif()

execute_process(COMMAND "${COMPILER}" "${RESULT_SOURCE}" --target metal -O2 -o "${OUTPUT}-result"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostics)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "GPU text result compiler failure: ${diagnostics}")
endif()
execute_process(COMMAND "${OUTPUT}-result/program" RESULT_VARIABLE status
  OUTPUT_FILE "${OUTPUT}-result/output.bin" ERROR_VARIABLE diagnostics)
file(READ "${OUTPUT}-result/output.bin" result HEX)
# Compare bytes, including the embedded NUL, after both device results settle.
if(NOT status EQUAL 0 OR NOT result STREQUAL "d8b3002fc3a9e78cabf09fa7aa0a")
  message(FATAL_ERROR "GPU text result failure: ${status} ${result} ${diagnostics}")
endif()
execute_process(COMMAND "${OUTPUT}-result/program" negative-index RESULT_VARIABLE status
  OUTPUT_VARIABLE result ERROR_VARIABLE diagnostics)
# The GPU supervisor prints TaskFailure::Fault / FaultKind.Bounds as 1/5.
if(NOT status EQUAL 1 OR NOT result STREQUAL "" OR NOT diagnostics MATCHES "Metal task failed: 1/5")
  message(FATAL_ERROR "GPU text bounds failure was not reported: ${status} ${result} ${diagnostics}")
endif()
