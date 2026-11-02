if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(MUTATION_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(MUTATION_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(MUTATION_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-array-mutation-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-mutation-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayMutation.cmake")
    set_tests_properties(native-array-mutation-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-array-sort-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-sort-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArraySort.cmake")
    set_tests_properties(native-array-sort-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-array-rebinding-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-rebinding-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayRebinding.cmake")
    set_tests_properties(native-array-rebinding-${VARIANT} PROPERTIES TIMEOUT 900)
  endforeach()
endif()
