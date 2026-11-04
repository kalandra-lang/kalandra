if(BUILD_TESTING)
  foreach(SUITE IN ITEMS control-arena device-root-liveness device-bool-layout)
    if(SUITE STREQUAL "control-arena")
      set(STORAGE_TEST_SOURCES gpu-runtime/ControlArenaTests.cpp)
    elseif(SUITE STREQUAL "device-bool-layout")
      set(STORAGE_TEST_SOURCES tests/DeviceBoolLayoutTests.cpp)
    else()
      set(STORAGE_TEST_SOURCES metal-source/DeviceLiveness.cpp metal-source/DeviceLivenessTests.cpp)
    endif()
    foreach(VARIANT IN ITEMS optimized sanitized thread-sanitized)
      add_executable(kalandra-${SUITE}-${VARIANT} ${STORAGE_TEST_SOURCES})
      target_include_directories(kalandra-${SUITE}-${VARIANT} PRIVATE include)
      target_compile_options(kalandra-${SUITE}-${VARIANT} PRIVATE -Wall -Wextra -Werror)
      target_link_libraries(kalandra-${SUITE}-${VARIANT} PRIVATE Threads::Threads)
      if(VARIANT STREQUAL "optimized")
        target_compile_options(kalandra-${SUITE}-${VARIANT} PRIVATE -O2)
      elseif(VARIANT STREQUAL "sanitized")
        target_compile_options(kalandra-${SUITE}-${VARIANT} PRIVATE
          -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(kalandra-${SUITE}-${VARIANT} PRIVATE -fsanitize=address,undefined)
      else()
        target_compile_options(kalandra-${SUITE}-${VARIANT} PRIVATE
          -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(kalandra-${SUITE}-${VARIANT} PRIVATE -fsanitize=thread)
      endif()
      if(SUITE STREQUAL "device-bool-layout")
        if(VARIANT STREQUAL "optimized")
          target_link_libraries(kalandra-${SUITE}-${VARIANT} PRIVATE kalandra-task-core-optimized)
        else()
          target_link_libraries(kalandra-${SUITE}-${VARIANT} PRIVATE kalandra-native-runtime-${VARIANT})
        endif()
      endif()
      add_test(NAME native-${SUITE}-${VARIANT} COMMAND kalandra-${SUITE}-${VARIANT})
      set_tests_properties(native-${SUITE}-${VARIANT} PROPERTIES TIMEOUT 30)
    endforeach()
  endforeach()
endif()

if(BUILD_TESTING)
  add_executable(kalandra-device-trace-emitter
    metal-source/DeviceTraceEmitterTests.cpp metal-source/DeviceHeap.cpp metal-source/DeviceSort.cpp)
  target_include_directories(kalandra-device-trace-emitter PRIVATE include)
  target_include_directories(kalandra-device-trace-emitter SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
  target_link_libraries(kalandra-device-trace-emitter PRIVATE MLIR LLVM)
  target_compile_options(kalandra-device-trace-emitter PRIVATE -Wall -Wextra -Werror)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-device-trace-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DEMITTER=$<TARGET_FILE:kalandra-device-trace-emitter>"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DMLIR_OPT=${LLVM_TOOLS_BINARY_DIR}/mlir-opt${CMAKE_EXECUTABLE_SUFFIX}"
        "-DMLIR_TRANSLATE=${LLVM_TOOLS_BINARY_DIR}/mlir-translate${CMAKE_EXECUTABLE_SUFFIX}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DVARIANT=${VARIANT}"
        "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/device-trace-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyDeviceTrace.cmake")
    set_tests_properties(native-device-trace-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()

endif()
