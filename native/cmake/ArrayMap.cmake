if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-array-map-tests-${VARIANT} tests/ArrayMapTests.cpp)
    target_compile_options(kalandra-array-map-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-array-map-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-array-map-tests-${VARIANT} PRIVATE KALANDRA_MAP_FRONTEND_ONLY)
      target_compile_options(kalandra-array-map-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-array-map-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-array-map-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-array-map-frontend-${VARIANT} COMMAND kalandra-array-map-tests-${VARIANT})
    set_tests_properties(native-array-map-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(MAP_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(MAP_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
    else()
      set(MAP_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
    endif()
    add_test(NAME native-array-map-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${MAP_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-map-${VARIANT}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayMap.cmake")
    add_test(NAME native-array-closures-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${MAP_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-closures-${VARIANT}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyArrayClosures.cmake")
    set_tests_properties(native-array-closures-${VARIANT} PROPERTIES TIMEOUT 900)
    set_tests_properties(native-array-map-${VARIANT} PROPERTIES TIMEOUT 900)
  endforeach()
endif()
