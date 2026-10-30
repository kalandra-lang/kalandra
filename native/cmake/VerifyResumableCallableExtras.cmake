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
COMPILE(zero-argument [=[fn answer()->int:
  21
fn run()->int ! TaskError:
  try parallel:
    try await spawn answer()
fn ordinary()->int ! TaskError:
  21
fn choose(flag:Bool)->(fn()->int ! TaskError):
  if flag:run
  else:ordinary
fn main()->int ! TaskError:
  first=choose(true)
  second=choose(false)
  left=try first()
  right=try second()
  left+right
]=])
EXPECT(zero-argument 42)
COMPILE(multiple-arguments [=[fn add(left:int,right:int)->int:
  left+right
fn run(left:int,right:int)->int ! TaskError:
  try parallel:
    try await spawn add(left,right)
fn main()->int ! TaskError:
  callback=run
  try callback(20,22)
]=])
EXPECT(multiple-arguments 42)
