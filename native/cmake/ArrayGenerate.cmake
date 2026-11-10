if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-array-generate-tests-${VARIANT} tests/ArrayGenerateTests.cpp)
    target_compile_options(kalandra-array-generate-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-array-generate-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-array-generate-tests-${VARIANT} PRIVATE KALANDRA_GENERATE_FRONTEND_ONLY)
      target_compile_options(kalandra-array-generate-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-array-generate-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-array-generate-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-array-generate-frontend-${VARIANT} COMMAND kalandra-array-generate-tests-${VARIANT})
    set_tests_properties(native-array-generate-frontend-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(GENERATE_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(GENERATE_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
    else()
      set(GENERATE_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
    endif()
    add_test(NAME native-array-generate-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=${GENERATE_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-generate-${VARIANT}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayGenerate.cmake")
    set_tests_properties(native-array-generate-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-array-copy-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra-test-observer>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=${GENERATE_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-copy-${VARIANT}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayCopy.cmake")
    set_tests_properties(native-array-copy-${VARIANT} PROPERTIES TIMEOUT 900)
  endforeach()
endif()
