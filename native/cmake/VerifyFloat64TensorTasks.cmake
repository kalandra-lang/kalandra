include("${SOURCE_DIR}/cmake/Float64Driver.cmake")
function(QUALIFY NAME SOURCE RANK SCENARIO)
  COMPILE("${NAME}" "${SOURCE}" --test-entry probe)
  RUN_OK("${OUTPUT_DIR}/${NAME}")
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    "-DTENSOR_RANK=${RANK}" -DFLOAT_RESULT=64 "${SOURCE_DIR}/tests/Int32TensorTaskTests.cpp"
    "${OUTPUT_DIR}/${NAME}.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/${NAME}-harness")
  foreach(WORKERS IN ITEMS 1 2 4)
    RUN_OK("${OUTPUT_DIR}/${NAME}-harness" "${WORKERS}" "${SCENARIO}" 0)
  endforeach()
endfunction()
QUALIFY(exact [=[fn identity(values:Tensor[float,?]) -> Tensor[float,?]:
  values
fn probe() -> Tensor[float,?] ! TaskError:
  values:Tensor[float,4] = tensor([-0.0,1.0000000000000002,4.9406564584124654e-324,1.7976931348623157e308])
  try parallel:
    first = spawn identity(values)
    alias = first
    completed = try await first
    again = try await alias
    try await spawn identity(again)
fn main() -> float:
  0
]=] 1 1)
QUALIFY(nested [=[fn shift(values:Tensor[float,?,?],amount:float) -> Tensor[float,?,?]:
  values + amount
fn nested(values:Tensor[float,?,?]) -> Tensor[float,?,?] ! TaskError:
  adjusted = values + 1
  try parallel:
    first = spawn shift(adjusted,1)
    alias = first
    completed = try await first
    again = try await alias
    completed + (again - completed)
fn probe() -> Tensor[float,?,?] ! TaskError:
  values:Tensor[float,2,2] = tensor([[9007199254740990.0,10],[100,200]])
  try parallel:
    work = spawn nested(values)
    try await work
fn main() -> float:
  0
]=] 2 2)
QUALIFY(rankzero [=[fn shift(values:Tensor[float],amount:float) -> Tensor[float]:
  values + amount
fn combine(a:Tensor[float],b:Tensor[float]) -> Tensor[float]:
  a+b
fn probe() -> Tensor[float] ! TaskError:
  values:Tensor[float] = tensor(9007199254740992.0)
  try parallel:
    work = spawn shift(values,2)
    first = try await work
    again = try await work
    try await spawn combine(first,again)
fn main() -> float:
  0
]=] 0 0)
QUALIFY(rankthree [=[fn twice(values:Tensor[float,?,?,?]) -> Tensor[float,?,?,?]:
  values + values
fn probe() -> Tensor[float,?,?,?] ! TaskError:
  values:Tensor[float,2,1,2] = tensor([1,2,3,4],[2,1,2])
  try parallel:
    try await spawn twice(values)
fn main() -> float:
  0
]=] 3 3)
QUALIFY(empty [=[fn divide(values:Tensor[float,?,?]) -> Tensor[float,?,?]:
  values / 0
fn probe() -> Tensor[float,?,?] ! TaskError:
  values:Tensor[float,2,0] = tensor([],[2,0])
  try parallel:
    try await spawn divide(values)
fn main() -> float:
  0
]=] 2 4)
QUALIFY(ieee [=[fn identity(values:Tensor[float,?]) -> Tensor[float,?]:
  values
fn produce(values:Tensor[float,?]) -> Tensor[float,?]:
  tensor([1.0/0.0,-1.0/0.0,0.0/0.0,-0.0])
fn probe() -> Tensor[float,?] ! TaskError:
  input:Tensor[float,4] = tensor([1,-1,0,-0.0])
  try parallel:
    try await spawn produce(input)
fn main() -> float:
  0
]=] 1 9)
QUALIFY(recovery [=[fn identity(values:Tensor[float,?]) -> Tensor[float,?]:
  values
fn overflow(value:int64) -> int64:
  value + 1
fn batch(values:Tensor[float,?]) -> Tensor[float,?] ! TaskError:
  try parallel:
    bad = spawn overflow(9223372036854775807)
    sibling = spawn identity(values)
    try await bad
    try await sibling
fn probe() -> Tensor[float,?] ! TaskError:
  original:Tensor[float,2] = tensor([1.0000000000000002,-0.0])
  result = batch(original)
  recovered = result catch fn(error): original
  try parallel:
    work = spawn identity(recovered)
    first = try await work
    try await work
fn main() -> float:
  0
]=] 1 5)
QUALIFY(shape-recovery [=[fn identity(values:Tensor[float,?]) -> Tensor[float,?]:
  values
fn coordinate(values:Tensor[float,?]) -> Tensor[float,?] ! TaskError:
  try parallel:
    try await spawn identity(values)
fn invalidShape(seed:float) -> Tensor[float,2] ! TaskError:
  values:Tensor[float,1] = tensor([9])
  fallback:Tensor[float,2] = tensor([1,2])
  result = coordinate(values)
  recovered = result catch fn(error): fallback
  checked:Tensor[float,2] = recovered
  checked
fn batch() -> Tensor[float,2] ! TaskError:
  try parallel:
    try await spawn invalidShape(0)
fn probe() -> Tensor[float,?] ! TaskError:
  original:Tensor[float,2] = tensor([1.0000000000000002,-0.0])
  result = batch()
  recovered = result catch fn(error): original
  try parallel:
    work = spawn identity(recovered)
    first = try await work
    try await work
fn main() -> float:
  0
]=] 1 5)
QUALIFY(shape-fault [=[fn identity(values:Tensor[float,?]) -> Tensor[float,?]:
  values
fn coordinate(values:Tensor[float,?]) -> Tensor[float,?] ! TaskError:
  try parallel:
    try await spawn identity(values)
fn probe() -> Tensor[float,2] ! TaskError:
  values:Tensor[float,1] = tensor([9])
  fallback:Tensor[float,2] = tensor([1,2])
  result = coordinate(values)
  recovered = result catch fn(error): fallback
  checked:Tensor[float,2] = recovered
  checked
fn main() -> float:
  0
]=] 1 6)
file(READ "${SOURCE_DIR}/tests/fixtures/task-float64-tensor-mixed.k" MIXED)
QUALIFY(mixed "${MIXED}" 1 1)
file(READ "${OUTPUT_DIR}/mixed.ll" IR)
foreach(TYPE IN ITEMS float64 uint64 int64 uint32 int32)
  if(NOT IR MATCHES "value.layout.tensor.${TYPE}.1" OR NOT IR MATCHES "result.tensor.${TYPE}.1")
    message(FATAL_ERROR "Missing distinct ${TYPE} layout")
  endif()
endforeach()
if(NOT IR MATCHES "value.layout.tensor1" OR NOT IR MATCHES "result.tensor1" OR NOT IR MATCHES "value.layout.scalar.float64")
  message(FATAL_ERROR "Mixed packet lost float32 or scalar float64")
endif()
foreach(WORKERS IN ITEMS 1 2 4)
  foreach(MODE IN ITEMS 1 2 4)
    RUN_OK("${OUTPUT_DIR}/nested-harness" "${WORKERS}" 2 "${MODE}")
  endforeach()
  foreach(ATTEMPT RANGE 1 24)
    RUN_OK("${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${ATTEMPT}"
      "${OUTPUT_DIR}/recovery-harness" "${WORKERS}" 5 3)
  endforeach()
endforeach()
message(STATUS "Float64 tensor tasks: ranks 0-3, six mixed tensor families, exact IEEE lanes, nested/cached awaits, retained ownership, shape/integer fault recovery, allocation failures and cancellation passed")
