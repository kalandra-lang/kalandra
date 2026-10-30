if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-function-value-tests-${VARIANT} tests/FunctionValueTests.cpp)
    target_compile_options(kalandra-function-value-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-function-value-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-function-value-tests-${VARIANT} PRIVATE KALANDRA_FUNCTION_FRONTEND_ONLY)
      target_compile_options(kalandra-function-value-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-function-value-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-function-value-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-function-values-frontend-${VARIANT} COMMAND kalandra-function-value-tests-${VARIANT})
    set_tests_properties(native-function-values-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
    add_executable(kalandra-fallible-function-tests-${VARIANT} tests/FallibleFunctionTests.cpp)
    target_compile_options(kalandra-fallible-function-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-fallible-function-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-fallible-function-tests-${VARIANT} PRIVATE KALANDRA_FUNCTION_FRONTEND_ONLY)
      target_compile_options(kalandra-fallible-function-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-fallible-function-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-fallible-function-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-fallible-functions-frontend-${VARIANT} COMMAND kalandra-fallible-function-tests-${VARIANT})
    set_tests_properties(native-fallible-functions-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(FUNCTION_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(FUNCTION_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(FUNCTION_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-function-values-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}"
        "-DRUNTIME=$<TARGET_FILE:${FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/function-values-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFunctionValues.cmake")
    set_tests_properties(native-function-values-${VARIANT} PROPERTIES TIMEOUT 240)
    add_test(NAME native-function-collections-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}"
        "-DRUNTIME=$<TARGET_FILE:${FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/function-collections-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFunctionCollections.cmake")
    set_tests_properties(native-function-collections-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-fallible-functions-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/fallible-functions-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFallibleFunctions.cmake")
    set_tests_properties(native-fallible-functions-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-closure-tests-${VARIANT} tests/ClosureTests.cpp)
    target_compile_options(kalandra-closure-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-closure-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-closure-tests-${VARIANT} PRIVATE KALANDRA_FUNCTION_FRONTEND_ONLY)
      target_compile_options(kalandra-closure-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-closure-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-closure-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-closures-frontend-${VARIANT} COMMAND kalandra-closure-tests-${VARIANT})
    set_tests_properties(native-closures-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(CLOSURE_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(CLOSURE_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(CLOSURE_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-closures-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${CLOSURE_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/closures-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyClosures.cmake")
    set_tests_properties(native-closures-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()

  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-block-closure-tests-${VARIANT} tests/BlockClosureTests.cpp)
    target_compile_options(kalandra-block-closure-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-block-closure-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-block-closure-tests-${VARIANT} PRIVATE KALANDRA_FUNCTION_FRONTEND_ONLY)
      target_compile_options(kalandra-block-closure-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-block-closure-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-block-closure-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-block-closures-frontend-${VARIANT} COMMAND kalandra-block-closure-tests-${VARIANT})
    set_tests_properties(native-block-closures-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(BLOCK_CLOSURE_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(BLOCK_CLOSURE_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(BLOCK_CLOSURE_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-block-closures-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${BLOCK_CLOSURE_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/block-closures-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBlockClosures.cmake")
    set_tests_properties(native-block-closures-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()

  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-anonymous-error-tests-${VARIANT} tests/AnonymousErrorTests.cpp)
    target_compile_options(kalandra-anonymous-error-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-anonymous-error-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-anonymous-error-tests-${VARIANT} PRIVATE KALANDRA_FUNCTION_FRONTEND_ONLY)
      target_compile_options(kalandra-anonymous-error-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-anonymous-error-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-anonymous-error-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-anonymous-errors-frontend-${VARIANT} COMMAND kalandra-anonymous-error-tests-${VARIANT})
    set_tests_properties(native-anonymous-errors-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(ANONYMOUS_ERROR_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(ANONYMOUS_ERROR_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(ANONYMOUS_ERROR_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-anonymous-errors-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${ANONYMOUS_ERROR_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/anonymous-errors-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyAnonymousErrors.cmake")
    set_tests_properties(native-anonymous-errors-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()

  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-callable-field-tests-${VARIANT} tests/CallableFieldTests.cpp)
    target_compile_options(kalandra-callable-field-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-callable-field-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-callable-field-tests-${VARIANT} PRIVATE KALANDRA_FUNCTION_FRONTEND_ONLY)
      target_compile_options(kalandra-callable-field-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-callable-field-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-callable-field-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-callable-fields-frontend-${VARIANT} COMMAND kalandra-callable-field-tests-${VARIANT})
    set_tests_properties(native-callable-fields-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(CALLABLE_FIELD_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(CALLABLE_FIELD_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(CALLABLE_FIELD_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-callable-fields-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${CALLABLE_FIELD_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/callable-fields-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyCallableFields.cmake")
    set_tests_properties(native-callable-fields-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()

  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-function-array-tests-${VARIANT} tests/FunctionArrayTests.cpp)
    target_compile_options(kalandra-function-array-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-function-array-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-function-array-tests-${VARIANT} PRIVATE KALANDRA_FUNCTION_FRONTEND_ONLY)
      target_compile_options(kalandra-function-array-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-function-array-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-function-array-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-function-arrays-frontend-${VARIANT} COMMAND kalandra-function-array-tests-${VARIANT})
    set_tests_properties(native-function-arrays-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(FUNCTION_ARRAY_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(FUNCTION_ARRAY_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(FUNCTION_ARRAY_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-function-arrays-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${FUNCTION_ARRAY_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/function-arrays-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyFunctionArrays.cmake")
    set_tests_properties(native-function-arrays-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()


  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-control-map-tests-${VARIANT} tests/ControlMapTests.cpp)
    target_compile_options(kalandra-control-map-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-control-map-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-control-map-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-control-map-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-control-map-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-control-maps-frontend-${VARIANT} COMMAND kalandra-control-map-tests-${VARIANT})
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(CONTROL_MAP_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(CONTROL_MAP_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(CONTROL_MAP_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-control-maps-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${CONTROL_MAP_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/control-maps-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyControlMaps.cmake")
    set_tests_properties(native-control-maps-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()


  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-delimited-closure-tests-${VARIANT} tests/DelimitedClosureTests.cpp)
    target_compile_options(kalandra-delimited-closure-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(kalandra-delimited-closure-tests-${VARIANT} PRIVATE KALANDRA_DELIMITED_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/delimited-closures")
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-delimited-closure-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-delimited-closure-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-delimited-closure-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-delimited-closure-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-delimited-closures-frontend-${VARIANT} COMMAND kalandra-delimited-closure-tests-${VARIANT})
    set_tests_properties(native-delimited-closures-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(DELIMITED_CLOSURE_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(DELIMITED_CLOSURE_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(DELIMITED_CLOSURE_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-delimited-closures-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${DELIMITED_CLOSURE_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/delimited-closures-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyDelimitedClosures.cmake")
    set_tests_properties(native-delimited-closures-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()

  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-recursive-callable-tests-${VARIANT} tests/RecursiveCallableTests.cpp)
    target_compile_options(kalandra-recursive-callable-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(kalandra-recursive-callable-tests-${VARIANT} PRIVATE KALANDRA_RECURSIVE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recursive-callables")
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-recursive-callable-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-recursive-callable-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-recursive-callable-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-recursive-callable-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-recursive-callables-frontend-${VARIANT} COMMAND kalandra-recursive-callable-tests-${VARIANT})
    set_tests_properties(native-recursive-callables-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(RECURSIVE_CALLABLE_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(RECURSIVE_CALLABLE_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(RECURSIVE_CALLABLE_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-recursive-callables-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${RECURSIVE_CALLABLE_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-callables-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveCallables.cmake")
    set_tests_properties(native-recursive-callables-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()

  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-worker-function-tests-${VARIANT} tests/WorkerNamedFunctionTests.cpp)
    target_compile_options(kalandra-worker-function-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(kalandra-worker-function-tests-${VARIANT} PRIVATE KALANDRA_WORKER_ERROR_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-error-functions" KALANDRA_WORKER_COLLECTION_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-callable-collections" KALANDRA_WORKER_MAP_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-callable-maps" KALANDRA_INDIRECT_SPAWN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/indirect-spawn" KALANDRA_WORKER_INDIRECT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-indirect-provenance" KALANDRA_WORKER_PARAMETER_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-parameters" KALANDRA_WORKER_AGGREGATE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-callable-aggregates" KALANDRA_WORKER_RESULT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-function-results" KALANDRA_WORKER_NESTED_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-nested-closures" KALANDRA_WORKER_CLOSURE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-closures" KALANDRA_WORKER_FUNCTION_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worker-named-functions")
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-worker-function-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-worker-function-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-worker-function-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-worker-function-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-worker-functions-frontend-${VARIANT} COMMAND kalandra-worker-function-tests-${VARIANT})
    set_tests_properties(native-worker-functions-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-parallel-map-tests-${VARIANT} tests/ParallelMapTests.cpp)
    target_compile_options(kalandra-parallel-map-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(kalandra-parallel-map-tests-${VARIANT} PRIVATE KALANDRA_PARALLEL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/parallel-map")
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-parallel-map-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-parallel-map-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
      target_link_options(kalandra-parallel-map-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-parallel-map-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-parallel-map-frontend-${VARIANT} COMMAND kalandra-parallel-map-tests-${VARIANT})
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(WORKER_FUNCTION_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(WORKER_FUNCTION_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(WORKER_FUNCTION_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-worker-functions-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-functions-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerFunctions.cmake")
    set_tests_properties(native-worker-functions-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-closures-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-closures-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerClosures.cmake")
    set_tests_properties(native-worker-closures-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-nested-closures-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-nested-closures-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerNestedClosures.cmake")
    set_tests_properties(native-worker-nested-closures-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-function-results-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-function-results-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerFunctionResults.cmake")
    set_tests_properties(native-worker-function-results-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-parallel-map-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/parallel-map-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyParallelMap.cmake")
    set_tests_properties(native-parallel-map-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-error-functions-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-error-functions-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerErrorFunctions.cmake")
    set_tests_properties(native-worker-error-functions-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-callable-collections-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-callable-collections-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerCallableCollections.cmake")
    set_tests_properties(native-worker-callable-collections-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-callable-maps-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-callable-maps-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerCallableMaps.cmake")
    set_tests_properties(native-worker-callable-maps-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-indirect-spawn-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/indirect-spawn-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyIndirectSpawn.cmake")
    set_tests_properties(native-indirect-spawn-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-indirect-provenance-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-indirect-provenance-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerIndirectProvenance.cmake")
    set_tests_properties(native-worker-indirect-provenance-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-parameters-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-parameters-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerParameters.cmake")
    set_tests_properties(native-worker-parameters-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-worker-callable-aggregates-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${WORKER_FUNCTION_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/worker-callable-aggregates-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyWorkerCallableAggregates.cmake")
    set_tests_properties(native-worker-callable-aggregates-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()

endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-execution-flow-tests-${VARIANT} tests/ExecutionFlowTests.cpp)
    target_compile_options(kalandra-execution-flow-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-execution-flow-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-execution-flow-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
      target_link_options(kalandra-execution-flow-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-execution-flow-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME frontend-execution-flow-${VARIANT} COMMAND kalandra-execution-flow-tests-${VARIANT})
    set_tests_properties(frontend-execution-flow-${VARIANT} PROPERTIES TIMEOUT 60)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-resumable-callable-tests-${VARIANT} tests/ResumableCallableTests.cpp)
    target_compile_options(kalandra-resumable-callable-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(kalandra-resumable-callable-tests-${VARIANT} PRIVATE KALANDRA_RESUMABLE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/resumable-callables")
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-resumable-callable-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-resumable-callable-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
      target_link_options(kalandra-resumable-callable-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-resumable-callable-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME frontend-resumable-callables-${VARIANT} COMMAND kalandra-resumable-callable-tests-${VARIANT})
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(CALLABLE_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(CALLABLE_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(CALLABLE_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-resumable-callables-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${CALLABLE_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/resumable-callables-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyResumableCallables.cmake")
    set_tests_properties(native-resumable-callables-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-resumable-callable-extras-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLVM_ROOT=${KALANDRA_LLVM_ROOT}" "-DRUNTIME=$<TARGET_FILE:${CALLABLE_RUNTIME}>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/resumable-callable-extras-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyResumableCallableExtras.cmake")
    set_tests_properties(native-resumable-callable-extras-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-stack-outline-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/stack-outline-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyStackOutline.cmake")
    set_tests_properties(native-stack-outline-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
