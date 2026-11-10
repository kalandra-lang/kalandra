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
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-mutation-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayMutation.cmake")
    set_tests_properties(native-array-mutation-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-array-sort-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-sort-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArraySort.cmake")
    set_tests_properties(native-array-sort-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-array-rebinding-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-rebinding-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayRebinding.cmake")
    set_tests_properties(native-array-rebinding-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-scoped-boolean-map-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/scoped-boolean-map-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyScopedBooleanMap.cmake")
    set_tests_properties(native-scoped-boolean-map-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-bool-array-rebinding-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/bool-array-rebinding-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBoolArrayRebinding.cmake")
    set_tests_properties(native-bool-array-rebinding-${VARIANT} PROPERTIES TIMEOUT 600)
    add_test(NAME native-owned-map-builder-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=$<TARGET_FILE:${MUTATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/owned-map-builder-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyOwnedMapBuilder.cmake")
    set_tests_properties(native-owned-map-builder-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()
