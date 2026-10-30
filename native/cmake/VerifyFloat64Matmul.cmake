include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
COMPILE(dynamic "fn operation(a:Tensor[float64,?,?],b:Tensor[float64,?,?]) -> Tensor[float64,?,?]:\n  matmul(a,b)\nfn main() -> float:\n  0\n" --test-entry operation)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module -ffp-contract=off ${FLAGS}
  -isystem "${LLVM_ROOT}/include" "${SOURCE_DIR}/tests/Float64MatmulRuntimeTests.cpp"
  "${OUTPUT_DIR}/dynamic.ll" "${RUNTIME}" "-L${LLVM_ROOT}/lib" "-Wl,-rpath,${LLVM_ROOT}/lib" -lLLVM
  -o "${OUTPUT_DIR}/dynamic-harness")
RUN_OK("${OUTPUT_DIR}/dynamic-harness")
file(READ "${OUTPUT_DIR}/dynamic.ll" IR)
if(IR MATCHES "(fadd|fsub|fmul|fdiv|fneg) double|fpext|fptrunc|llvm.fma")
  message(FATAL_ERROR "Float64 matmul bypassed strict separately rounded arithmetic")
endif()
set(SOURCE [=[fn product(a:Tensor[float,?,?],b:Tensor[float,?,?]) -> Tensor[float,?,?]:
  matmul(a,b)
fn nested(a:Tensor[float,?,?],b:Tensor[float,?,?]) -> Tensor[float,?,?] ! TaskError:
  try parallel:
    work = spawn matmul(a,b)
    alias = work
    first = try await work
    again = try await alias
    first + (again-first)
fn probe() -> Tensor[float,?,?] ! TaskError:
  a:Tensor[float,2,2] = tensor([[1,2],[3,4]])
  b:Tensor[float,2,2] = tensor([[2,0],[0,2]])
  try parallel:
    try await spawn nested(a,b)
fn main() -> float ! TaskError:
  result = try probe()
  reduceSum(result)
]=])
COMPILE(nested "${SOURCE}" --test-entry probe)
EXPECT(nested 20)
set(RECOVERY [=[fn overflow(value:int64) -> int64:
  value+1
fn product(a:Tensor[float,?,?],b:Tensor[float,?,?]) -> Tensor[float,?,?]:
  matmul(a,b)
fn batch(a:Tensor[float,?,?],b:Tensor[float,?,?]) -> Tensor[float,?,?] ! TaskError:
  try parallel:
    bad = spawn overflow(9223372036854775807)
    sibling = spawn product(a,b)
    try await bad
    try await sibling
fn probe() -> Tensor[float,?,?] ! TaskError:
  original:Tensor[float,2,2] = tensor([[2,4],[6,8]])
  eye:Tensor[float,2,2] = tensor([[1,0],[0,1]])
  result = batch(original,eye)
  recovered = result catch fn(error): original
  try parallel:
    work = spawn product(recovered,eye)
    first = try await work
    try await work
fn main() -> float ! TaskError:
  result = try probe()
  reduceSum(result)
]=])
COMPILE(recovery "${RECOVERY}" --test-entry probe)
EXPECT(recovery 20)
COMPILE(empty-columns "fn product(a:Tensor[float,?,?],b:Tensor[float,?,?]) -> Tensor[float,?,?]:\n  matmul(a,b)\nfn probe() -> Tensor[float,?,?] ! TaskError:\n  a:Tensor[float,2,3] = tensor([[1,2,3],[4,5,6]])\n  b:Tensor[float,3,0] = tensor([],[3,0])\n  try parallel:\n    try await spawn product(a,b)\nfn main() -> float:\n  0\n" --test-entry probe)
COMPILE(zero-inner "fn product(a:Tensor[float,?,?],b:Tensor[float,?,?]) -> Tensor[float,?,?]:\n  matmul(a,b)\nfn probe() -> Tensor[float,?,?] ! TaskError:\n  a:Tensor[float,2,0] = tensor([],[2,0])\n  b:Tensor[float,0,2] = tensor([],[0,2])\n  try parallel:\n    try await spawn product(a,b)\nfn main() -> float:\n  0\n" --test-entry probe)
foreach(NAME IN ITEMS nested recovery empty-columns zero-inner)
  set(SCENARIO 3)
  if(NAME STREQUAL "empty-columns")
    set(SCENARIO 4)
  elseif(NAME STREQUAL "zero-inner")
    set(SCENARIO 10)
  endif()
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    -DTENSOR_RANK=2 -DFLOAT_RESULT=64 "${SOURCE_DIR}/tests/Int32TensorTaskTests.cpp"
    "${OUTPUT_DIR}/${NAME}.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/${NAME}-harness")
  foreach(WORKERS IN ITEMS 1 2 4)
    RUN_OK("${OUTPUT_DIR}/${NAME}-harness" "${WORKERS}" "${SCENARIO}" 0)
  endforeach()
endforeach()
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(MODE IN ITEMS 1 2 4)
    RUN_OK("${OUTPUT_DIR}/nested-harness" "${WORKERS}" 3 "${MODE}")
  endforeach()
  foreach(ATTEMPT RANGE 1 24)
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${ATTEMPT}"
      "${OUTPUT_DIR}/recovery-harness" "${WORKERS}" 3 3)
  endforeach()
endforeach()
COMPILE(fixed "fn product(a:Tensor[float,2,3],b:Tensor[float,3,2]) -> Tensor[float,2,2]:\n  matmul(a,b)\nfn main() -> float:\n  a:Tensor[float,2,3] = tensor([[1,2,3],[4,5,6]])\n  b:Tensor[float,3,2] = tensor([[7,8],[9,10],[11,12]])\n  reduceSum(product(a,b))\n")
EXPECT(fixed 415)
COMPILE(rounding "fn main() -> float:\n  a:Tensor[float,1,2] = tensor([[1.0000000000000002,-1]])\n  b:Tensor[float,2,1] = tensor([[0.9999999999999998],[1]])\n  reduceSum(matmul(a,b))\n")
EXPECT(rounding 0)
file(WRITE "${OUTPUT_DIR}/metal.k" "${SOURCE}")
file(WRITE "${OUTPUT_DIR}/preserved" "preserved executable")
execute_process(COMMAND "${COMPILER}" "${OUTPUT_DIR}/metal.k" --target metal -o "${OUTPUT_DIR}/preserved"
  RESULT_VARIABLE STATUS ERROR_VARIABLE ERR)
file(READ "${OUTPUT_DIR}/preserved" PRESERVED)
if(NOT STATUS EQUAL 1 OR NOT ERR MATCHES "float64" OR NOT PRESERVED STREQUAL "preserved executable")
  message(FATAL_ERROR "Metal float64 gate failed: ${STATUS}: ${ERR}")
endif()
message(STATUS "Float64 matmul: strict products/tree, ranked views, retained tensors, nested/builtin/cached workers, recovery and failure settlement passed")
