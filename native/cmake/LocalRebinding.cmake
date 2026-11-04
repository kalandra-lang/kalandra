if(BUILD_TESTING)
  if(TARGET kalandra-gpu-supervisor)
    add_executable(kalandra-local-gpu-control-tests tests/LocalGpuControlTests.cpp)
    target_compile_options(kalandra-local-gpu-control-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(kalandra-local-gpu-control-tests PRIVATE kalandra-gpu-supervisor)
    add_test(NAME native-local-gpu-control COMMAND kalandra-local-gpu-control-tests)
    set_tests_properties(native-local-gpu-control PROPERTIES TIMEOUT 180)
  endif()
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-local-rebinding-tests-${VARIANT} tests/LocalRebindingTests.cpp)
    target_compile_options(kalandra-local-rebinding-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-local-rebinding-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-local-rebinding-tests-${VARIANT} PRIVATE KALANDRA_REBIND_FRONTEND_ONLY)
      target_compile_options(kalandra-local-rebinding-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-local-rebinding-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-local-rebinding-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-local-rebinding-frontend-${VARIANT} COMMAND kalandra-local-rebinding-tests-${VARIANT})
    set_tests_properties(native-local-rebinding-frontend-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(LOCAL_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(LOCAL_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
    else()
      set(LOCAL_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
    endif()
    add_test(NAME native-local-rebinding-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=${LOCAL_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/local-rebinding-${VARIANT}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyLocalRebinding.cmake")
    set_tests_properties(native-local-rebinding-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-loop-control-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=${LOCAL_RUNTIME}" "-DVARIANT=${VARIANT}" "-DPYTHON=${Python3_EXECUTABLE}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/loop-control-${VARIANT}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyLoopControl.cmake")
    set_tests_properties(native-loop-control-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-collect-ownership-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DRUNTIME=${LOCAL_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/collect-ownership-${VARIANT}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyCollectOwnership.cmake")
    set_tests_properties(native-collect-ownership-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()
