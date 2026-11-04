if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-array-access-${VARIANT} tests/ArrayAccessTests.cpp)
    target_compile_options(kalandra-array-access-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-array-access-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-array-access-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
      target_link_options(kalandra-array-access-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-array-access-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-array-access-${VARIANT} COMMAND kalandra-array-access-${VARIANT})
    set_tests_properties(native-array-access-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(ACCESS_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(ACCESS_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(ACCESS_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-array-access-runtime-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${ACCESS_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-access-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayAccess.cmake")
    set_tests_properties(native-array-access-runtime-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-loop-allocation-instances-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${ACCESS_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/loop-allocation-instances-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyLoopAllocationInstances.cmake")
    set_tests_properties(native-loop-allocation-instances-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()
endif()
