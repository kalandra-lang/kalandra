include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
file(READ "${SOURCE_DIR}/tests/fixtures/resumable-callable-map-created.k" TEXT)
COMPILE(map-created "${TEXT}")
EXPECT(map-created 42)
file(READ "${SOURCE_DIR}/tests/fixtures/resumable-callable-unbound.k" TEXT)
COMPILE(unbound "${TEXT}")
EXPECT(unbound 42)
file(APPEND "${OUTPUT_DIR}/unbound.ll" "\n@kalandra_unbound_entry = constant ptr @task.entry.2\n@kalandra_unbound_code = constant ptr @task.entry.1\n")
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/ResumableUnboundRuntimeTests.cpp" "${OUTPUT_DIR}/unbound.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/unbound-harness")
RUN_OK("${OUTPUT_DIR}/unbound-harness")
COMPILE(zero-argument [=[fn answer()->i64:
  return 21
fn run()->i64 ! TaskError:
  return try parallel:
    try await spawn answer()
fn ordinary()->i64 ! TaskError:
  return 21
fn choose(flag:bool)->(fn()->i64 ! TaskError):
  return if flag:run
  else:ordinary
fn main()->i64 ! TaskError:
  first=choose(true)
  second=choose(false)
  left=try first()
  right=try second()
  return left+right
]=])
EXPECT(zero-argument 42)
COMPILE(multiple-arguments [=[fn add(left:i64,right:i64)->i64:
  return left+right
fn run(left:i64,right:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn add(left,right)
fn main()->i64 ! TaskError:
  callback=run
  return try callback(20,22)
]=])
EXPECT(multiple-arguments 42)
