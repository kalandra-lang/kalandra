if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-record-storage-tests-${VARIANT} tests/RecordStorageTests.cpp)
    target_compile_options(kalandra-record-storage-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-record-storage-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-record-storage-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-record-storage-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-record-storage-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-record-storage-${VARIANT} COMMAND kalandra-record-storage-tests-${VARIANT})
    set_tests_properties(native-record-storage-${VARIANT} PROPERTIES TIMEOUT 120)
    add_executable(kalandra-record-physical-layout-tests-${VARIANT} tests/RecordPhysicalLayoutTests.cpp)
    target_include_directories(kalandra-record-physical-layout-tests-${VARIANT} SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
    target_compile_options(kalandra-record-physical-layout-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-record-physical-layout-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized MLIR LLVM)
      target_compile_definitions(kalandra-record-physical-layout-tests-${VARIANT} PRIVATE KALANDRA_PHYSICAL_LAYOUT_FRONTEND_ONLY)
      target_compile_options(kalandra-record-physical-layout-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-record-physical-layout-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-record-physical-layout-tests-${VARIANT} PRIVATE kalandra-backend MLIR LLVM)
    endif()
    add_executable(kalandra-task-value-layout-tests-${VARIANT} tests/TaskValueLayoutTests.cpp)
    target_include_directories(kalandra-task-value-layout-tests-${VARIANT} SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
    target_compile_options(kalandra-task-value-layout-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-task-value-layout-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized MLIR LLVM)
      target_compile_definitions(kalandra-task-value-layout-tests-${VARIANT} PRIVATE KALANDRA_TASK_LAYOUT_FRONTEND_ONLY)
      target_compile_options(kalandra-task-value-layout-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-task-value-layout-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-task-value-layout-tests-${VARIANT} PRIVATE kalandra-backend MLIR LLVM)
    endif()
    add_test(NAME native-task-value-layout-${VARIANT} COMMAND kalandra-task-value-layout-tests-${VARIANT})
    set_tests_properties(native-task-value-layout-${VARIANT} PROPERTIES TIMEOUT 120)
    add_test(NAME native-record-physical-layout-${VARIANT} COMMAND kalandra-record-physical-layout-tests-${VARIANT})
    set_tests_properties(native-record-physical-layout-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    if(VARIANT STREQUAL "sanitized")
      set(RECURSIVE_RUNTIME kalandra-native-runtime-sanitized)
      set(RECURSIVE_FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(RECURSIVE_RUNTIME kalandra-native-runtime-thread-sanitized)
      set(RECURSIVE_FLAGS -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    elseif(VARIANT STREQUAL "optimized")
      set(RECURSIVE_RUNTIME kalandra-task-core-optimized)
      set(RECURSIVE_FLAGS -O2)
    else()
      set(RECURSIVE_RUNTIME kalandra-native-runtime)
      set(RECURSIVE_FLAGS -O0)
    endif()
    add_executable(kalandra-recursive-handoff-tests-${VARIANT} tests/RecursiveHandoffTests.cpp)
    target_link_libraries(kalandra-recursive-handoff-tests-${VARIANT} PRIVATE ${RECURSIVE_RUNTIME})
    target_compile_options(kalandra-recursive-handoff-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror ${RECURSIVE_FLAGS})
    target_link_options(kalandra-recursive-handoff-tests-${VARIANT} PRIVATE ${RECURSIVE_FLAGS})
    add_test(NAME native-recursive-handoff-${VARIANT} COMMAND kalandra-recursive-handoff-tests-${VARIANT})
    set_tests_properties(native-recursive-handoff-${VARIANT} PROPERTIES TIMEOUT 120)
    add_executable(kalandra-recursive-value-runtime-tests-${VARIANT} tests/RecursiveValueRuntimeTests.cpp)
    target_link_libraries(kalandra-recursive-value-runtime-tests-${VARIANT} PRIVATE ${RECURSIVE_RUNTIME})
    target_compile_options(kalandra-recursive-value-runtime-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror ${RECURSIVE_FLAGS})
    target_link_options(kalandra-recursive-value-runtime-tests-${VARIANT} PRIVATE ${RECURSIVE_FLAGS})
    add_test(NAME native-recursive-value-runtime-${VARIANT} COMMAND kalandra-recursive-value-runtime-tests-${VARIANT})
    set_tests_properties(native-recursive-value-runtime-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  add_library(kalandra-recursive-task-abi-optimized OBJECT runtime/TaskABI.cpp)
  target_compile_options(kalandra-recursive-task-abi-optimized PRIVATE -Wall -Wextra -Werror -O2)
  add_executable(kalandra-recursive-record-emitter-tests tests/RecursiveRecordEmitterTests.cpp)
  add_dependencies(kalandra-recursive-record-emitter-tests kalandra-recursive-task-abi-optimized)
  target_include_directories(kalandra-recursive-record-emitter-tests SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  target_link_libraries(kalandra-recursive-record-emitter-tests PRIVATE kalandra-backend MLIR LLVM)
  target_compile_options(kalandra-recursive-record-emitter-tests PRIVATE -Wall -Wextra -Werror)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    set(BRIDGE_ABI)
    if(VARIANT STREQUAL "sanitized")
      set(BRIDGE_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(BRIDGE_RUNTIME kalandra-native-runtime-thread-sanitized)
    elseif(VARIANT STREQUAL "optimized")
      set(BRIDGE_RUNTIME kalandra-task-core-optimized)
      set(BRIDGE_ABI "$<TARGET_OBJECTS:kalandra-recursive-task-abi-optimized>")
    else()
      set(BRIDGE_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-recursive-record-bridge-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DEMITTER=$<TARGET_FILE:kalandra-recursive-record-emitter-tests>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DRUNTIME=$<TARGET_FILE:${BRIDGE_RUNTIME}>"
        "-DTASK_ABI_OBJECTS=${BRIDGE_ABI}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-record-bridge-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveRecordBridge.cmake")
    set_tests_properties(native-recursive-record-bridge-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-recursive-source-tests-${VARIANT} tests/RecursiveSourceTests.cpp)
    target_compile_options(kalandra-recursive-source-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-recursive-source-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-recursive-source-tests-${VARIANT} PRIVATE KALANDRA_RECURSIVE_SOURCE_FRONTEND_ONLY)
      target_compile_options(kalandra-recursive-source-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-recursive-source-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-recursive-source-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-recursive-source-frontend-${VARIANT} COMMAND kalandra-recursive-source-tests-${VARIANT})
    set_tests_properties(native-recursive-source-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    set(CONTINUATION_ABI)
    if(VARIANT STREQUAL "sanitized")
      set(CONTINUATION_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(CONTINUATION_RUNTIME kalandra-native-runtime-thread-sanitized)
    elseif(VARIANT STREQUAL "optimized")
      set(CONTINUATION_RUNTIME kalandra-task-core-optimized)
      set(CONTINUATION_ABI "$<TARGET_OBJECTS:kalandra-recursive-task-abi-optimized>")
    else()
      set(CONTINUATION_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-recursive-workers-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>"
        "-DRUNTIME=$<TARGET_FILE:${CONTINUATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DTASK_ABI_OBJECTS=${CONTINUATION_ABI}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-workers-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveWorkers.cmake")
    set_tests_properties(native-recursive-workers-${VARIANT} PROPERTIES TIMEOUT 600)
    add_test(NAME native-recursive-continuations-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DEMITTER=$<TARGET_FILE:kalandra-recursive-source-tests-debug>"
        "-DRUNTIME=$<TARGET_FILE:${CONTINUATION_RUNTIME}>" "-DVARIANT=${VARIANT}"
        "-DTASK_ABI_OBJECTS=${CONTINUATION_ABI}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-continuations-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveContinuations.cmake")
    set_tests_properties(native-recursive-continuations-${VARIANT} PROPERTIES TIMEOUT 180)
    add_test(NAME native-recursive-source-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-source-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveSource.cmake")
    set_tests_properties(native-recursive-source-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-recursive-strings-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-strings-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveStrings.cmake")
    set_tests_properties(native-recursive-strings-${VARIANT} PROPERTIES TIMEOUT 300)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-recursive-collections-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-collections-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveCollections.cmake")
    set_tests_properties(native-recursive-collections-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-recursive-string-arrays-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-string-arrays-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveStringArrays.cmake")
    set_tests_properties(native-recursive-string-arrays-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-recursive-record-arrays-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-record-arrays-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveRecordArrays.cmake")
    set_tests_properties(native-recursive-record-arrays-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-recursive-callable-fields-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-callable-fields-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveCallableFields.cmake")
    set_tests_properties(native-recursive-callable-fields-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-recursive-function-arrays-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/recursive-function-arrays-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecursiveFunctionArrays.cmake")
    set_tests_properties(native-recursive-function-arrays-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()
