set(INTEGER_OUTPUTS)
set(INTEGER_LL)
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64)
  set(BASE "${OUT}/integer-${TYPE}")
  add_custom_command(OUTPUT "${BASE}.metallib" "${BASE}.ll" "${BASE}.spvasm" "${BASE}.metal"
    COMMAND $<TARGET_FILE:kalandra-metal-integer> "${TYPE}" "${BASE}.gpu.mlir" "${BASE}.cpu.mlir"
    COMMAND "${MLIR_OPT}" "${BASE}.cpu.mlir" --verify-each --canonicalize --cse
      --convert-scf-to-cf --convert-to-llvm --reconcile-unrealized-casts -o "${BASE}.llvm.mlir"
    COMMAND "${MLIR_TRANSLATE}" --mlir-to-llvmir "${BASE}.llvm.mlir" -o "${BASE}.ll"
    COMMAND "${MLIR_OPT}" "${BASE}.gpu.mlir" --verify-each
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
    DEPENDS kalandra-metal-integer kalandra-metal-extract toolchain.json
    BYPRODUCTS "${BASE}.gpu.mlir" "${BASE}.cpu.mlir" "${BASE}.llvm.mlir" "${BASE}.converted.mlir"
      "${BASE}.kernel.mlir" "${BASE}.spv" "${BASE}.air" VERBATIM)
  list(APPEND INTEGER_OUTPUTS "${BASE}.metallib")
  list(APPEND INTEGER_LL "${BASE}.ll")
endforeach()
foreach(VARIANT IN ITEMS debug optimized sanitized)
  if(VARIANT STREQUAL "debug")
    set(FLAGS -O0)
  elseif(VARIANT STREQUAL "optimized")
    set(FLAGS -O2)
  else()
    set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  endif()
  set(CPU_BINARY "${OUT}/integer-cpu-${VARIANT}")
  set(GPU_BINARY "${OUT}/integer-gpu-${VARIANT}")
  add_custom_command(OUTPUT "${CPU_BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -Wno-override-module -isystem "${KALANDRA_LLVM_ROOT}/include"
      "-I${CMAKE_CURRENT_SOURCE_DIR}/../include" "${CMAKE_CURRENT_SOURCE_DIR}/IntegerCpu.cpp" ${INTEGER_LL}
      "-L${KALANDRA_LLVM_ROOT}/lib" "-Wl,-rpath,${KALANDRA_LLVM_ROOT}/lib" -lLLVM -o "${CPU_BINARY}"
    DEPENDS IntegerCpu.cpp IntegerOracle.h ${INTEGER_LL} VERBATIM)
  add_custom_command(OUTPUT "${GPU_BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -fobjc-arc -fblocks -fno-objc-msgsend-class-selector-stubs
      -isystem "${KALANDRA_LLVM_ROOT}/include" "-I${CMAKE_CURRENT_SOURCE_DIR}/../include"
      "${CMAKE_CURRENT_SOURCE_DIR}/IntegerGpu.mm" ${INTEGER_LL} -Wno-override-module
      -framework Foundation -framework Metal "-L${KALANDRA_LLVM_ROOT}/lib"
      "-Wl,-rpath,${KALANDRA_LLVM_ROOT}/lib" -lLLVM -o "${GPU_BINARY}"
    DEPENDS IntegerGpu.mm IntegerOracle.h ${INTEGER_LL} VERBATIM)
  list(APPEND INTEGER_OUTPUTS "${CPU_BINARY}" "${GPU_BINARY}")
  if(BUILD_TESTING)
    add_test(NAME "native-integer-compact-${VARIANT}" COMMAND "${CPU_BINARY}")
    add_test(NAME "native-integer-gpu-${VARIANT}" COMMAND "${GPU_BINARY}" "${OUT}" "${OUT}/integer-gpu-${VARIANT}.json")
    set_tests_properties("native-integer-compact-${VARIANT}" "native-integer-gpu-${VARIANT}" PROPERTIES TIMEOUT 180)
    set_tests_properties("native-integer-gpu-${VARIANT}" PROPERTIES RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endif()
endforeach()
add_custom_target(native-metal-integers ALL DEPENDS ${INTEGER_OUTPUTS})
