file(READ "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.json" METAL_LOCK)
foreach(KEY IN ITEMS spirvCrossRoot spirvToolsRoot metalToolchain metalCompilerVersion metalLanguageVersion mslVersion strictMetalLanguageVersion strictMslVersion typedMetalLanguageVersion typedMslVersion)
  string(JSON LOCK_${KEY} GET "${METAL_LOCK}" "${KEY}")
endforeach()
set(KALANDRA_SPIRV_CROSS_ROOT "${LOCK_spirvCrossRoot}" CACHE PATH "Pinned SPIRV-Cross")
set(KALANDRA_SPIRV_TOOLS_ROOT "${LOCK_spirvToolsRoot}" CACHE PATH "Pinned SPIRV-Tools")
set(KALANDRA_METAL_TOOLCHAIN "${LOCK_metalToolchain}" CACHE STRING "Apple Metal toolchain identifier")
set(CROSS "${KALANDRA_SPIRV_CROSS_ROOT}/bin/spirv-cross")
set(VALIDATOR "${KALANDRA_SPIRV_TOOLS_ROOT}/bin/spirv-val")
set(DISASSEMBLER "${KALANDRA_SPIRV_TOOLS_ROOT}/bin/spirv-dis")
foreach(TOOL IN ITEMS "${CROSS}" "${VALIDATOR}" "${DISASSEMBLER}")
  if(NOT EXISTS "${TOOL}")
    message(FATAL_ERROR "Metal proof dependency missing: ${TOOL}; see native/metal-proof/README.md")
  endif()
endforeach()
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MetalToolchain.cmake")
execute_process(COMMAND "${CROSS}" --revision OUTPUT_VARIABLE CROSS_VERSION ERROR_VARIABLE CROSS_ERROR)
execute_process(COMMAND "${VALIDATOR}" --version OUTPUT_VARIABLE VALIDATOR_VERSION)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/versions.txt"
  "LLVM/MLIR/Clang ${KALANDRA_LLVM_VERSION}\n${METAL_VERSION}${CROSS_VERSION}${CROSS_ERROR}${VALIDATOR_VERSION}")
add_executable(kalandra-metal-extract Extract.cpp)
target_include_directories(kalandra-metal-extract SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_link_libraries(kalandra-metal-extract PRIVATE MLIR LLVM)
target_compile_options(kalandra-metal-extract PRIVATE -Wall -Wextra -Werror)
set(MLIR_OPT "${KALANDRA_LLVM_ROOT}/bin/mlir-opt")
set(MLIR_TRANSLATE "${KALANDRA_LLVM_ROOT}/bin/mlir-translate")
set(OUT "${CMAKE_CURRENT_BINARY_DIR}")
add_custom_command(OUTPUT "${OUT}/converted.mlir"
  COMMAND "${MLIR_OPT}" "${CMAKE_CURRENT_SOURCE_DIR}/elementwise.mlir" --verify-each
    "--pass-pipeline=builtin.module(convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))"
    -o "${OUT}/converted.mlir"
  DEPENDS elementwise.mlir toolchain.json VERBATIM)
set(OUTPUTS)
foreach(ROUNDING_POLICY IN ITEMS decorated compiler-rounding)
  set(ROUNDING_OPTION)
  if(ROUNDING_POLICY STREQUAL "compiler-rounding")
    set(ROUNDING_OPTION --metal-compiler-rounding)
  endif()
  set(BASE "${OUT}/${ROUNDING_POLICY}")
  add_custom_command(OUTPUT "${BASE}.metallib" "${BASE}.reflection.json" "${BASE}.spvasm" "${BASE}.metal"
    COMMAND $<TARGET_FILE:kalandra-metal-extract> "${OUT}/converted.mlir" "${BASE}.mlir" ${ROUNDING_OPTION}
    COMMAND "${MLIR_TRANSLATE}" --no-implicit-module --serialize-spirv "${BASE}.mlir" -o "${BASE}.spv"
    COMMAND "${VALIDATOR}" --target-env vulkan1.0 "${BASE}.spv"
    COMMAND "${DISASSEMBLER}" "${BASE}.spv" -o "${BASE}.spvasm"
    COMMAND "${CROSS}" "${BASE}.spv" --reflect --output "${BASE}.reflection.json"
    COMMAND "${CROSS}" "${BASE}.spv" --msl --msl-version "${LOCK_mslVersion}" --output "${BASE}.metal"
    COMMAND "${KALANDRA_METAL_COMPILER}" "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}" -isysroot "${KALANDRA_METAL_SDK}"
      "-std=${LOCK_metalLanguageVersion}" -fno-fast-math -ffp-contract=off
      "-fmodules-cache-path=${OUT}/module-cache" -c "${BASE}.metal" -o "${BASE}.air"
    COMMAND "${KALANDRA_METAL_COMPILER}" "${BASE}.air" -o "${BASE}.metallib"
    DEPENDS "${OUT}/converted.mlir" kalandra-metal-extract toolchain.json
    BYPRODUCTS "${BASE}.mlir" "${BASE}.spv" "${BASE}.air" VERBATIM)
  list(APPEND OUTPUTS "${BASE}.metallib" "${BASE}.reflection.json" "${BASE}.spvasm" "${BASE}.metal")
endforeach()
foreach(VARIANT IN ITEMS debug optimized sanitized)
  if(VARIANT STREQUAL "debug")
    set(FLAGS -O0)
  elseif(VARIANT STREQUAL "optimized")
    set(FLAGS -O2)
  else()
    set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  endif()
  set(BINARY "${OUT}/metal-proof-${VARIANT}")
  add_custom_command(OUTPUT "${BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -fobjc-arc -fblocks -fno-objc-msgsend-class-selector-stubs "${CMAKE_CURRENT_SOURCE_DIR}/Harness.mm"
      -framework Foundation -framework Metal -o "${BINARY}"
    DEPENDS Harness.mm VERBATIM)
  list(APPEND OUTPUTS "${BINARY}")
  if(BUILD_TESTING)
    foreach(ROUNDING_POLICY IN ITEMS decorated compiler-rounding)
      add_test(NAME "metal-proof-${ROUNDING_POLICY}-${VARIANT}" COMMAND "${BINARY}"
        "${OUT}/${ROUNDING_POLICY}.metallib" "${OUT}/${ROUNDING_POLICY}-${VARIANT}.json")
      set_tests_properties("metal-proof-${ROUNDING_POLICY}-${VARIANT}" PROPERTIES
        TIMEOUT 60 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
    endforeach()
  endif()
endforeach()
add_custom_target(native-metal-proof ALL DEPENDS ${OUTPUTS})
if(BUILD_TESTING)
  add_test(NAME metal-proof-artifacts COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${OUT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyArtifacts.cmake")
  add_test(NAME metal-proof-extract-errors COMMAND "${CMAKE_COMMAND}"
    "-DEXTRACTOR=$<TARGET_FILE:kalandra-metal-extract>" "-DOUTPUT_DIR=${OUT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRejected.cmake")
endif()

add_executable(kalandra-metal-strict Strict.cpp)
target_link_libraries(kalandra-metal-strict PRIVATE kalandra-strict-float)
target_compile_options(kalandra-metal-strict PRIVATE -Wall -Wextra -Werror)

add_custom_command(OUTPUT "${OUT}/strict.lowered.mlir" "${OUT}/strict.metallib" "${OUT}/strict.spvasm" "${OUT}/strict.reflection.json" "${OUT}/strict.metal"
  COMMAND $<TARGET_FILE:kalandra-metal-strict> "${CMAKE_CURRENT_SOURCE_DIR}/strict-gpu.mlir" "${OUT}/strict.lowered.mlir"
  COMMAND "${MLIR_OPT}" "${OUT}/strict.lowered.mlir" --verify-each
    "--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))"
    -o "${OUT}/strict.converted.mlir"
  COMMAND $<TARGET_FILE:kalandra-metal-extract> "${OUT}/strict.converted.mlir" "${OUT}/strict.kernel.mlir" --metal-compiler-rounding
  COMMAND "${MLIR_TRANSLATE}" --no-implicit-module --serialize-spirv "${OUT}/strict.kernel.mlir" -o "${OUT}/strict.spv"
  COMMAND "${VALIDATOR}" --target-env vulkan1.0 "${OUT}/strict.spv"
  COMMAND "${DISASSEMBLER}" "${OUT}/strict.spv" -o "${OUT}/strict.spvasm"
  COMMAND "${CROSS}" "${OUT}/strict.spv" --reflect --output "${OUT}/strict.reflection.json"
  COMMAND "${CROSS}" "${OUT}/strict.spv" --msl --msl-version "${LOCK_strictMslVersion}" --output "${OUT}/strict.metal"
  COMMAND "${KALANDRA_METAL_COMPILER}" "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}" -isysroot "${KALANDRA_METAL_SDK}"
    "-std=${LOCK_strictMetalLanguageVersion}" -fno-fast-math -ffp-contract=off "-fmodules-cache-path=${OUT}/module-cache"
    -c "${OUT}/strict.metal" -o "${OUT}/strict.air"
  COMMAND "${KALANDRA_METAL_COMPILER}" "${OUT}/strict.air" -o "${OUT}/strict.metallib"
  DEPENDS strict-gpu.mlir kalandra-metal-strict kalandra-metal-extract toolchain.json
  BYPRODUCTS "${OUT}/strict.converted.mlir" "${OUT}/strict.kernel.mlir" "${OUT}/strict.spv" "${OUT}/strict.air" VERBATIM)
add_custom_command(OUTPUT "${OUT}/runtime.lowered.mlir" "${OUT}/runtime.metallib" "${OUT}/runtime.spvasm" "${OUT}/runtime.reflection.json" "${OUT}/runtime.metal"
  COMMAND $<TARGET_FILE:kalandra-metal-strict> "${CMAKE_CURRENT_SOURCE_DIR}/runtime-gpu.mlir" "${OUT}/runtime.lowered.mlir"
  COMMAND "${MLIR_OPT}" "${OUT}/runtime.lowered.mlir" --verify-each
    "--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))"
    -o "${OUT}/runtime.converted.mlir"
  COMMAND $<TARGET_FILE:kalandra-metal-extract> "${OUT}/runtime.converted.mlir" "${OUT}/runtime.kernel.mlir" --metal-compiler-rounding
  COMMAND "${MLIR_TRANSLATE}" --no-implicit-module --serialize-spirv "${OUT}/runtime.kernel.mlir" -o "${OUT}/runtime.spv"
  COMMAND "${VALIDATOR}" --target-env vulkan1.0 "${OUT}/runtime.spv"
  COMMAND "${DISASSEMBLER}" "${OUT}/runtime.spv" -o "${OUT}/runtime.spvasm"
  COMMAND "${CROSS}" "${OUT}/runtime.spv" --reflect --output "${OUT}/runtime.reflection.json"
  COMMAND "${CROSS}" "${OUT}/runtime.spv" --msl --msl-version "${LOCK_strictMslVersion}" --output "${OUT}/runtime.metal"
  COMMAND "${KALANDRA_METAL_COMPILER}" "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}" -isysroot "${KALANDRA_METAL_SDK}"
    "-std=${LOCK_strictMetalLanguageVersion}" -fno-fast-math -ffp-contract=off "-fmodules-cache-path=${OUT}/module-cache"
    -c "${OUT}/runtime.metal" -o "${OUT}/runtime.air"
  COMMAND "${KALANDRA_METAL_COMPILER}" "${OUT}/runtime.air" -o "${OUT}/runtime.metallib"
  DEPENDS runtime-gpu.mlir kalandra-metal-strict kalandra-metal-extract toolchain.json
  BYPRODUCTS "${OUT}/runtime.converted.mlir" "${OUT}/runtime.kernel.mlir" "${OUT}/runtime.spv" "${OUT}/runtime.air" VERBATIM)
add_custom_target(native-metal-runtime-shader ALL DEPENDS "${OUT}/runtime.metallib")
add_custom_command(OUTPUT "${OUT}/strict-cpu.ll"
  COMMAND $<TARGET_FILE:kalandra-metal-strict> "${CMAKE_CURRENT_SOURCE_DIR}/strict-cpu.mlir" "${OUT}/strict-cpu.lowered.mlir"
  COMMAND "${MLIR_OPT}" "${OUT}/strict-cpu.lowered.mlir" --verify-each --canonicalize --cse
    --convert-scf-to-cf --convert-to-llvm --reconcile-unrealized-casts -o "${OUT}/strict-cpu.llvm.mlir"
  COMMAND "${MLIR_TRANSLATE}" --mlir-to-llvmir "${OUT}/strict-cpu.llvm.mlir" -o "${OUT}/strict-cpu.ll"
  DEPENDS strict-cpu.mlir kalandra-metal-strict
  BYPRODUCTS "${OUT}/strict-cpu.lowered.mlir" "${OUT}/strict-cpu.llvm.mlir" VERBATIM)
set(STRICT_OUTPUTS "${OUT}/strict.metallib" "${OUT}/strict-cpu.ll" "${OUT}/strict.spvasm" "${OUT}/strict.reflection.json" "${OUT}/strict.metal")
foreach(VARIANT IN ITEMS debug optimized sanitized)
  if(VARIANT STREQUAL "debug")
    set(FLAGS -O0)
  elseif(VARIANT STREQUAL "optimized")
    set(FLAGS -O2)
  else()
    set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  endif()
  set(CPU_BINARY "${OUT}/strict-cpu-${VARIANT}")
  set(GPU_BINARY "${OUT}/strict-gpu-${VARIANT}")
  add_custom_command(OUTPUT "${CPU_BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -Wno-override-module -isystem "${KALANDRA_LLVM_ROOT}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/StrictCpu.cpp" "${OUT}/strict-cpu.ll"
      "-L${KALANDRA_LLVM_ROOT}/lib" "-Wl,-rpath,${KALANDRA_LLVM_ROOT}/lib" -lLLVM -o "${CPU_BINARY}"
    DEPENDS StrictCpu.cpp FloatOracle.h "${OUT}/strict-cpu.ll" VERBATIM)
  add_custom_command(OUTPUT "${GPU_BINARY}"
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 -Wall -Wextra -Werror ${FLAGS}
      -ffp-contract=off -fobjc-arc -fblocks -fno-objc-msgsend-class-selector-stubs -isystem "${KALANDRA_LLVM_ROOT}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/StrictGpu.mm" -framework Foundation -framework Metal
      "-L${KALANDRA_LLVM_ROOT}/lib" "-Wl,-rpath,${KALANDRA_LLVM_ROOT}/lib" -lLLVM -o "${GPU_BINARY}"
    DEPENDS StrictGpu.mm FloatOracle.h VERBATIM)
  list(APPEND STRICT_OUTPUTS "${CPU_BINARY}" "${GPU_BINARY}")
  if(BUILD_TESTING)
    add_test(NAME "metal-strict-cpu-${VARIANT}" COMMAND "${CPU_BINARY}")
    add_test(NAME "metal-strict-gpu-${VARIANT}" COMMAND "${GPU_BINARY}" "${OUT}/strict.metallib" "${OUT}/strict-${VARIANT}.json")
    set_tests_properties("metal-strict-cpu-${VARIANT}" "metal-strict-gpu-${VARIANT}" PROPERTIES TIMEOUT 120)
    set_tests_properties("metal-strict-gpu-${VARIANT}" PROPERTIES RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endif()
endforeach()
add_custom_target(native-metal-strict ALL DEPENDS ${STRICT_OUTPUTS})
if(BUILD_TESTING)
  add_test(NAME metal-strict-artifacts COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${OUT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyStrict.cmake")
  add_test(NAME metal-strict-rejected COMMAND "${CMAKE_COMMAND}" "-DLOWERER=$<TARGET_FILE:kalandra-metal-strict>" "-DOUTPUT_DIR=${OUT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyStrictRejected.cmake")
endif()

if(BUILD_TESTING)
  add_test(NAME metal-runtime-artifacts COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${OUT}"
    -DKERNEL_PREFIX=runtime -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyStrict.cmake")
endif()

include(Strict64.cmake)
if(BUILD_TESTING)
  add_test(NAME metal-integer-minmax COMMAND "${CMAKE_COMMAND}"
    "-DEXTRACTOR=$<TARGET_FILE:kalandra-metal-extract>" "-DTRANSLATE=${MLIR_TRANSLATE}"
    "-DVALIDATOR=${VALIDATOR}" "-DOUTPUT_DIR=${OUT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyIntegerMinMax.cmake")
  set_tests_properties(metal-integer-minmax PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME native-metal-toolchain-selection COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/ToolchainSelectionTests.py" "${CMAKE_COMMAND}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MetalToolchain.cmake"
    "${KALANDRA_METAL_COMPILER}" "${LOCK_metalCompilerVersion}")
endif()

include(Strict64Gpu.cmake)

add_executable(kalandra-metal-integer Integer.cpp)
target_link_libraries(kalandra-metal-integer PRIVATE kalandra-gpu-numeric)
target_compile_options(kalandra-metal-integer PRIVATE -Wall -Wextra -Werror)

include(Integer.cmake)

add_executable(kalandra-metal-numeric-wire NumericWire.cpp)
target_link_libraries(kalandra-metal-numeric-wire PRIVATE kalandra-gpu-numeric kalandra-metal-source-backend)
target_compile_options(kalandra-metal-numeric-wire PRIVATE -Wall -Wextra -Werror)

include(NumericWire.cmake)

add_executable(kalandra-metal-conversion Conversion.cpp)
target_link_libraries(kalandra-metal-conversion PRIVATE kalandra-gpu-numeric MLIR LLVM)
target_compile_options(kalandra-metal-conversion PRIVATE -Wall -Wextra -Werror)

include(Conversion.cmake)
