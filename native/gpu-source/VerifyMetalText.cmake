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
