if(BUILD_TESTING)
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-generics-tests-${variant} tests/GenericsTests.cpp)
    set(semantics kalandra-semantics)
    if(variant STREQUAL "sanitized")
      set(semantics kalandra-semantics-sanitized)
      target_compile_options(kalandra-generics-tests-${variant} PRIVATE
        -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-generics-tests-${variant} PRIVATE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    target_link_libraries(kalandra-generics-tests-${variant} PRIVATE ${semantics})
    target_compile_options(kalandra-generics-tests-${variant} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME frontend-generics-${variant} COMMAND kalandra-generics-tests-${variant}
      "${CMAKE_CURRENT_BINARY_DIR}/generics-modules-${variant}")
    set_tests_properties(frontend-generics-${variant} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

if(BUILD_TESTING)
  add_custom_target(native-generics-qualification
    DEPENDS kalandra kalandra-generics-tests-debug kalandra-generics-tests-sanitized)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    if(WIN32 AND variant STREQUAL "thread-sanitized")
      continue()
    endif()
    add_test(NAME native-generics-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/GenericsSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --variant "${variant}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/generics-source-${variant}")
    set_tests_properties(native-generics-source-${variant} PROPERTIES TIMEOUT 900
      LABELS "feature-frontend;feature-tasks;feature-control")
    foreach(backend IN ITEMS metal vulkan cuda)
      if(NOT ((backend STREQUAL "metal" AND KALANDRA_BUILD_METAL_PROOF AND KALANDRA_BUILD_METAL_RUNTIME)
         OR (backend STREQUAL "vulkan" AND KALANDRA_BUILD_VULKAN_RUNTIME)
         OR (backend STREQUAL "cuda" AND KALANDRA_BUILD_CUDA_SOURCE AND KALANDRA_BUILD_CUDA_RUNTIME)))
        continue()
      endif()
      if(NOT backend STREQUAL "metal" AND variant STREQUAL "thread-sanitized")
        continue()
      endif()
      if(backend STREQUAL "cuda" AND variant STREQUAL "sanitized")
        continue()
      endif()
      if(backend STREQUAL "vulkan" AND variant STREQUAL "sanitized" AND NOT KALANDRA_VULKAN_HAS_ASAN)
        continue()
      endif()
      set(execution)
      if(WIN32)
        set(execution --compile-only)
      endif()
      add_test(NAME native-${backend}-generics-source-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/GenericsSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --variant "${variant}" --target "${backend}"
          --output "${CMAKE_CURRENT_BINARY_DIR}/${backend}-generics-source-${variant}" ${execution})
      set_tests_properties(native-${backend}-generics-source-${variant} PROPERTIES TIMEOUT 900
        LABELS "feature-frontend;feature-tasks;feature-gpu" RESOURCE_LOCK "${backend}-gpu")
      if(backend STREQUAL "metal")
        set_tests_properties(native-${backend}-generics-source-${variant} PROPERTIES
          RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
      endif()
    endforeach()
  endforeach()
endif()
