add_library(kalandra-metal-source-backend STATIC Compiler.cpp Build.cpp)
target_link_libraries(kalandra-metal-source-backend PUBLIC kalandra-semantics PRIVATE kalandra-gpu-source-ir kalandra-packages kalandra-backend kalandra-gpu-numeric MLIR LLVM)
target_include_directories(kalandra-metal-source-backend SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_options(kalandra-metal-source-backend PRIVATE -Wall -Wextra -Werror)
add_executable(kalandra-metal-source main.cpp)
target_link_libraries(kalandra-metal-source PRIVATE kalandra-metal-source-backend)
target_compile_options(kalandra-metal-source PRIVATE -Wall -Wextra -Werror)
target_link_libraries(kalandra PRIVATE kalandra-metal-source-backend)
target_compile_definitions(kalandra PRIVATE KALANDRA_HAS_METAL_SOURCE=1)
function(add_source_fixture FIXTURE)
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/${FIXTURE}")
add_custom_command(OUTPUT "${GENERATED}/host.cpp" "${GENERATED}/placement.ir" "${GENERATED}/continuation.plan" "${GENERATED}/kernel0.mlir" "${GENERATED}/manifest.txt"
  COMMAND $<TARGET_FILE:kalandra-metal-source> --test-cpu-exports --arithmetic software "${CMAKE_CURRENT_SOURCE_DIR}/${FIXTURE}.k" -o "${GENERATED}"
  DEPENDS kalandra-metal-source "${FIXTURE}.k" VERBATIM)
# Use the same pinned toolchain and strict integer lowering as the numerical proof.
file(READ "${PROJECT_SOURCE_DIR}/native/metal-proof/toolchain.json" SOURCE_LOCK)
foreach(KEY IN ITEMS spirvCrossRoot spirvToolsRoot metalToolchain strictMetalLanguageVersion strictMslVersion)
  string(JSON SOURCE_${KEY} GET "${SOURCE_LOCK}" "${KEY}")
endforeach()
set(BASE "${GENERATED}/kernel0")
add_custom_command(OUTPUT "${BASE}.metallib" "${BASE}.lowered.mlir" "${BASE}.spvasm" "${BASE}.metal" "${BASE}.reflection.json"
  COMMAND $<TARGET_FILE:kalandra-metal-strict> "${BASE}.mlir" "${BASE}.lowered.mlir"
  COMMAND "${KALANDRA_LLVM_ROOT}/bin/mlir-opt" "${BASE}.lowered.mlir" --verify-each
    "--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))" -o "${BASE}.converted.mlir"
  COMMAND $<TARGET_FILE:kalandra-metal-extract> "${BASE}.converted.mlir" "${BASE}.kernel.mlir" --metal-compiler-rounding
  COMMAND "${KALANDRA_LLVM_ROOT}/bin/mlir-translate" --no-implicit-module --serialize-spirv "${BASE}.kernel.mlir" -o "${BASE}.spv"
  COMMAND "${SOURCE_spirvToolsRoot}/bin/spirv-val" --target-env vulkan1.0 "${BASE}.spv"
  COMMAND "${SOURCE_spirvToolsRoot}/bin/spirv-dis" "${BASE}.spv" -o "${BASE}.spvasm"
  COMMAND "${SOURCE_spirvCrossRoot}/bin/spirv-cross" "${BASE}.spv" --reflect --output "${BASE}.reflection.json"
  COMMAND "${SOURCE_spirvCrossRoot}/bin/spirv-cross" "${BASE}.spv" --msl --msl-version "${SOURCE_strictMslVersion}" --output "${BASE}.metal"
  COMMAND "${KALANDRA_METAL_COMPILER}" "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}" -isysroot "${KALANDRA_METAL_SDK}" "-std=${SOURCE_strictMetalLanguageVersion}" -fno-fast-math -ffp-contract=off
    "-fmodules-cache-path=${CMAKE_CURRENT_BINARY_DIR}/module-cache" -c "${BASE}.metal" -o "${BASE}.air"
  COMMAND "${KALANDRA_METAL_COMPILER}" "${BASE}.air" -o "${BASE}.metallib"
  DEPENDS "${BASE}.mlir" kalandra-metal-strict kalandra-metal-extract "${PROJECT_SOURCE_DIR}/native/metal-proof/toolchain.json"
  BYPRODUCTS "${BASE}.converted.mlir" "${BASE}.kernel.mlir" "${BASE}.spv" "${BASE}.air" VERBATIM)
add_custom_target(native-metal-source-shader-${FIXTURE} ALL DEPENDS "${BASE}.metallib")
if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_executable(kalandra-metal-source-${FIXTURE}-${VARIANT} SourceTests.cpp)
    if(VARIANT STREQUAL "sanitized")
      set(FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(FLAGS -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    elseif(VARIANT STREQUAL "optimized")
      set(FLAGS -O2)
    else()
      set(FLAGS -O0)
    endif()
    target_include_directories(kalandra-metal-source-${FIXTURE}-${VARIANT} PRIVATE ../metal "${GENERATED}")
    target_compile_options(kalandra-metal-source-${FIXTURE}-${VARIANT} PRIVATE -Wall -Wextra -Werror ${FLAGS})
    target_link_options(kalandra-metal-source-${FIXTURE}-${VARIANT} PRIVATE ${FLAGS})
    target_link_libraries(kalandra-metal-source-${FIXTURE}-${VARIANT} PRIVATE kalandra-metal-${VARIANT} LLVM)
    target_include_directories(kalandra-metal-source-${FIXTURE}-${VARIANT} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    if(FIXTURE STREQUAL "composed")
      target_compile_definitions(kalandra-metal-source-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_SOURCE_COMPOSED)
    endif()
    add_dependencies(kalandra-metal-source-${FIXTURE}-${VARIANT} native-metal-source-shader-${FIXTURE})
    add_test(NAME native-metal-source-${FIXTURE}-${VARIANT} COMMAND kalandra-metal-source-${FIXTURE}-${VARIANT} "${GENERATED}")
    set_tests_properties(native-metal-source-${FIXTURE}-${VARIANT} PROPERTIES TIMEOUT 90 RESOURCE_LOCK apple-metal-gpu
      ENVIRONMENT "MTL_DEBUG_LAYER=1" )
  endforeach()
endif()

endfunction()
add_source_fixture(placed)
add_source_fixture(composed)
if(BUILD_TESTING)
  # Keep libc++ container instrumentation consistent across the compiler's
  # translation units, including the TOML parser and numerical lowering.
  add_library(kalandra-metal-source-lowering-sanitized STATIC
    ../lib/Backend.cpp ../lib/CheckedABI.cpp ../lib/TaskBackend.cpp ../lib/TaskEnvironment.cpp
    ../lib/NumericConversion.cpp ../lib/StrictFloat.cpp)
  target_link_libraries(kalandra-metal-source-lowering-sanitized PUBLIC kalandra-semantics-sanitized PRIVATE MLIR LLVM)
  target_include_directories(kalandra-metal-source-lowering-sanitized SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  target_compile_options(kalandra-metal-source-lowering-sanitized PRIVATE -Wall -Wextra -Werror
    -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_executable(kalandra-metal-source-compiler-sanitized main.cpp Compiler.cpp NumericShader.cpp NumericProgram.cpp ../lib/GpuInteger.cpp ../lib/GpuConversion.cpp Build.cpp)
  target_link_libraries(kalandra-metal-source-compiler-sanitized PRIVATE kalandra-semantics-sanitized kalandra-packages-sanitized kalandra-metal-source-lowering-sanitized MLIR LLVM)
  target_include_directories(kalandra-metal-source-compiler-sanitized SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  target_compile_options(kalandra-metal-source-compiler-sanitized PRIVATE -Wall -Wextra -Werror -O1 -g
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(kalandra-metal-source-compiler-sanitized PRIVATE -fsanitize=address,undefined)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-diagnostics COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>"
      "-DCPU_CHECKER=$<TARGET_FILE:kalandra-check>" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-checks"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRejected.cmake")
    set_tests_properties(${COMPILER_TARGET}-diagnostics PROPERTIES TIMEOUT 90)
  endforeach()
endif()
if(BUILD_TESTING)
  add_test(NAME native-metal-source-artifacts COMMAND "${CMAKE_COMMAND}"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyArtifacts.cmake")
endif()

file(READ "${PROJECT_SOURCE_DIR}/native/metal-proof/toolchain.json" NUMERIC_SOURCE_LOCK)
string(JSON NUMERIC_MSL_VERSION GET "${NUMERIC_SOURCE_LOCK}" typedMslVersion)
string(JSON NUMERIC_METAL_LANGUAGE GET "${NUMERIC_SOURCE_LOCK}" typedMetalLanguageVersion)
string(JSON NUMERIC_METAL_VERSION GET "${NUMERIC_SOURCE_LOCK}" metalCompilerVersion)
set(TOOLCHAIN_HEADER "#pragma once\n")
foreach(PAIR IN ITEMS
    "CLANG|${CMAKE_CXX_COMPILER}" "OPT|${KALANDRA_LLVM_ROOT}/bin/mlir-opt"
    "TRANSLATE|${KALANDRA_LLVM_ROOT}/bin/mlir-translate"
    "STRICT|$<TARGET_FILE:kalandra-metal-strict>" "STRICT64|$<TARGET_FILE:kalandra-metal-strict64>"
    "TYPED_MSL_VERSION|${NUMERIC_MSL_VERSION}" "TYPED_METAL_LANGUAGE|${NUMERIC_METAL_LANGUAGE}" "EXTRACT|$<TARGET_FILE:kalandra-metal-extract>"
    "CROSS|${KALANDRA_SPIRV_CROSS_ROOT}/bin/spirv-cross" "VALIDATE|${KALANDRA_SPIRV_TOOLS_ROOT}/bin/spirv-val"
    "METAL_TOOLCHAIN|${KALANDRA_METAL_TOOLCHAIN}" "METAL_VERSION|${NUMERIC_METAL_VERSION}" "METAL_COMPILER|${KALANDRA_METAL_COMPILER}" "METAL_SDK|${KALANDRA_METAL_SDK}" "INCLUDE|${PROJECT_SOURCE_DIR}/native/metal"
    "METAL_DEBUG|$<TARGET_FILE:kalandra-metal-debug>" "CORE_DEBUG|$<TARGET_FILE:kalandra-native-runtime>"
    "METAL_OPTIMIZED|$<TARGET_FILE:kalandra-metal-optimized>" "CORE_OPTIMIZED|$<TARGET_FILE:kalandra-task-core-optimized>"
    "METAL_SANITIZED|$<TARGET_FILE:kalandra-metal-sanitized>" "CORE_SANITIZED|$<TARGET_FILE:kalandra-native-runtime-sanitized>"
    "METAL_THREAD_SANITIZED|$<TARGET_FILE:kalandra-metal-thread-sanitized>" "CORE_THREAD_SANITIZED|$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
  string(REPLACE "|" ";" PARTS "${PAIR}")
  list(GET PARTS 0 KEY)
  list(GET PARTS 1 VALUE)
  if(KEY MATCHES "^(STRICT|STRICT64|EXTRACT|METAL_(DEBUG|OPTIMIZED|SANITIZED|THREAD_SANITIZED)|CORE_(DEBUG|OPTIMIZED|SANITIZED|THREAD_SANITIZED))$")
    string(REPLACE "$<TARGET_FILE:" "$<TARGET_FILE_NAME:" INSTALLED_VALUE "${VALUE}")
  elseif(KEY STREQUAL "INCLUDE")
    set(INSTALLED_VALUE "headers/metal")
  else()
    set(INSTALLED_VALUE "${VALUE}")
  endif()
  string(APPEND TOOLCHAIN_HEADER "#ifdef KALANDRA_INSTALLED_COMPILER\n#define KALANDRA_GPU_${KEY} \"${INSTALLED_VALUE}\"\n#else\n#define KALANDRA_GPU_${KEY} \"${VALUE}\"\n#endif\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Toolchain.h" CONTENT "${TOOLCHAIN_HEADER}")
target_include_directories(kalandra-metal-source-backend PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(kalandra-metal-source-backend kalandra-metal-strict kalandra-metal-strict64 kalandra-metal-extract
  kalandra-metal-debug kalandra-metal-optimized kalandra-metal-sanitized kalandra-metal-thread-sanitized)
if(BUILD_TESTING)
  target_include_directories(kalandra-metal-source-compiler-sanitized PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(BUILD_TESTING)
  add_test(NAME native-metal-source-cli-build COMMAND "${CMAKE_COMMAND}"
    "-DCOMPILER=$<TARGET_FILE:kalandra-metal-source>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyBuild.cmake")
  set_tests_properties(native-metal-source-cli-build PROPERTIES TIMEOUT 120 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    set(CPU_FLAGS)
    if(VARIANT STREQUAL "sanitized")
      set(CPU_OPTION --sanitize)
      set(CPU_FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(CPU_OPTION --thread-sanitize)
      set(CPU_FLAGS -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    elseif(VARIANT STREQUAL "optimized")
      set(CPU_OPTION -O2)
      set(CPU_FLAGS -O2)
    else()
      set(CPU_OPTION -O0)
      set(CPU_FLAGS -O0)
    endif()
    set(CPU_BASE "${CMAKE_CURRENT_BINARY_DIR}/cpu-consumer-${VARIANT}")
    add_custom_command(OUTPUT "${CPU_BASE}.o" "${CPU_BASE}.ll"
      COMMAND $<TARGET_FILE:kalandra> "${CMAKE_CURRENT_SOURCE_DIR}/cpu-consumer.k"
        -o "${CPU_BASE}" --emit-llvm "${CPU_BASE}.ll" --test-entry total ${CPU_OPTION}
      COMMAND "${CMAKE_CXX_COMPILER}" -Wno-override-module -ffp-contract=off ${CPU_FLAGS}
        -c "${CPU_BASE}.ll" -o "${CPU_BASE}.o"
      DEPENDS kalandra cpu-consumer.k VERBATIM)
    set_source_files_properties("${CPU_BASE}.o" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    add_executable(kalandra-metal-interop-${VARIANT} InteropTests.cpp "${CPU_BASE}.o")
    target_include_directories(kalandra-metal-interop-${VARIANT} PRIVATE ../metal)
    target_compile_options(kalandra-metal-interop-${VARIANT} PRIVATE -Wall -Wextra -Werror -ffp-contract=off ${CPU_FLAGS})
    target_link_options(kalandra-metal-interop-${VARIANT} PRIVATE ${CPU_FLAGS})
    target_link_libraries(kalandra-metal-interop-${VARIANT} PRIVATE kalandra-metal-${VARIANT})
    add_dependencies(kalandra-metal-interop-${VARIANT} native-metal-source-shader-placed)
    add_test(NAME native-metal-interop-${VARIANT} COMMAND kalandra-metal-interop-${VARIANT} "${CMAKE_CURRENT_BINARY_DIR}/placed")
    set_tests_properties(native-metal-interop-${VARIANT} PROPERTIES TIMEOUT 60 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
  add_test(NAME native-unified-metal-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/unified-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUnified.cmake")
  set_tests_properties(native-unified-metal-cli PROPERTIES TIMEOUT 60 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  foreach(FIXTURE IN ITEMS mixed tensor-results shared-helpers scalar-entry matrix-elementwise differences arguments unused-inputs indirect-arguments tensor-negation division host-expressions cpu-spawn nested-parallel recovery resumable-helpers spawned-helpers reduction pre-reduction post-reduction matmul post-matmul pre-matmul matmul-reduction matmul-adjust-reduction matmul-scalar-reduction matrix-graph matrix-graph-reduction matrix-graph-arithmetic multiple-reductions reduction-feedback tensor-feedback scalar-control tensor-control tensor-sources dynamic-choices nary-choices)
    if(FIXTURE STREQUAL "mixed")
      set(HARNESS MixedTests.cpp)
    elseif(FIXTURE STREQUAL "spawned-helpers")
      set(HARNESS SpawnedHelperTests.cpp)
    elseif(FIXTURE STREQUAL "recovery" OR FIXTURE STREQUAL "resumable-helpers")
      set(HARNESS RecoveryTests.cpp)
    elseif(FIXTURE STREQUAL "nested-parallel")
      set(HARNESS NestedParallelTests.cpp)
    elseif(FIXTURE STREQUAL "cpu-spawn")
      set(HARNESS CpuSpawnTests.cpp)
    elseif(FIXTURE STREQUAL "host-expressions")
      set(HARNESS HostExpressionTests.cpp)
    elseif(FIXTURE STREQUAL "post-matmul" OR FIXTURE STREQUAL "pre-matmul")
      set(HARNESS MatmulPostTests.cpp)
    elseif(FIXTURE STREQUAL "matmul-reduction" OR FIXTURE STREQUAL "matmul-adjust-reduction" OR FIXTURE STREQUAL "matmul-scalar-reduction")
      set(HARNESS MatmulReductionTests.cpp)
    elseif(FIXTURE STREQUAL "nary-choices")
      set(HARNESS NaryChoiceTests.cpp)
    elseif(FIXTURE STREQUAL "dynamic-choices")
      set(HARNESS DynamicChoiceTests.cpp)
    elseif(FIXTURE STREQUAL "tensor-sources")
      set(HARNESS TensorSourceTests.cpp)
    elseif(FIXTURE STREQUAL "tensor-control")
      set(HARNESS TensorControlTests.cpp)
    elseif(FIXTURE STREQUAL "scalar-control")
      set(HARNESS ScalarControlTests.cpp)
    elseif(FIXTURE STREQUAL "tensor-feedback")
      set(HARNESS TensorFeedbackTests.cpp)
    elseif(FIXTURE STREQUAL "reduction-feedback")
      set(HARNESS ReductionFeedbackTests.cpp)
    elseif(FIXTURE STREQUAL "multiple-reductions")
      set(HARNESS MultipleReductionTests.cpp)
    elseif(FIXTURE STREQUAL "matrix-graph-arithmetic")
      set(HARNESS MatrixGraphArithmeticTests.cpp)
    elseif(FIXTURE STREQUAL "matrix-graph-reduction")
      set(HARNESS MatrixGraphReductionTests.cpp)
    elseif(FIXTURE STREQUAL "matrix-graph")
      set(HARNESS MatrixGraphTests.cpp)
    elseif(FIXTURE STREQUAL "matmul")
      set(HARNESS MatmulTests.cpp)
    elseif(FIXTURE STREQUAL "pre-reduction" OR FIXTURE STREQUAL "post-reduction")
      set(HARNESS PreReductionTests.cpp)
    elseif(FIXTURE STREQUAL "reduction")
      set(HARNESS ReductionTests.cpp)
    else()
      set(HARNESS TensorResultTests.cpp)
    endif()
    foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
      set(MIXED_OPTION)
      set(MIXED_FLAGS -O0)
      if(VARIANT STREQUAL "sanitized")
        set(MIXED_OPTION --sanitize)
        set(MIXED_FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      elseif(VARIANT STREQUAL "thread-sanitized")
        set(MIXED_OPTION --thread-sanitize)
        set(MIXED_FLAGS -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
      elseif(VARIANT STREQUAL "optimized")
        set(MIXED_OPTION -O2)
        set(MIXED_FLAGS -O2)
      endif()
      set(MIXED_DIR "${CMAKE_CURRENT_BINARY_DIR}/${FIXTURE}-${VARIANT}")
      add_custom_command(OUTPUT "${MIXED_DIR}/host.cpp" "${MIXED_DIR}/placement.ir" "${MIXED_DIR}/continuation.plan" "${MIXED_DIR}/cpu.ll" "${MIXED_DIR}/cpu.o" "${MIXED_DIR}/kernel0.metallib"
        COMMAND $<TARGET_FILE:kalandra-metal-source> --test-cpu-exports --arithmetic software "${CMAKE_CURRENT_SOURCE_DIR}/${FIXTURE}.k" -o "${MIXED_DIR}" --build ${MIXED_OPTION}
        COMMAND "${CMAKE_CXX_COMPILER}" -Wno-override-module -ffp-contract=off ${MIXED_FLAGS} -c "${MIXED_DIR}/cpu.ll" -o "${MIXED_DIR}/cpu.o"
        DEPENDS kalandra-metal-source "${FIXTURE}.k" VERBATIM)
      set_source_files_properties("${MIXED_DIR}/cpu.o" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
      add_executable(kalandra-metal-${FIXTURE}-${VARIANT} ${HARNESS} "${MIXED_DIR}/cpu.o" "${MIXED_DIR}/host.cpp")
      set_source_files_properties("${MIXED_DIR}/host.cpp" PROPERTIES HEADER_FILE_ONLY TRUE)
      target_include_directories(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE ../metal "${MIXED_DIR}")
      target_compile_options(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE -Wall -Wextra -Werror -ffp-contract=off ${MIXED_FLAGS})
      target_link_options(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE ${MIXED_FLAGS})
      target_link_libraries(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE kalandra-metal-${VARIANT})
      if(FIXTURE STREQUAL "matmul-scalar-reduction")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_MATRIX_SCALAR)
      endif()
      if(FIXTURE STREQUAL "matmul-adjust-reduction" OR FIXTURE STREQUAL "matmul-scalar-reduction")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_MATRIX_ADJUSTMENT)
      endif()
      if(FIXTURE STREQUAL "pre-matmul")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_PRE_MATMUL)
      endif()
      if(FIXTURE STREQUAL "post-reduction")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_POST_REDUCTION)
      endif()
      if(FIXTURE STREQUAL "division")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_DIVISION)
      endif()
      if(FIXTURE STREQUAL "tensor-negation")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_TENSOR_NEGATION)
      endif()
      if(FIXTURE STREQUAL "indirect-arguments")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_INDIRECT_ARGUMENTS)
      endif()
      if(FIXTURE STREQUAL "unused-inputs")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_UNUSED_INPUTS)
      endif()
      if(FIXTURE STREQUAL "arguments")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_ARGUMENTS)
      endif()
      if(FIXTURE STREQUAL "differences")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_DIFFERENCES)
      endif()
      if(FIXTURE STREQUAL "matrix-elementwise")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_MATRIX_ENTRY)
      endif()
      if(FIXTURE STREQUAL "scalar-entry")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_SCALAR_ENTRY)
      endif()
      if(FIXTURE STREQUAL "shared-helpers")
        target_compile_definitions(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE KALANDRA_SHARED_HELPERS)
      endif()
      if(FIXTURE STREQUAL "shared-helpers" OR FIXTURE STREQUAL "scalar-entry" OR FIXTURE STREQUAL "matrix-elementwise" OR FIXTURE STREQUAL "differences" OR FIXTURE STREQUAL "arguments" OR FIXTURE STREQUAL "unused-inputs" OR FIXTURE STREQUAL "indirect-arguments" OR FIXTURE STREQUAL "tensor-negation" OR FIXTURE STREQUAL "division" OR FIXTURE STREQUAL "reduction" OR FIXTURE STREQUAL "pre-reduction" OR FIXTURE STREQUAL "post-reduction" OR FIXTURE STREQUAL "matmul" OR FIXTURE STREQUAL "post-matmul" OR FIXTURE STREQUAL "pre-matmul" OR FIXTURE STREQUAL "matmul-reduction" OR FIXTURE STREQUAL "matmul-adjust-reduction" OR FIXTURE STREQUAL "matmul-scalar-reduction" OR FIXTURE STREQUAL "matrix-graph" OR FIXTURE STREQUAL "matrix-graph-reduction" OR FIXTURE STREQUAL "matrix-graph-arithmetic" OR FIXTURE STREQUAL "multiple-reductions" OR FIXTURE STREQUAL "reduction-feedback" OR FIXTURE STREQUAL "tensor-feedback" OR FIXTURE STREQUAL "scalar-control" OR FIXTURE STREQUAL "tensor-control" OR FIXTURE STREQUAL "tensor-sources" OR FIXTURE STREQUAL "dynamic-choices" OR FIXTURE STREQUAL "nary-choices")
        target_link_libraries(kalandra-metal-${FIXTURE}-${VARIANT} PRIVATE LLVM)
        target_include_directories(kalandra-metal-${FIXTURE}-${VARIANT} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
      endif()
      add_test(NAME native-metal-${FIXTURE}-${VARIANT} COMMAND kalandra-metal-${FIXTURE}-${VARIANT} "${MIXED_DIR}")
      set_tests_properties(native-metal-${FIXTURE}-${VARIANT} PROPERTIES TIMEOUT 60 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
      if(FIXTURE STREQUAL "matmul" OR FIXTURE STREQUAL "indirect-arguments")
        set_tests_properties(native-metal-${FIXTURE}-${VARIANT} PROPERTIES TIMEOUT 180)
      endif()
    endforeach()
  endforeach()
endif()
if(BUILD_TESTING)
  add_test(NAME native-metal-mixed-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/mixed-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMixed.cmake")
  set_tests_properties(native-metal-mixed-cli PROPERTIES TIMEOUT 90 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()
if(BUILD_TESTING)
  add_test(NAME native-metal-tensor-results-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/tensor-results-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorResults.cmake")
  set_tests_properties(native-metal-tensor-results-cli PROPERTIES TIMEOUT 90 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()
if(BUILD_TESTING)
  add_test(NAME native-shared-helpers-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/shared-helpers-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyShared.cmake")
  set_tests_properties(native-shared-helpers-cli PROPERTIES TIMEOUT 90 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-scalar-entry-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/scalar-entry-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyScalarEntry.cmake")
  set_tests_properties(native-scalar-entry-cli PROPERTIES TIMEOUT 90 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-reduction-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/reduction-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyReduction.cmake")
  set_tests_properties(native-reduction-cli PROPERTIES TIMEOUT 90 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-matmul-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matmul-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatmul.cmake")
  set_tests_properties(native-matmul-cli PROPERTIES TIMEOUT 90 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()
if(BUILD_TESTING)
  add_test(NAME native-matrix-elementwise-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matrix-elementwise-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatrixElementwise.cmake")
  set_tests_properties(native-matrix-elementwise-cli PROPERTIES TIMEOUT 120 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-differences-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/differences-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDifferences.cmake")
  set_tests_properties(native-differences-cli PROPERTIES TIMEOUT 180 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-arguments-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/arguments-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyArguments.cmake")
  set_tests_properties(native-arguments-cli PROPERTIES TIMEOUT 180 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-unused-inputs-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/unused-inputs-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyUnusedInputs.cmake")
  set_tests_properties(native-unused-inputs-cli PROPERTIES TIMEOUT 180 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-indirect-arguments-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/indirect-arguments-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyIndirectArguments.cmake")
  set_tests_properties(native-indirect-arguments-cli PROPERTIES TIMEOUT 180 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-tensor-negation-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/tensor-negation-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorNegation.cmake")
  set_tests_properties(native-tensor-negation-cli PROPERTIES TIMEOUT 180 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-division-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/division-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDivision.cmake")
  set_tests_properties(native-division-cli PROPERTIES TIMEOUT 180 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-host-expressions-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/host-expressions-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyHostExpressions.cmake")
  set_tests_properties(native-host-expressions-cli PROPERTIES TIMEOUT 240 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-cpu-spawn-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cpu-spawn-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyCpuSpawn.cmake")
  set_tests_properties(native-cpu-spawn-cli PROPERTIES TIMEOUT 240 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-nested-parallel-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/nested-parallel-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyNestedParallel.cmake")
  set_tests_properties(native-nested-parallel-cli PROPERTIES TIMEOUT 240 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-recovery-cli COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recovery-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRecovery.cmake")
  set_tests_properties(native-recovery-cli PROPERTIES TIMEOUT 240 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-shared-placement-ir COMMAND "${CMAKE_COMMAND}" "-DCHECKER=$<TARGET_FILE:kalandra-check>"
    "-DCOMPILER=$<TARGET_FILE:kalandra-metal-source>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPlacement.cmake")
  set_tests_properties(native-shared-placement-ir PROPERTIES TIMEOUT 90)
endif()

if(BUILD_TESTING)
  add_test(NAME native-ir-supervisor-cli COMMAND "${CMAKE_COMMAND}"
    "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DSANITIZED_COMPILER=$<TARGET_FILE:kalandra-metal-source-compiler-sanitized>"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/ir-supervisor.k"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/ir-supervisor-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyIRSupervisor.cmake")
  set_tests_properties(native-ir-supervisor-cli PROPERTIES TIMEOUT 240 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-resumable-helpers-cli COMMAND "${CMAKE_COMMAND}"
    "-DNORMAL_COMPILER=$<TARGET_FILE:kalandra-metal-source>"
    "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DCHECKER=$<TARGET_FILE:kalandra-check>"
    "-DSANITIZED_COMPILER=$<TARGET_FILE:kalandra-metal-source-compiler-sanitized>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/resumable-helpers-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyResumableHelpers.cmake")
  set_tests_properties(native-resumable-helpers-cli PROPERTIES TIMEOUT 240 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-spawned-helpers-cli COMMAND "${CMAKE_COMMAND}"
    "-DCOMPILER=$<TARGET_FILE:kalandra>"
    "-DNORMAL_COMPILER=$<TARGET_FILE:kalandra-metal-source>"
    "-DCHECKER=$<TARGET_FILE:kalandra-check>"
    "-DSANITIZED_COMPILER=$<TARGET_FILE:kalandra-metal-source-compiler-sanitized>"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/spawned-helpers-cli"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifySpawnedHelpers.cmake")
  set_tests_properties(native-spawned-helpers-cli PROPERTIES TIMEOUT 240 RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-modules COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DCHECKER=$<TARGET_FILE:kalandra-check>"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-modules"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyModules.cmake")
    set_tests_properties(${COMPILER_TARGET}-modules PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-modules-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/modules-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyModules.cmake")
    set_tests_properties(native-metal-modules-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-fixed-shapes COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-fixed-shapes"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyFixedShapes.cmake")
    set_tests_properties(${COMPILER_TARGET}-fixed-shapes PROPERTIES TIMEOUT 90)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-fixed-shapes-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fixed-shapes-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyFixedShapes.cmake")
    set_tests_properties(native-metal-fixed-shapes-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-forwarding COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-forwarding"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyForwarding.cmake")
    set_tests_properties(${COMPILER_TARGET}-forwarding PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-forwarding-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/forwarding-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyForwarding.cmake")
    set_tests_properties(native-metal-forwarding-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-capabilities COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/CapabilityTests.py"
      --compiler "$<TARGET_FILE:${COMPILER_TARGET}>" --checker "$<TARGET_FILE:kalandra-check>"
      --root "${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-capabilities")
    set_tests_properties(${COMPILER_TARGET}-capabilities PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-pre-reduction COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-pre-reduction"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPreReduction.cmake")
    set_tests_properties(${COMPILER_TARGET}-pre-reduction PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-pre-reduction-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/pre-reduction-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPreReduction.cmake")
    set_tests_properties(native-metal-pre-reduction-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-post-reduction COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" -DPOST=ON "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-post-reduction"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPreReduction.cmake")
    set_tests_properties(${COMPILER_TARGET}-post-reduction PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-post-reduction-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}" -DPOST=ON
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/post-reduction-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPreReduction.cmake")
    set_tests_properties(native-metal-post-reduction-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-post-matmul COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-post-matmul"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPostMatmul.cmake")
    set_tests_properties(${COMPILER_TARGET}-post-matmul PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-post-matmul-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/post-matmul-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPostMatmul.cmake")
    set_tests_properties(native-metal-post-matmul-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-pre-matmul COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-pre-matmul"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPreMatmul.cmake")
    set_tests_properties(${COMPILER_TARGET}-pre-matmul PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-pre-matmul-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/pre-matmul-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyPreMatmul.cmake")
    set_tests_properties(native-metal-pre-matmul-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-matmul-reduction COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-matmul-reduction"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatmulReduction.cmake")
    set_tests_properties(${COMPILER_TARGET}-matmul-reduction PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-matmul-reduction-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matmul-reduction-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatmulReduction.cmake")
    set_tests_properties(native-metal-matmul-reduction-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-matmul-adjust-reduction COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-matmul-adjust-reduction"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatmulAdjustmentReduction.cmake")
    set_tests_properties(${COMPILER_TARGET}-matmul-adjust-reduction PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-matmul-adjust-reduction-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matmul-adjust-reduction-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatmulAdjustmentReduction.cmake")
    set_tests_properties(native-metal-matmul-adjust-reduction-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-matmul-scalar-reduction COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-matmul-scalar-reduction"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatmulScalarReduction.cmake")
    set_tests_properties(${COMPILER_TARGET}-matmul-scalar-reduction PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-matmul-scalar-reduction-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matmul-scalar-reduction-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatmulScalarReduction.cmake")
    set_tests_properties(native-metal-matmul-scalar-reduction-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-matrix-graph COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-matrix-graph"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatrixGraph.cmake")
    set_tests_properties(${COMPILER_TARGET}-matrix-graph PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-matrix-graph-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matrix-graph-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatrixGraph.cmake")
    set_tests_properties(native-metal-matrix-graph-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-matrix-graph-reduction COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-matrix-graph-reduction"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatrixGraphReduction.cmake")
    set_tests_properties(${COMPILER_TARGET}-matrix-graph-reduction PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-matrix-graph-reduction-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matrix-graph-reduction-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatrixGraphReduction.cmake")
    set_tests_properties(native-metal-matrix-graph-reduction-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-matrix-graph-arithmetic COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-matrix-graph-arithmetic"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatrixGraphArithmetic.cmake")
    set_tests_properties(${COMPILER_TARGET}-matrix-graph-arithmetic PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-matrix-graph-arithmetic-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/matrix-graph-arithmetic-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMatrixGraphArithmetic.cmake")
    set_tests_properties(native-metal-matrix-graph-arithmetic-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-multiple-reductions COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-multiple-reductions"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMultipleReductions.cmake")
    set_tests_properties(${COMPILER_TARGET}-multiple-reductions PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-multiple-reductions-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/multiple-reductions-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyMultipleReductions.cmake")
    set_tests_properties(native-metal-multiple-reductions-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-reduction-feedback COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-reduction-feedback"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyReductionFeedback.cmake")
    set_tests_properties(${COMPILER_TARGET}-reduction-feedback PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-reduction-feedback-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/reduction-feedback-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyReductionFeedback.cmake")
    set_tests_properties(native-metal-reduction-feedback-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-tensor-feedback COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-tensor-feedback"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorFeedback.cmake")
    set_tests_properties(${COMPILER_TARGET}-tensor-feedback PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-tensor-feedback-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/tensor-feedback-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorFeedback.cmake")
    set_tests_properties(native-metal-tensor-feedback-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-scalar-control COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-scalar-control"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyScalarControl.cmake")
    set_tests_properties(${COMPILER_TARGET}-scalar-control PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-scalar-control-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/scalar-control-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyScalarControl.cmake")
    set_tests_properties(native-metal-scalar-control-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-tensor-control COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-tensor-control"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorControl.cmake")
    set_tests_properties(${COMPILER_TARGET}-tensor-control PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-tensor-control-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/tensor-control-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorControl.cmake")
    set_tests_properties(native-metal-tensor-control-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-tensor-sources COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-tensor-sources"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorSources.cmake")
    set_tests_properties(${COMPILER_TARGET}-tensor-sources PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-tensor-sources-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/tensor-sources-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyTensorSources.cmake")
    set_tests_properties(native-metal-tensor-sources-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-dynamic-choices COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-dynamic-choices"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDynamicChoices.cmake")
    set_tests_properties(${COMPILER_TARGET}-dynamic-choices PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-dynamic-choices-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/dynamic-choices-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDynamicChoices.cmake")
    set_tests_properties(native-metal-dynamic-choices-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-nary-choices COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-nary-choices"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyNaryChoices.cmake")
    set_tests_properties(${COMPILER_TARGET}-nary-choices PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-nary-choices-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/nary-choices-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyNaryChoices.cmake")
    set_tests_properties(native-metal-nary-choices-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-operand-choices COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-operand-choices"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyOperandChoices.cmake")
    set_tests_properties(${COMPILER_TARGET}-operand-choices PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-operand-choices-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/operand-choices-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyOperandChoices.cmake")
    set_tests_properties(native-metal-operand-choices-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-result-choices COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-result-choices"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyResultChoices.cmake")
    set_tests_properties(${COMPILER_TARGET}-result-choices PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-result-choices-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/result-choices-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyResultChoices.cmake")
    set_tests_properties(native-metal-result-choices-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-reduction-choices COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-reduction-choices"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyReductionChoices.cmake")
    set_tests_properties(${COMPILER_TARGET}-reduction-choices PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-reduction-choices-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/reduction-choices-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyReductionChoices.cmake")
    set_tests_properties(native-metal-reduction-choices-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-host-control COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:${COMPILER_TARGET}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-host-control"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyHostControl.cmake")
    set_tests_properties(${COMPILER_TARGET}-host-control PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-host-control-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/host-control-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyHostControl.cmake")
    set_tests_properties(native-metal-host-control-cli-${VARIANT} PROPERTIES TIMEOUT 300
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-higher-ranks COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/HigherRankTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-higher-ranks")
    set_tests_properties(${COMPILER_TARGET}-higher-ranks PROPERTIES TIMEOUT 180)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-higher-ranks-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/HigherRankTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/higher-ranks-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-higher-ranks-cli-${VARIANT} PROPERTIES TIMEOUT 600
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-wide-host COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/WideHostTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-wide-host")
    set_tests_properties(${COMPILER_TARGET}-wide-host PROPERTIES TIMEOUT 180)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-wide-host-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/WideHostTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/wide-host-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-wide-host-cli-${VARIANT} PROPERTIES TIMEOUT 600
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-maps COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericSourceTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-maps")
    set_tests_properties(${COMPILER_TARGET}-numeric-maps PROPERTIES TIMEOUT 240)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-maps-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericSourceTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-maps-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-maps-cli-${VARIANT} PROPERTIES TIMEOUT 900
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-reductions COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericReductionTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-reductions")
    set_tests_properties(${COMPILER_TARGET}-numeric-reductions PROPERTIES TIMEOUT 240)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-reductions-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericReductionTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-reductions-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-reductions-cli-${VARIANT} PROPERTIES TIMEOUT 900
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-matrices COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericMatmulTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-matrices")
    set_tests_properties(${COMPILER_TARGET}-numeric-matrices PROPERTIES TIMEOUT 240)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-matrices-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericMatmulTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-matrices-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-matrices-cli-${VARIANT} PROPERTIES TIMEOUT 1200
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-scalars COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericScalarTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-scalars")
    set_tests_properties(${COMPILER_TARGET}-numeric-scalars PROPERTIES TIMEOUT 240)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-scalars-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericScalarTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-scalars-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-scalars-cli-${VARIANT} PROPERTIES TIMEOUT 900
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-conversions COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericConversionTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-conversions")
    set_tests_properties(${COMPILER_TARGET}-numeric-conversions PROPERTIES TIMEOUT 240)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-conversions-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericConversionTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-conversions-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-conversions-cli-${VARIANT} PROPERTIES TIMEOUT 1200
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-tier2 COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericTier2Tests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-tier2")
    set_tests_properties(${COMPILER_TARGET}-numeric-tier2 PROPERTIES TIMEOUT 240)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-tier2-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericTier2Tests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-tier2-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-tier2-cli-${VARIANT} PROPERTIES TIMEOUT 1200
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-programs COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericProgramTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-programs")
    set_tests_properties(${COMPILER_TARGET}-numeric-programs PROPERTIES TIMEOUT 600)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-programs-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericProgramTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-programs-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-programs-cli-${VARIANT} PROPERTIES TIMEOUT 1800
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-matrix-programs COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericMatrixProgramTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-matrix-programs")
    set_tests_properties(${COMPILER_TARGET}-numeric-matrix-programs PROPERTIES TIMEOUT 600)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-matrix-programs-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericMatrixProgramTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-matrix-programs-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-matrix-programs-cli-${VARIANT} PROPERTIES TIMEOUT 1800
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(COMPILER_TARGET IN ITEMS kalandra-metal-source kalandra-metal-source-compiler-sanitized)
    add_test(NAME ${COMPILER_TARGET}-numeric-matrix-control COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericMatrixControlTests.py" "--compiler=$<TARGET_FILE:${COMPILER_TARGET}>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/${COMPILER_TARGET}-numeric-matrix-control")
    set_tests_properties(${COMPILER_TARGET}-numeric-matrix-control PROPERTIES TIMEOUT 600)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-metal-numeric-matrix-control-cli-${VARIANT} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/NumericMatrixControlTests.py" "--compiler=$<TARGET_FILE:kalandra>"
      "--root=${CMAKE_CURRENT_BINARY_DIR}/numeric-matrix-control-cli-${VARIANT}" "--variant=${VARIANT}")
    set_tests_properties(native-metal-numeric-matrix-control-cli-${VARIANT} PROPERTIES TIMEOUT 1800
      RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
  endforeach()
endif()

if(BUILD_TESTING)
  add_test(NAME native-metal-arithmetic-policy
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/ArithmeticPolicyTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/arithmetic-policy" --gpu)
  set_tests_properties(native-metal-arithmetic-policy PROPERTIES TIMEOUT 300 RESOURCE_LOCK apple-metal-gpu)
endif()

if(BUILD_TESTING)
  add_test(NAME native-metal-pipeline
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/PipelineTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/pipeline")
  set_tests_properties(native-metal-pipeline PROPERTIES TIMEOUT 600
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  add_test(NAME native-metal-relu
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/ReluTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/relu" --gpu --gpu-only)
  set_tests_properties(native-metal-relu PROPERTIES TIMEOUT 600
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  add_test(NAME native-metal-gpu-optimization
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/GpuOptimizationTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/gpu-optimization")
  set_tests_properties(native-metal-gpu-optimization PROPERTIES TIMEOUT 900
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  add_test(NAME native-metal-steel-matmul
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/SteelMatmulTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/steel-matmul")
  set_tests_properties(native-metal-steel-matmul PROPERTIES TIMEOUT 1800
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
endif()

if(BUILD_TESTING)
  add_test(NAME native-metal-graph-batching
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/GraphBatchTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/graph-batching")
  set_tests_properties(native-metal-graph-batching PROPERTIES TIMEOUT 900
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  add_test(NAME native-metal-work-proofs
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/NumericWorkProofTests.py"
      --compiler "$<TARGET_FILE:kalandra>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/work-proofs")
  set_tests_properties(native-metal-work-proofs PROPERTIES TIMEOUT 900)
  add_test(NAME native-metal-prepared-programs
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/PreparedNumericProgramTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/prepared-programs")
  set_tests_properties(native-metal-prepared-programs PROPERTIES TIMEOUT 1200
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  add_test(NAME native-metal-encoding-overlap
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/EncodingOverlapTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/encoding-overlap")
  set_tests_properties(native-metal-encoding-overlap PROPERTIES TIMEOUT 1200
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  add_test(NAME native-metal-tensor-transforms
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/native/tests/TensorTransformTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --clang "${CMAKE_CXX_COMPILER}" --gpu
      --output "${CMAKE_CURRENT_BINARY_DIR}/tensor-transforms")
  set_tests_properties(native-metal-tensor-transforms PROPERTIES TIMEOUT 1200
    RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
endif()
