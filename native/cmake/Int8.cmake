if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-int8-tests-${VARIANT} tests/Int8Tests.cpp)
    target_compile_options(kalandra-int8-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-int8-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-int8-tests-${VARIANT} PRIVATE KALANDRA_INT8_FRONTEND_ONLY)
      target_compile_options(kalandra-int8-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-int8-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-int8-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-int8-frontend-${VARIANT} COMMAND kalandra-int8-tests-${VARIANT})
    set_tests_properties(native-int8-frontend-${VARIANT} PROPERTIES TIMEOUT 90)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(INT8_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(INT8_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(INT8_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-int8-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${INT8_RUNTIME}>"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/int8-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyInt8.cmake")
    set_tests_properties(native-int8-${VARIANT} PROPERTIES TIMEOUT 600)
    add_test(NAME native-int8-existing-conversion-smoke-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${INT8_RUNTIME}>" "-DVARIANT=${VARIANT}" "-DSMOKE=ON"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/int8-old-conversions-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyConversions.cmake")
    set_tests_properties(native-int8-existing-conversion-smoke-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
