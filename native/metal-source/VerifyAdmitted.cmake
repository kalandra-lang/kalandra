# Former rejection probes become positive source gates as typed programs expand.
# Device semantics are covered independently by the typed application suites.
function(admit_case NAME SOURCE)
  file(WRITE "${ROOT}/${NAME}-admitted.k" "${SOURCE}")
  set(DEST "${OUTPUT_DIR}/${NAME}-admitted")
  execute_process(COMMAND "${COMPILER}" --arithmetic software "${ROOT}/${NAME}-admitted.k" -o "${DEST}"
    RESULT_VARIABLE STATUS ERROR_VARIABLE ERROR TIMEOUT 90)
  if(NOT STATUS EQUAL 0 OR NOT EXISTS "${DEST}/kernel0.mlir")
    message(FATAL_ERROR "${NAME}: typed program admission failed: ${STATUS}: ${ERROR}")
  endif()
  foreach(FILE IN ITEMS host.cpp manifest.txt placement.ir continuation.plan)
    if(NOT EXISTS "${DEST}/${FILE}")
      message(FATAL_ERROR "${NAME}: missing admitted artifact ${FILE}")
    endif()
  endforeach()
endfunction()
