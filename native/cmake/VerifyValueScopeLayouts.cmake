include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
if(CMAKE_HOST_WIN32)
  set(SCOPE_ABI_FLAGS -fms-runtime-lib=static)
else()
  set(SCOPE_ABI_FLAGS -pthread)
endif()
set(TYPES i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool)
set(SIZES 1 2 4 8 1 2 4 8 4 8 1)
foreach(INDEX RANGE 0 10)
  list(GET TYPES ${INDEX} TYPE)
  list(GET SIZES ${INDEX} BYTES)
  set(BOOL 0)
  set(LITERAL "${TYPE}(1),${TYPE}(2)")
  if(TYPE STREQUAL "bool")
    set(BOOL 1)
    set(LITERAL "true,false")
  endif()
  set(NAME "layout-${TYPE}")
  COMPILE("${NAME}" "fn make()->[${TYPE}]:\n  return [${LITERAL}]\nfn main()->[${TYPE}] ! TaskError:\n  return try parallel:\n    try await spawn make()\n")
  file(READ "${OUTPUT_DIR}/${NAME}.ll" IR)
  string(REGEX MATCHALL "@value\\.layout\\.array\\.[A-Za-z0-9_.]+ = internal constant" LAYOUTS "${IR}")
  list(LENGTH LAYOUTS COUNT)
  if(NOT COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one generated array layout for ${TYPE}")
  endif()
  list(GET LAYOUTS 0 LAYOUT)
  string(REGEX REPLACE " = internal constant$" "" SYMBOL "${LAYOUT}")
  file(APPEND "${OUTPUT_DIR}/${NAME}.ll" "
@scope_test_layouts = internal constant [3 x ptr] [ptr ${SYMBOL}, ptr ${SYMBOL}, ptr ${SYMBOL}]
define ptr @scope_test_layout() { ret ptr ${SYMBOL} }
")
  foreach(DECL IN ITEMS
      "ptr @kalandra_value_scopes_create_v1(ptr, ptr, i64)"
      "i1 @kalandra_value_scopes_begin_v1(ptr)"
      "i1 @kalandra_value_scopes_commit_v1(ptr, i64, ptr)"
      "i1 @kalandra_value_scopes_batch_v1(ptr, ptr, i64)"
      "void @kalandra_value_scopes_destroy_v1(ptr)")
    string(REGEX MATCH "@([A-Za-z0-9_]+)" FUNCTION_NAME "${DECL}")
    if(NOT IR MATCHES "${FUNCTION_NAME}\\(")
      file(APPEND "${OUTPUT_DIR}/${NAME}.ll" "declare ${DECL}\n")
    endif()
  endforeach()
  file(APPEND "${OUTPUT_DIR}/${NAME}.ll" "
define ptr @scope_test_create(ptr %ctx) {
  %result = call ptr @kalandra_value_scopes_create_v1(ptr %ctx, ptr @scope_test_layouts, i64 3)
  ret ptr %result
}
define i1 @scope_test_begin(ptr %arena) {
  %ok = call i1 @kalandra_value_scopes_begin_v1(ptr %arena)
  ret i1 %ok
}
define i1 @scope_test_commit(ptr %arena, i64 %slot, ptr %value) {
  %ok = call i1 @kalandra_value_scopes_commit_v1(ptr %arena, i64 %slot, ptr %value)
  ret i1 %ok
}
define i1 @scope_test_batch(ptr %arena, ptr %writes, i64 %count) {
  %ok = call i1 @kalandra_value_scopes_batch_v1(ptr %arena, ptr %writes, i64 %count)
  ret i1 %ok
}
define void @scope_test_destroy(ptr %arena) {
  call void @kalandra_value_scopes_destroy_v1(ptr %arena)
  ret void
}
")
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} ${SCOPE_ABI_FLAGS}
    "-DELEMENT_BYTES=${BYTES}" "-DBOOL_ELEMENTS=${BOOL}"
    "${SOURCE_DIR}/tests/CompilerValueScopeTests.cpp" "${OUTPUT_DIR}/${NAME}.ll" "${RUNTIME}"
    -o "${OUTPUT_DIR}/${NAME}-harness")
  RUN_OK("${OUTPUT_DIR}/${NAME}-harness")
endforeach()
