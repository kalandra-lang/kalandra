file(READ "${OUTPUT_DIR}/strict64-gpu.spvasm" SPIRV)
file(READ "${OUTPUT_DIR}/strict64-gpu.metal" MSL)
file(READ "${OUTPUT_DIR}/strict64-compact.ll" CPU)
if(SPIRV MATCHES "OpTypeFloat|OpTypeInt 128|OpF(Add|Sub|Mul|Div|Negate)|OpCapability Float64" OR
   NOT SPIRV MATCHES "OpTypeInt 64" OR NOT SPIRV MATCHES "OpTypeInt 32" OR
   MSL MATCHES "(^|[^A-Za-z])(double|float|half)([^A-Za-z]|$)" OR CPU MATCHES "i128")
  message(FATAL_ERROR "Binary64 GPU bit carriers escaped integer-only 32/64-bit lowering")
endif()
message(STATUS "Binary64 GPU shader uses integer-only storage/arithmetic with no native double or i128")
