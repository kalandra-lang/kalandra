foreach(VARIANT IN ITEMS debug sanitized)
  add_executable(kalandra-conversion-tests-${VARIANT} tests/ConversionTests.cpp)
  target_compile_options(kalandra-conversion-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
  if(VARIANT STREQUAL "sanitized")
    target_link_libraries(kalandra-conversion-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized kalandra-backend)
    target_compile_options(kalandra-conversion-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(kalandra-conversion-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
  else()
    target_link_libraries(kalandra-conversion-tests-${VARIANT} PRIVATE kalandra-backend)
  endif()
  add_test(NAME native-conversions-frontend-${VARIANT} COMMAND kalandra-conversion-tests-${VARIANT})
  set_tests_properties(native-conversions-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
endforeach()
foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
  if(VARIANT STREQUAL "sanitized")
    set(CONVERSION_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
  elseif(VARIANT STREQUAL "thread-sanitized")
    set(CONVERSION_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
  else()
    set(CONVERSION_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
  endif()
  add_test(NAME native-conversions-${VARIANT}
    COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
      "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${CONVERSION_RUNTIME}" "-DVARIANT=${VARIANT}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/conversions-${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyConversions.cmake")
  set_tests_properties(native-conversions-${VARIANT} PROPERTIES TIMEOUT 600)
  foreach(TYPE IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
    add_test(NAME native-tensor-conversions-${TYPE}-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${CONVERSION_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DSOURCE_FILTER=${TYPE}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/tensor-conversions-${TYPE}-${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyTensorConversions.cmake")
    set_tests_properties(native-tensor-conversions-${TYPE}-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endforeach()
