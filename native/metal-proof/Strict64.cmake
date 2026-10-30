# Binary64 is CPU-only. Keep the existing binary32/Metal tool and artifacts unchanged.
add_executable(kalandra-cpu-strict Strict.cpp)
target_compile_definitions(kalandra-cpu-strict PRIVATE KALANDRA_STRICT_CPU=1)
target_compile_options(kalandra-cpu-strict PRIVATE -Wall -Wextra -Werror)
target_link_libraries(kalandra-cpu-strict PRIVATE kalandra-strict-float)
add_custom_command(OUTPUT "${OUT}/strict64-cpu.ll"
  COMMAND $<TARGET_FILE:kalandra-cpu-strict> "${CMAKE_CURRENT_SOURCE_DIR}/strict64-cpu.mlir" "${OUT}/strict64-cpu.lowered.mlir"
  COMMAND "${MLIR_OPT}" "${OUT}/strict64-cpu.lowered.mlir" --verify-each --canonicalize --cse
    --convert-scf-to-cf --convert-to-llvm --reconcile-unrealized-casts -o "${OUT}/strict64-cpu.llvm.mlir"
  COMMAND "${MLIR_TRANSLATE}" --mlir-to-llvmir "${OUT}/strict64-cpu.llvm.mlir" -o "${OUT}/strict64-cpu.ll"
  DEPENDS strict64-cpu.mlir kalandra-cpu-strict
  BYPRODUCTS "${OUT}/strict64-cpu.lowered.mlir" "${OUT}/strict64-cpu.llvm.mlir" VERBATIM)
set(STRICT64_OUTPUTS "${OUT}/strict64-cpu.ll")
foreach(VARIANT IN ITEMS debug optimized sanitized)
  if(VARIANT STREQUAL "debug")
    set(FLAGS -O0)
  elseif(VARIANT STREQUAL "optimized")
    set(FLAGS -O2)
  else()
    set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  endif()
  set(BINARY "${OUT}/strict64-cpu-${VARIANT}")
  add_custom_command(OUTPUT "${BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -Wno-override-module -isystem "${KALANDRA_LLVM_ROOT}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/Strict64Cpu.cpp" "${OUT}/strict64-cpu.ll"
      "-L${KALANDRA_LLVM_ROOT}/lib" "-Wl,-rpath,${KALANDRA_LLVM_ROOT}/lib" -lLLVM -o "${BINARY}"
    DEPENDS Strict64Cpu.cpp Float64Oracle.h "${OUT}/strict64-cpu.ll" VERBATIM)
  list(APPEND STRICT64_OUTPUTS "${BINARY}")
  if(BUILD_TESTING)
    add_test(NAME "native-strict64-cpu-${VARIANT}" COMMAND "${BINARY}")
    set_tests_properties("native-strict64-cpu-${VARIANT}" PROPERTIES TIMEOUT 180)
  endif()
endforeach()
add_custom_target(native-strict64-cpu ALL DEPENDS ${STRICT64_OUTPUTS})
if(BUILD_TESTING)
  add_test(NAME native-strict64-artifacts-and-gates COMMAND "${CMAKE_COMMAND}"
    "-DOUTPUT_DIR=${OUT}" "-DLOWERER=$<TARGET_FILE:kalandra-cpu-strict>"
    "-DGPU_LOWERER=$<TARGET_FILE:kalandra-metal-strict>"
    "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/strict64-cpu.mlir"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyStrict64.cmake")
endif()
