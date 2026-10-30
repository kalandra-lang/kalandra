set(PROOF_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(MLIR_OPT "${KALANDRA_LLVM_ROOT}/bin/mlir-opt${CMAKE_EXECUTABLE_SUFFIX}")
set(MLIR_TRANSLATE "${KALANDRA_LLVM_ROOT}/bin/mlir-translate${CMAKE_EXECUTABLE_SUFFIX}")
foreach(TOOL IN ITEMS "${MLIR_OPT}" "${MLIR_TRANSLATE}")
  execute_process(COMMAND "${TOOL}" --version OUTPUT_VARIABLE VERSION_TEXT RESULT_VARIABLE STATUS)
  if(NOT STATUS EQUAL 0 OR NOT VERSION_TEXT MATCHES "version ${KALANDRA_LLVM_VERSION}")
    message(FATAL_ERROR "Missing or mismatched pinned tool: ${TOOL}")
  endif()
endforeach()
add_custom_command(
  OUTPUT "${PROOF_DIR}/projection.ll" "${PROOF_DIR}/projection.s"
  COMMAND $<TARGET_FILE:kalandra-mlir-check> "${CMAKE_CURRENT_SOURCE_DIR}/projection.mlir"
  COMMAND "${MLIR_OPT}" "${CMAKE_CURRENT_SOURCE_DIR}/projection.mlir" --verify-each
    --one-shot-bufferize=bufferize-function-boundaries -o "${PROOF_DIR}/bufferized.mlir"
  COMMAND "${MLIR_OPT}" "${PROOF_DIR}/bufferized.mlir" --verify-each
    --buffer-deallocation-pipeline -o "${PROOF_DIR}/owned.mlir"
  COMMAND "${MLIR_OPT}" "${PROOF_DIR}/owned.mlir" --verify-each
    --convert-linalg-to-loops --convert-scf-to-cf
    --finalize-memref-to-llvm=use-generic-functions --convert-to-llvm
    --reconcile-unrealized-casts -o "${PROOF_DIR}/llvm.mlir"
  COMMAND "${MLIR_TRANSLATE}" --mlir-to-llvmir "${PROOF_DIR}/llvm.mlir" -o "${PROOF_DIR}/projection.ll"
  COMMAND "${CMAKE_C_COMPILER}" -O2 -fno-inline -ffp-contract=off -S -Wno-override-module
    "${PROOF_DIR}/projection.ll" -o "${PROOF_DIR}/projection.s"
  DEPENDS projection.mlir kalandra-mlir-check
  BYPRODUCTS "${PROOF_DIR}/bufferized.mlir" "${PROOF_DIR}/owned.mlir" "${PROOF_DIR}/llvm.mlir"
  VERBATIM)
add_custom_command(
  OUTPUT "${PROOF_DIR}/cases/cases.txt"
  COMMAND $<TARGET_FILE:kalandra-proof-cases> "${PROOF_DIR}/cases"
  DEPENDS kalandra-proof-cases VERBATIM)
set(PROOF_OUTPUTS "${PROOF_DIR}/cases/cases.txt")
foreach(VARIANT IN ITEMS debug optimized sanitized)
  if(VARIANT STREQUAL "debug")
    set(FLAGS -O0)
  elseif(VARIANT STREQUAL "optimized")
    set(FLAGS -O2 -fno-inline)
  else()
    set(FLAGS -O1 -fno-inline -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  endif()
  set(BINARY "${PROOF_DIR}/proof-${VARIANT}")
  add_custom_command(OUTPUT "${BINARY}"
    COMMAND "${CMAKE_C_COMPILER}" ${FLAGS} -std=c11 -Wall -Wextra -Werror
      -ffp-contract=off -Wno-override-module "${CMAKE_CURRENT_SOURCE_DIR}/harness.c"
      "${PROOF_DIR}/projection.ll" -o "${BINARY}"
    DEPENDS harness.c "${PROOF_DIR}/projection.ll" VERBATIM)
  list(APPEND PROOF_OUTPUTS "${BINARY}")
  if(BUILD_TESTING)
    add_test(NAME native-proof-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DPROOF_BINARY=${BINARY}" "-DPROOF_DIR=${PROOF_DIR}"
        -P "${PROJECT_SOURCE_DIR}/native/cmake/VerifyProof.cmake")
  endif()
endforeach()
add_custom_target(native-toolchain-proof ALL DEPENDS ${PROOF_OUTPUTS})
if(BUILD_TESTING)
  add_test(NAME mlir-api-valid COMMAND kalandra-mlir-check "${CMAKE_CURRENT_SOURCE_DIR}/projection.mlir")
  add_test(NAME mlir-api-invalid
    COMMAND "${CMAKE_COMMAND}" "-DCHECKER=$<TARGET_FILE:kalandra-mlir-check>"
      "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/invalid.mlir"
      -P "${PROJECT_SOURCE_DIR}/native/cmake/VerifyRejected.cmake")
endif()
