include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
foreach(RANK IN ITEMS 0 1 2 3)
  set(TYPE "Tensor[f64")
  if(RANK GREATER 0)
    foreach(I RANGE 1 ${RANK})
      string(APPEND TYPE ",?")
    endforeach()
  endif()
  string(APPEND TYPE "]")
  COMPILE("rank-${RANK}" "fn operation(values:${TYPE}) -> f64:\n  return reduceSum(values)\nfn main() -> f64:\n  return 0\n" --test-entry operation)
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module -ffp-contract=off ${FLAGS}
    "-DTENSOR_RANK=${RANK}" -isystem "${LLVM_ROOT}/include" "${SOURCE_DIR}/tests/Float64ReductionRuntimeTests.cpp"
    "${OUTPUT_DIR}/rank-${RANK}.ll" "${RUNTIME}" "-L${LLVM_ROOT}/lib" "-Wl,-rpath,${LLVM_ROOT}/lib" -lLLVM
    -o "${OUTPUT_DIR}/rank-${RANK}-harness")
  RUN_OK("${OUTPUT_DIR}/rank-${RANK}-harness")
  file(READ "${OUTPUT_DIR}/rank-${RANK}.ll" IR)
  if(IR MATCHES "(fadd|fsub|fmul|fdiv|fneg) double|fpext|fptrunc")
    message(FATAL_ERROR "Float64 reduction bypassed strict arithmetic or changed precision")
  endif()
endforeach()
set(SOURCE [=[fn sum(values:Tensor[f64,?]) -> f64:
  return reduceSum(values)
fn nested(values:Tensor[f64,?]) -> f64 ! TaskError:
  return try parallel:
    work = spawn reduceSum(values)
    alias = work
    first = try await work
    again = try await alias
    first + again
fn probe() -> f64 ! TaskError:
  values:Tensor[f64,2] = tensor([1.0000000000000002,-0.0])
  return try parallel:
    try await spawn nested(values)
fn main() -> f64 ! TaskError:
  return try probe()
]=])
COMPILE(nested "${SOURCE}" --test-entry probe)
EXPECT(nested 2.0000000000000004)
set(RECOVERY [=[fn overflow(value:i64) -> i64:
  return value + 1
fn sum(values:Tensor[f64,?]) -> f64:
  return reduceSum(values)
fn batch(values:Tensor[f64,?]) -> f64 ! TaskError:
  return try parallel:
    bad = spawn overflow(9223372036854775807)
    sibling = spawn sum(values)
    try await bad
    try await sibling
fn probe() -> f64 ! TaskError:
  original:Tensor[f64,2] = tensor([1.0000000000000002,-0.0])
  result = batch(original)
  recovered = result catch fn(error): reduceSum(original)
  return try parallel:
    work = spawn sum(original)
    first = try await work
    again = try await work
    recovered + again
fn main() -> f64 ! TaskError:
  return try probe()
]=])
COMPILE(recovery "${RECOVERY}" --test-entry probe)
EXPECT(recovery 2.0000000000000004)
foreach(NAME IN ITEMS nested recovery)
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    "-I${SOURCE_DIR}/include" "${SOURCE_DIR}/tests/Float64TaskTests.cpp" "${OUTPUT_DIR}/${NAME}.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/${NAME}-harness")
  foreach(WORKERS IN ITEMS 1 2 4)
    RUN_OK("${OUTPUT_DIR}/${NAME}-harness" "${WORKERS}" 4000000000000001 0 0)
  endforeach()
endforeach()
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(MODE IN ITEMS 1 2 4)
    RUN_OK("${OUTPUT_DIR}/nested-harness" "${WORKERS}" 4000000000000001 "${MODE}" 0)
  endforeach()
  foreach(ATTEMPT RANGE 1 24)
    RUN_OK("${OUTPUT_DIR}/recovery-harness" "${WORKERS}" 4000000000000001 3 "${ATTEMPT}")
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${ATTEMPT}"
      "${OUTPUT_DIR}/recovery-harness" "${WORKERS}" 4000000000000001 3 0)
  endforeach()
endforeach()
COMPILE(tree-order "fn main() -> f64:\n  values:Tensor[f64,4] = tensor([18014398509481984.0,1,-18014398509481984.0,1])\n  reduceSum(values)\n")
EXPECT(tree-order 0)
COMPILE(empty "fn main() -> f64:\n  values:Tensor[f64,0] = tensor([])\n  reduceSum(values)\n")
EXPECT(empty 0)
# GPU f64 remains gated; failed compilation preserves artifacts.
foreach(GATE IN ITEMS metal)
  set(GATE_SOURCE "${SOURCE}")
  set(GATE_FLAGS --target metal)
  set(GATE_ERROR f64)
  file(WRITE "${OUTPUT_DIR}/${GATE}.k" "${GATE_SOURCE}")
  file(WRITE "${OUTPUT_DIR}/preserved" "preserved executable")
  execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/${GATE}.k" -o "${OUTPUT_DIR}/preserved" ${GATE_FLAGS}
    RESULT_VARIABLE STATUS ERROR_VARIABLE ERR)
  file(READ "${OUTPUT_DIR}/preserved" PRESERVED)
  if(NOT STATUS EQUAL 1 OR NOT ERR MATCHES "${GATE_ERROR}" OR NOT PRESERVED STREQUAL "preserved executable")
    message(FATAL_ERROR "${GATE} capability gate failed: ${STATUS}: ${ERR}")
  endif()
endforeach()
message(STATUS "Float64 reductions: exact ranked adjacent tree, empty identity, strict IEEE arithmetic, nested/builtin/cached worker calls, recovery and failure settlement passed")
