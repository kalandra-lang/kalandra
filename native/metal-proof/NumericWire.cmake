set(NUMERIC_WIRE_OUTPUTS)
foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
  foreach(MODE IN ITEMS map scalar reduce)
  set(EXTRA)
  set(BASE "${OUT}/numeric-${TYPE}")
  if(MODE STREQUAL "scalar")
    set(BASE "${OUT}/numeric-scalar-${TYPE}")
    set(EXTRA scalar)
  elseif(MODE STREQUAL "reduce")
    set(BASE "${OUT}/numeric-reduce-${TYPE}")
    set(EXTRA reduce)
  endif()
  add_custom_command(OUTPUT "${BASE}.metallib" "${BASE}.spvasm" "${BASE}.metal"
    COMMAND $<TARGET_FILE:kalandra-metal-numeric-wire> "${TYPE}" "${BASE}.mlir" ${EXTRA}
    COMMAND $<TARGET_FILE:kalandra-metal-strict64> "${BASE}.mlir" "${BASE}.lowered.mlir"
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
    DEPENDS kalandra-metal-numeric-wire kalandra-metal-strict64 kalandra-metal-extract toolchain.json
    BYPRODUCTS "${BASE}.mlir" "${BASE}.lowered.mlir" "${BASE}.converted.mlir"
      "${BASE}.kernel.mlir" "${BASE}.spv" "${BASE}.air" VERBATIM)
  list(APPEND NUMERIC_WIRE_OUTPUTS "${BASE}.metallib")
  endforeach()
endforeach()
add_custom_target(native-metal-numeric-wire ALL DEPENDS ${NUMERIC_WIRE_OUTPUTS})
