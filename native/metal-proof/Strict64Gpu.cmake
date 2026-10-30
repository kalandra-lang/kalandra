# Binary64 bit carriers use i64 storage and integer-only device arithmetic.
# Source admission is gated independently by the typed shader/host bridge.
add_executable(kalandra-metal-strict64 Strict.cpp)
target_compile_definitions(kalandra-metal-strict64 PRIVATE KALANDRA_STRICT_GPU64=1)
target_compile_options(kalandra-metal-strict64 PRIVATE -Wall -Wextra -Werror)
target_link_libraries(kalandra-metal-strict64 PRIVATE kalandra-strict-float)
set(BASE "${OUT}/strict64-gpu")
add_custom_command(OUTPUT "${BASE}.metallib" "${BASE}.spvasm" "${BASE}.metal"
  COMMAND $<TARGET_FILE:kalandra-metal-strict64> "${CMAKE_CURRENT_SOURCE_DIR}/strict64-gpu.mlir" "${BASE}.lowered.mlir"
  COMMAND "${MLIR_OPT}" "${BASE}.lowered.mlir" --verify-each
    "--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))"
    -o "${BASE}.converted.mlir"
  COMMAND $<TARGET_FILE:kalandra-metal-extract> "${BASE}.converted.mlir" "${BASE}.kernel.mlir" --metal-compiler-rounding
  COMMAND "${MLIR_TRANSLATE}" --no-implicit-module --serialize-spirv "${BASE}.kernel.mlir" -o "${BASE}.spv"
  COMMAND "${VALIDATOR}" --target-env vulkan1.0 "${BASE}.spv"
  COMMAND "${DISASSEMBLER}" "${BASE}.spv" -o "${BASE}.spvasm"
  COMMAND "${CROSS}" "${BASE}.spv" --msl --msl-version "${LOCK_typedMslVersion}" --output "${BASE}.metal"
  COMMAND "${KALANDRA_METAL_COMPILER}" "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}" -isysroot "${KALANDRA_METAL_SDK}"
    "-std=${LOCK_typedMetalLanguageVersion}" -fno-fast-math -ffp-contract=off
    "-fmodules-cache-path=${OUT}/module-cache" -c "${BASE}.metal" -o "${BASE}.air"
  COMMAND "${KALANDRA_METAL_COMPILER}" "${BASE}.air" -o "${BASE}.metallib"
  DEPENDS strict64-gpu.mlir kalandra-metal-strict64 kalandra-metal-extract toolchain.json
  BYPRODUCTS "${BASE}.lowered.mlir" "${BASE}.converted.mlir" "${BASE}.kernel.mlir" "${BASE}.spv" "${BASE}.air" VERBATIM)
set(COMPACT "${OUT}/strict64-compact")
add_custom_command(OUTPUT "${COMPACT}.ll"
  COMMAND $<TARGET_FILE:kalandra-metal-strict64> "${CMAKE_CURRENT_SOURCE_DIR}/strict64-cpu.mlir" "${COMPACT}.mlir"
  COMMAND "${MLIR_OPT}" "${COMPACT}.mlir" --verify-each --canonicalize --cse
    --convert-scf-to-cf --convert-to-llvm --reconcile-unrealized-casts -o "${COMPACT}.llvm.mlir"
  COMMAND "${MLIR_TRANSLATE}" --mlir-to-llvmir "${COMPACT}.llvm.mlir" -o "${COMPACT}.ll"
  DEPENDS strict64-cpu.mlir kalandra-metal-strict64
  BYPRODUCTS "${COMPACT}.mlir" "${COMPACT}.llvm.mlir" VERBATIM)
set(STRICT64_GPU_OUTPUTS "${BASE}.metallib")
foreach(VARIANT IN ITEMS debug optimized sanitized)
  if(VARIANT STREQUAL "debug")
    set(FLAGS -O0)
  elseif(VARIANT STREQUAL "optimized")
    set(FLAGS -O2)
  else()
    set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  endif()
  set(CPU_BINARY "${COMPACT}-${VARIANT}")
  set(GPU_BINARY "${BASE}-${VARIANT}")
  add_custom_command(OUTPUT "${CPU_BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -Wno-override-module -isystem "${KALANDRA_LLVM_ROOT}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/Strict64Cpu.cpp" "${COMPACT}.ll"
      "-L${KALANDRA_LLVM_ROOT}/lib" "-Wl,-rpath,${KALANDRA_LLVM_ROOT}/lib" -lLLVM -o "${CPU_BINARY}"
    DEPENDS Strict64Cpu.cpp Float64Oracle.h "${COMPACT}.ll" VERBATIM)
  add_custom_command(OUTPUT "${GPU_BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -fobjc-arc -fblocks -fno-objc-msgsend-class-selector-stubs -isystem "${KALANDRA_LLVM_ROOT}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/Strict64Gpu.mm" -framework Foundation -framework Metal
      "-L${KALANDRA_LLVM_ROOT}/lib" "-Wl,-rpath,${KALANDRA_LLVM_ROOT}/lib" -lLLVM -o "${GPU_BINARY}"
    DEPENDS Strict64Gpu.mm Float64Oracle.h VERBATIM)
  list(APPEND STRICT64_GPU_OUTPUTS "${CPU_BINARY}" "${GPU_BINARY}")
  if(BUILD_TESTING)
    add_test(NAME "native-strict64-compact-${VARIANT}" COMMAND "${CPU_BINARY}")
    add_test(NAME "native-strict64-gpu-${VARIANT}" COMMAND "${GPU_BINARY}" "${BASE}.metallib" "${BASE}-${VARIANT}.json")
    set_tests_properties("native-strict64-compact-${VARIANT}" "native-strict64-gpu-${VARIANT}" PROPERTIES TIMEOUT 180)
    set_tests_properties("native-strict64-gpu-${VARIANT}" PROPERTIES RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endif()
endforeach()
add_custom_target(native-metal-strict64 ALL DEPENDS ${STRICT64_GPU_OUTPUTS})
if(BUILD_TESTING)
  add_test(NAME native-strict64-gpu-artifacts COMMAND "${CMAKE_COMMAND}"
    "-DOUTPUT_DIR=${OUT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyStrict64Gpu.cmake")
endif()
