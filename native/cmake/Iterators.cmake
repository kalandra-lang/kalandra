foreach(target IN ITEMS kalandra-native-runtime kalandra-native-runtime-sanitized
    kalandra-native-runtime-thread-sanitized kalandra-task-core-optimized)
  target_sources(${target} PRIVATE runtime/GeneratorFrame.cpp)
endforeach()
if(BUILD_TESTING)
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-iterator-contract-tests-${variant} tests/IteratorContractsTests.cpp)
    set(semantics kalandra-semantics)
    if(variant STREQUAL "sanitized")
      set(semantics kalandra-semantics-sanitized)
      target_compile_options(kalandra-iterator-contract-tests-${variant} PRIVATE
        -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-iterator-contract-tests-${variant} PRIVATE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    target_link_libraries(kalandra-iterator-contract-tests-${variant} PRIVATE ${semantics})
    target_compile_options(kalandra-iterator-contract-tests-${variant} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME frontend-iterator-contracts-${variant}
      COMMAND kalandra-iterator-contract-tests-${variant}
        "${CMAKE_CURRENT_BINARY_DIR}/iterator-contracts-${variant}")
    set_tests_properties(frontend-iterator-contracts-${variant} PROPERTIES TIMEOUT 120
      LABELS "feature-frontend;feature-control")
  endforeach()
  add_custom_target(native-iterator-qualification DEPENDS kalandra
    kalandra-iterator-contract-tests-debug kalandra-iterator-contract-tests-sanitized)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    if(WIN32 AND variant STREQUAL "thread-sanitized")
      continue()
    endif()
    set(runtime kalandra-native-runtime)
    if(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
    endif()
    add_executable(kalandra-owned-scope-tests-${variant} tests/OwnedScopeTests.cpp)
    target_link_libraries(kalandra-owned-scope-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-owned-scope-tests-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "optimized")
      target_compile_options(kalandra-owned-scope-tests-${variant} PRIVATE -O2)
    elseif(variant STREQUAL "sanitized")
      target_compile_options(kalandra-owned-scope-tests-${variant} PRIVATE
        -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-owned-scope-tests-${variant} PRIVATE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    elseif(variant STREQUAL "thread-sanitized")
      target_compile_options(kalandra-owned-scope-tests-${variant} PRIVATE -O1 -g -fsanitize=thread)
      target_link_options(kalandra-owned-scope-tests-${variant} PRIVATE -fsanitize=thread)
    endif()
    add_test(NAME native-owned-scopes-${variant} COMMAND kalandra-owned-scope-tests-${variant})
    set_tests_properties(native-owned-scopes-${variant} PROPERTIES TIMEOUT 120
      LABELS "feature-control;feature-io")
    add_dependencies(native-iterator-qualification kalandra-owned-scope-tests-${variant})
    add_executable(kalandra-generator-frame-tests-${variant} tests/GeneratorFrameTests.cpp)
    target_link_libraries(kalandra-generator-frame-tests-${variant} PRIVATE ${runtime})
    get_target_property(options kalandra-owned-scope-tests-${variant} COMPILE_OPTIONS)
    target_compile_options(kalandra-generator-frame-tests-${variant} PRIVATE ${options})
    if(variant STREQUAL "debug")
      target_compile_options(kalandra-generator-frame-tests-${variant} PRIVATE -O0)
    endif()
    get_target_property(options kalandra-owned-scope-tests-${variant} LINK_OPTIONS)
    if(options)
      target_link_options(kalandra-generator-frame-tests-${variant} PRIVATE ${options})
    endif()
    add_test(NAME native-generator-frames-${variant} COMMAND kalandra-generator-frame-tests-${variant})
    set_tests_properties(native-generator-frames-${variant} PROPERTIES TIMEOUT 120
      LABELS "feature-control;feature-io")
    add_dependencies(native-iterator-qualification kalandra-generator-frame-tests-${variant})
    if(TARGET native-io-qualification)
      # Both suites carry feature-io: the existing I/O workflow must build the
      # executables before selecting that label, including on fresh CI hosts.
      add_dependencies(native-io-qualification kalandra-owned-scope-tests-${variant}
        kalandra-generator-frame-tests-${variant})
    endif()
    add_test(NAME native-iterators-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/IteratorSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --variant "${variant}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/iterators-source-${variant}")
    set_tests_properties(native-iterators-source-${variant} PROPERTIES TIMEOUT 600
      LABELS "feature-frontend;feature-control")
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
      if(backend STREQUAL "vulkan" AND variant STREQUAL "sanitized" AND NOT TARGET kalandra-vulkan-language-sanitized)
        continue()
      endif()
      set(execution)
      if(WIN32)
        set(execution --compile-only)
      endif()
      add_test(NAME native-${backend}-iterators-source-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/IteratorSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --variant "${variant}" --target "${backend}"
          --output "${CMAKE_CURRENT_BINARY_DIR}/${backend}-iterators-source-${variant}" ${execution})
      set_tests_properties(native-${backend}-iterators-source-${variant} PROPERTIES TIMEOUT 600
        LABELS "feature-frontend;feature-control;feature-gpu" RESOURCE_LOCK "${backend}-gpu")
      if(backend STREQUAL "metal")
        set_tests_properties(native-${backend}-iterators-source-${variant} PROPERTIES
          RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
      endif()
    endforeach()
  endforeach()
endif()
