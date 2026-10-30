# Native float64 source and typed worker integration.
foreach(VARIANT IN ITEMS debug sanitized)
  add_executable(kalandra-float64-tests-${VARIANT} tests/Float64Tests.cpp)
  target_compile_options(kalandra-float64-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
  if(VARIANT STREQUAL "sanitized")
    target_link_libraries(kalandra-float64-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
    target_compile_definitions(kalandra-float64-tests-${VARIANT} PRIVATE KALANDRA_FLOAT64_FRONTEND_ONLY)
    target_compile_options(kalandra-float64-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(kalandra-float64-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
  else()
    target_link_libraries(kalandra-float64-tests-${VARIANT} PRIVATE kalandra-backend)
  endif()
  add_test(NAME native-float64-frontend-${VARIANT} COMMAND kalandra-float64-tests-${VARIANT})
  set_tests_properties(native-float64-frontend-${VARIANT} PROPERTIES TIMEOUT 60)
endforeach()
foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
  if(VARIANT STREQUAL "sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
  elseif(VARIANT STREQUAL "thread-sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
  else()
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
  endif()
  foreach(KIND IN ITEMS scalars tasks)
    add_test(NAME native-float64-${KIND}-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${FLOAT64_RUNTIME}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/float64-${KIND}-${VARIANT}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFloat64${KIND}.cmake")
    set_tests_properties(native-float64-${KIND}-${VARIANT} PROPERTIES TIMEOUT 240)
  endforeach()
endforeach()
foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
  if(VARIANT STREQUAL "sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
  elseif(VARIANT STREQUAL "thread-sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
  else()
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
  endif()
  add_test(NAME native-float64-tensors-${VARIANT}
    COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
      "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${FLOAT64_RUNTIME}" "-DVARIANT=${VARIANT}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/float64-tensors-${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFloat64Tensors.cmake")
  set_tests_properties(native-float64-tensors-${VARIANT} PROPERTIES TIMEOUT 240)
endforeach()

foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
  if(VARIANT STREQUAL "sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
  elseif(VARIANT STREQUAL "thread-sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
  else()
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
  endif()
  add_test(NAME native-float64-tensor-tasks-${VARIANT}
    COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
      "-DRUNTIME=${FLOAT64_RUNTIME}" "-DVARIANT=${VARIANT}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/float64-tensor-tasks-${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFloat64TensorTasks.cmake")
  set_tests_properties(native-float64-tensor-tasks-${VARIANT} PROPERTIES TIMEOUT 240)
endforeach()

foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
  if(VARIANT STREQUAL "sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
  elseif(VARIANT STREQUAL "thread-sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
  else()
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
  endif()
  add_test(NAME native-float64-reductions-${VARIANT}
    COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
      "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${FLOAT64_RUNTIME}" "-DVARIANT=${VARIANT}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/float64-reductions-${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFloat64Reductions.cmake")
  set_tests_properties(native-float64-reductions-${VARIANT} PROPERTIES TIMEOUT 240)
endforeach()

foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
  if(VARIANT STREQUAL "sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-sanitized>")
  elseif(VARIANT STREQUAL "thread-sanitized")
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime-thread-sanitized>")
  else()
    set(FLOAT64_RUNTIME "$<TARGET_FILE:kalandra-native-runtime>")
  endif()
  add_test(NAME native-float64-matmul-${VARIANT}
    COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCLANGXX=${CMAKE_CXX_COMPILER}"
      "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=${FLOAT64_RUNTIME}" "-DVARIANT=${VARIANT}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/float64-matmul-${VARIANT}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFloat64Matmul.cmake")
  set_tests_properties(native-float64-matmul-${VARIANT} PROPERTIES TIMEOUT 240)
endforeach()
