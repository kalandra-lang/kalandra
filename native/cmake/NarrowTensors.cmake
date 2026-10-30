if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-narrow-tensor-tests-${VARIANT} tests/NarrowTensorTests.cpp)
    target_compile_options(kalandra-narrow-tensor-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-narrow-tensor-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-narrow-tensor-tests-${VARIANT} PRIVATE KALANDRA_NARROW_TENSOR_FRONTEND_ONLY)
      target_compile_options(kalandra-narrow-tensor-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-narrow-tensor-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-narrow-tensor-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-narrow-tensors-frontend-${VARIANT} COMMAND kalandra-narrow-tensor-tests-${VARIANT})
    set_tests_properties(native-narrow-tensors-frontend-${VARIANT} PROPERTIES TIMEOUT 90)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(NARROW_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(NARROW_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(NARROW_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-narrow-tensors-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DRUNTIME=$<TARGET_FILE:${NARROW_RUNTIME}>"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/narrow-tensors-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyNarrowTensors.cmake")
    set_tests_properties(native-narrow-tensors-${VARIANT} PROPERTIES TIMEOUT 900)
    add_test(NAME native-narrow-tensors-existing-conversion-smoke-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DSMOKE=ON"
        "-DRUNTIME=$<TARGET_FILE:${NARROW_RUNTIME}>" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/narrow-tensors-old-conversions-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyTensorConversions.cmake")
    set_tests_properties(native-narrow-tensors-existing-conversion-smoke-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
