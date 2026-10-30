# Resolve once and use the verified executable for every shader build. Recent
# Xcode component selectors may return the default version for a pinned ID.
set(KALANDRA_METAL_COMPILER "" CACHE FILEPATH "Explicit pinned Apple Metal compiler")
if(NOT KALANDRA_METAL_COMPILER)
  execute_process(COMMAND xcrun --toolchain "${KALANDRA_METAL_TOOLCHAIN}" --find metal
    OUTPUT_VARIABLE METAL_PATH ERROR_VARIABLE METAL_ERROR RESULT_VARIABLE STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT STATUS EQUAL 0 OR NOT IS_ABSOLUTE "${METAL_PATH}")
    message(FATAL_ERROR "Cannot resolve Metal compiler: ${METAL_ERROR}; set KALANDRA_METAL_COMPILER to the pinned executable")
  endif()
  set(KALANDRA_METAL_COMPILER "${METAL_PATH}" CACHE FILEPATH "Explicit pinned Apple Metal compiler" FORCE)
endif()
execute_process(COMMAND "${KALANDRA_METAL_COMPILER}" --version
  OUTPUT_VARIABLE METAL_VERSION ERROR_VARIABLE METAL_ERROR RESULT_VARIABLE STATUS)
string(REGEX MATCH "Apple metal version ([0-9]+[.][0-9]+) " METAL_VERSION_MATCH "${METAL_VERSION}")
if(NOT STATUS EQUAL 0 OR NOT METAL_VERSION_MATCH OR
   NOT CMAKE_MATCH_1 STREQUAL LOCK_metalCompilerVersion)
  message(FATAL_ERROR "Pinned Metal compiler ${LOCK_metalCompilerVersion} unavailable at ${KALANDRA_METAL_COMPILER}: ${METAL_VERSION}${METAL_ERROR}")
endif()
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
  OUTPUT_VARIABLE KALANDRA_METAL_SDK ERROR_VARIABLE METAL_ERROR RESULT_VARIABLE STATUS
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT STATUS EQUAL 0 OR NOT IS_DIRECTORY "${KALANDRA_METAL_SDK}")
  message(FATAL_ERROR "macOS SDK unavailable: ${METAL_ERROR}")
endif()
# Shader proofs, source compilation and installation are sibling directories.
# Retain the verified SDK outside this directory's normal variable scope.
set(KALANDRA_METAL_SDK "${KALANDRA_METAL_SDK}" CACHE INTERNAL "Verified macOS SDK" FORCE)
