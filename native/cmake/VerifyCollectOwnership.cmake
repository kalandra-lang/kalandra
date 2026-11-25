include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
if(NOT DEFINED PHASE)
  set(PHASE "ALL")
endif()
if(PHASE STREQUAL "ALL" OR PHASE STREQUAL "BUILD")
  file(READ "${SOURCE_DIR}/tests/fixtures/scoped-owned-maps/collect-probe.k" SOURCE)
  COMPILE_IR(probe "${SOURCE}" --test-entry probe)
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    -DEXPECTED_PER_PAIR=167 -DTASK_IN_FLIGHT=16 "${SOURCE_DIR}/tests/ArrayRebindingTaskTests.cpp"
    "${OUTPUT_DIR}/probe.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/probe-harness")

  file(READ "${SOURCE_DIR}/tests/fixtures/scoped-owned-maps/collect-recovery-probe.k" SOURCE)
  COMPILE_IR(recovery "${SOURCE}" --test-entry probe)
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    -DTASK_IN_FLIGHT=16 "${SOURCE_DIR}/tests/ArrayRebindingTaskTests.cpp"
    "${OUTPUT_DIR}/recovery.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/recovery-harness")
endif()
if(PHASE STREQUAL "ALL" OR PHASE STREQUAL "RUN")
  RUN_OK("${OUTPUT_DIR}/probe-harness")
  RUN_OK("${OUTPUT_DIR}/recovery-harness")
endif()
