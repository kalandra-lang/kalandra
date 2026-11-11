foreach(target IN ITEMS kalandra-native-runtime kalandra-native-runtime-sanitized kalandra-native-runtime-thread-sanitized kalandra-task-core-optimized)
  target_sources(${target} PRIVATE runtime/Format.cpp runtime/Console.cpp runtime/Application.cpp runtime/FileSystem.cpp runtime/Parse.cpp runtime/Path.cpp runtime/Environment.cpp runtime/Time.cpp)
endforeach()
if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    if(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
    endif()
    add_executable(kalandra-format-tests-${variant} tests/FormatTests.cpp)
    add_executable(kalandra-console-tests-${variant} tests/ConsoleRuntimeTests.cpp)
    target_link_libraries(kalandra-console-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-console-tests-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "optimized")
      target_compile_options(kalandra-console-tests-${variant} PRIVATE -O2)
    elseif(variant STREQUAL "sanitized")
      target_compile_options(kalandra-console-tests-${variant} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-console-tests-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    elseif(variant STREQUAL "thread-sanitized")
      target_compile_options(kalandra-console-tests-${variant} PRIVATE -O1 -g -fsanitize=thread)
      target_link_options(kalandra-console-tests-${variant} PRIVATE -fsanitize=thread)
    endif()
    add_test(NAME native-console-runtime-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ConsoleRuntimeTests.py"
        --driver "$<TARGET_FILE:kalandra-console-tests-${variant}>")
    set_tests_properties(native-console-runtime-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io")
    target_include_directories(kalandra-format-tests-${variant} PRIVATE include)
    target_link_libraries(kalandra-format-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-format-tests-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "optimized")
      target_compile_options(kalandra-format-tests-${variant} PRIVATE -O2)
    elseif(variant STREQUAL "sanitized")
      target_compile_options(kalandra-format-tests-${variant} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-format-tests-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    elseif(variant STREQUAL "thread-sanitized")
      target_compile_options(kalandra-format-tests-${variant} PRIVATE -O1 -g -fsanitize=thread)
      target_link_options(kalandra-format-tests-${variant} PRIVATE -fsanitize=thread)
    endif()
    add_test(NAME native-format-application-${variant} COMMAND kalandra-format-tests-${variant})
    set_tests_properties(native-format-application-${variant} PROPERTIES TIMEOUT 60 LABELS "feature-io")
  endforeach()
endif()

if(BUILD_TESTING)
  add_executable(kalandra-standard-library-tests tests/StandardLibraryTests.cpp)
  target_include_directories(kalandra-standard-library-tests PRIVATE include)
  target_link_libraries(kalandra-standard-library-tests PRIVATE kalandra-syntax)
  target_compile_options(kalandra-standard-library-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-standard-library-modules
    COMMAND kalandra-standard-library-tests "${CMAKE_CURRENT_BINARY_DIR}/standard-library-modules")
  set_tests_properties(native-standard-library-modules PROPERTIES TIMEOUT 30 LABELS "feature-io")
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    if(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
    endif()
    add_test(NAME native-format-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/FormatSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/format-source-${variant}"
        --clang "${CMAKE_CXX_COMPILER}" --runtime "$<TARGET_FILE:${runtime}>"
        --variant "${variant}")
    set_tests_properties(native-format-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io")
    add_test(NAME native-console-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ConsoleSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/console-source-${variant}"
        --variant "${variant}")
    set_tests_properties(native-console-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io")
    add_test(NAME native-application-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ApplicationSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/application-source-${variant}"
        --variant "${variant}")
    set_tests_properties(native-application-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io")
  endforeach()
endif()

foreach(target IN ITEMS kalandra-semantics kalandra-semantics-sanitized)
  if(TARGET ${target})
    target_sources(${target} PRIVATE lib/Application.cpp)
  endif()
endforeach()
if(BUILD_TESTING)
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-application-tests-${variant} tests/ApplicationTests.cpp)
    target_compile_options(kalandra-application-tests-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "sanitized")
      target_link_libraries(kalandra-application-tests-${variant} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-application-tests-${variant} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-application-tests-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-application-tests-${variant} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-application-entry-${variant} COMMAND kalandra-application-tests-${variant}
      "${CMAKE_CURRENT_BINARY_DIR}/application-entry-${variant}")
    set_tests_properties(native-application-entry-${variant} PROPERTIES TIMEOUT 60 LABELS "feature-io")
  endforeach()
endif()

foreach(variant IN ITEMS debug sanitized thread-sanitized)
  add_library(kalandra-application-entry-${variant} STATIC runtime/ApplicationEntry.cpp)
  target_compile_options(kalandra-application-entry-${variant} PRIVATE -Wall -Wextra -Werror)
  if(variant STREQUAL "sanitized")
    target_compile_options(kalandra-application-entry-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  elseif(variant STREQUAL "thread-sanitized")
    target_compile_options(kalandra-application-entry-${variant} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  endif()
endforeach()
target_compile_definitions(kalandra PRIVATE
  KALANDRA_APPLICATION_ENTRY="$<TARGET_FILE:kalandra-application-entry-debug>"
  KALANDRA_APPLICATION_SANITIZED_ENTRY="$<TARGET_FILE:kalandra-application-entry-sanitized>"
  KALANDRA_APPLICATION_THREAD_SANITIZED_ENTRY="$<TARGET_FILE:kalandra-application-entry-thread-sanitized>")
kalandra_compiler_runtimes(kalandra-application-entry-debug kalandra-application-entry-sanitized kalandra-application-entry-thread-sanitized)

option(KALANDRA_BUILD_TEST_OBSERVER "Build the non-installed numerical qualification launcher" ${BUILD_TESTING})
if(BUILD_TESTING OR KALANDRA_BUILD_TEST_OBSERVER)
  add_executable(kalandra-test-observer tests/CompilerObserver.cpp)
  target_compile_options(kalandra-test-observer PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(kalandra-test-observer PRIVATE KALANDRA_COMPILER_PATH="$<TARGET_FILE:kalandra>")
  add_dependencies(kalandra-test-observer kalandra)
endif()
if(BUILD_TESTING)
  add_test(NAME native-compiler-observer
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompilerObserverTests.py"
      --compiler "$<TARGET_FILE:kalandra>" --observer "$<TARGET_FILE:kalandra-test-observer>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/compiler-observer")
  set_tests_properties(native-compiler-observer PROPERTIES TIMEOUT 120 LABELS "feature-io")
endif()

if(BUILD_TESTING)
  add_executable(kalandra-console-host-generator tests/ConsoleHostBridgeGenerator.cpp)
  target_link_libraries(kalandra-console-host-generator PRIVATE kalandra-semantics LLVM)
  target_include_directories(kalandra-console-host-generator SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_options(kalandra-console-host-generator PRIVATE -Wall -Wextra -Werror)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    if(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
    endif()
    add_test(NAME native-console-host-bridge-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ConsoleHostBridgeTests.py"
        --generator "$<TARGET_FILE:kalandra-console-host-generator>"
        --clang "${CMAKE_CXX_COMPILER}" --runtime "$<TARGET_FILE:${runtime}>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/console-host-bridge-${variant}" --variant "${variant}")
    set_tests_properties(native-console-host-bridge-${variant} PROPERTIES TIMEOUT 180 LABELS "feature-io")
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(backend IN ITEMS metal vulkan cuda)
    if((backend STREQUAL "metal" AND KALANDRA_BUILD_METAL_PROOF AND KALANDRA_BUILD_METAL_RUNTIME)
       OR (backend STREQUAL "vulkan" AND KALANDRA_BUILD_VULKAN_RUNTIME)
       OR (backend STREQUAL "cuda" AND KALANDRA_BUILD_CUDA_SOURCE))
      foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
        if(NOT backend STREQUAL "metal" AND variant STREQUAL "thread-sanitized")
          continue()
        endif()
        if(backend STREQUAL "cuda" AND variant STREQUAL "sanitized")
          continue()
        endif()
        if(backend STREQUAL "vulkan" AND variant STREQUAL "sanitized" AND NOT KALANDRA_VULKAN_HAS_ASAN)
          continue()
        endif()
        add_test(NAME native-console-${backend}-source-${variant}
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/GpuConsoleSourceTests.py"
            --compiler "$<TARGET_FILE:kalandra>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/console-${backend}-source-${variant}"
            --target "${backend}" --variant "${variant}")
        set_tests_properties(native-console-${backend}-source-${variant} PROPERTIES
          TIMEOUT 600 LABELS "feature-io;feature-gpu" RESOURCE_LOCK "${backend}-gpu")
        if(backend STREQUAL "metal")
          set_tests_properties(native-console-${backend}-source-${variant} PROPERTIES
            RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
        endif()
      endforeach()
    endif()
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug sanitized)
    set(server kalandra-lsp)
    if(variant STREQUAL "sanitized")
      set(server kalandra-lsp-sanitized)
    endif()
    add_test(NAME native-lsp-standard-library-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/lsp/StandardLibraryProtocolTests.py"
        "$<TARGET_FILE:${server}>" "${CMAKE_CURRENT_BINARY_DIR}/lsp-standard-library-${variant}")
    set_tests_properties(native-lsp-standard-library-${variant} PROPERTIES TIMEOUT 60 LABELS "feature-io;feature-tooling")
  endforeach()
endif()

if(BUILD_TESTING)
  add_custom_target(native-io-qualification DEPENDS
    kalandra kalandra-test-observer kalandra-check kalandra-lsp kalandra-lsp-sanitized
    kalandra-standard-library-tests kalandra-console-host-generator
    kalandra-application-tests-debug kalandra-application-tests-sanitized)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    add_dependencies(native-io-qualification kalandra-format-tests-${variant} kalandra-console-tests-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    set(flags -O0)
    if(variant STREQUAL "optimized")
      set(runtime kalandra-task-core-optimized)
      set(flags -O2)
    elseif(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
      set(flags -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
      set(flags -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    endif()
    add_executable(kalandra-filesystem-tests-${variant} tests/FileSystemRuntimeTests.cpp)
    target_link_libraries(kalandra-filesystem-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-filesystem-tests-${variant} PRIVATE -Wall -Wextra -Werror ${flags})
    target_link_options(kalandra-filesystem-tests-${variant} PRIVATE ${flags})
    add_test(NAME native-filesystem-runtime-${variant} COMMAND kalandra-filesystem-tests-${variant}
      "${CMAKE_CURRENT_BINARY_DIR}/filesystem-runtime-${variant}")
    set_tests_properties(native-filesystem-runtime-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-filesystem-tests-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-filesystem-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/FileSystemSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/filesystem-source-${variant}"
        --variant "${variant}")
    set_tests_properties(native-filesystem-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io")
    if(KALANDRA_BUILD_METAL_RUNTIME AND KALANDRA_BUILD_METAL_PROOF)
      add_test(NAME native-metal-filesystem-source-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/FileSystemSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/metal-filesystem-source-${variant}"
          --target metal --variant "${variant}")
      set_tests_properties(native-metal-filesystem-source-${variant} PROPERTIES TIMEOUT 900 LABELS "feature-io;feature-gpu" RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
    endif()
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-filesystem-capabilities-${variant} tests/FileSystemCapabilityTests.cpp)
    target_include_directories(kalandra-filesystem-capabilities-${variant} PRIVATE include)
    target_compile_options(kalandra-filesystem-capabilities-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "sanitized")
      target_compile_options(kalandra-filesystem-capabilities-${variant} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-filesystem-capabilities-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_test(NAME native-filesystem-capabilities-${variant} COMMAND kalandra-filesystem-capabilities-${variant})
    set_tests_properties(native-filesystem-capabilities-${variant} PROPERTIES TIMEOUT 30 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-filesystem-capabilities-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    set(flags -O0)
    if(variant STREQUAL "optimized")
      set(runtime kalandra-task-core-optimized)
      set(flags -O2)
    elseif(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
      set(flags -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
      set(flags -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    endif()
    add_executable(kalandra-parse-tests-${variant} tests/ParseRuntimeTests.cpp)
    target_link_libraries(kalandra-parse-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-parse-tests-${variant} PRIVATE -Wall -Wextra -Werror ${flags})
    target_link_options(kalandra-parse-tests-${variant} PRIVATE ${flags})
    add_test(NAME native-parse-runtime-${variant} COMMAND kalandra-parse-tests-${variant})
    set_tests_properties(native-parse-runtime-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-parse-tests-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-parse-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ParseSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/parse-source-${variant}"
        --variant "${variant}")
    set_tests_properties(native-parse-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io")
    if(KALANDRA_BUILD_METAL_RUNTIME AND KALANDRA_BUILD_METAL_PROOF)
      add_test(NAME native-metal-parse-source-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ParseSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/metal-parse-source-${variant}"
          --target metal --variant "${variant}")
      set_tests_properties(native-metal-parse-source-${variant} PROPERTIES TIMEOUT 900 LABELS "feature-io;feature-gpu" RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
    endif()
  endforeach()
  foreach(variant IN ITEMS optimized sanitized)
    add_test(NAME native-parse-oracle-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ParseOracleTests.py"
        --driver "$<TARGET_FILE:kalandra-parse-tests-${variant}>")
    set_tests_properties(native-parse-oracle-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io")
  endforeach()
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-parse-capabilities-${variant} tests/ParseCapabilityTests.cpp)
    target_include_directories(kalandra-parse-capabilities-${variant} PRIVATE include)
    target_compile_options(kalandra-parse-capabilities-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "sanitized")
      target_compile_options(kalandra-parse-capabilities-${variant} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-parse-capabilities-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_test(NAME native-parse-capabilities-${variant} COMMAND kalandra-parse-capabilities-${variant})
    set_tests_properties(native-parse-capabilities-${variant} PROPERTIES TIMEOUT 30 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-parse-capabilities-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    set(flags -O0)
    if(variant STREQUAL "optimized")
      set(runtime kalandra-task-core-optimized)
      set(flags -O2)
    elseif(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
      set(flags -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
      set(flags -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    endif()
    add_executable(kalandra-path-tests-${variant} tests/PathRuntimeTests.cpp)
    target_link_libraries(kalandra-path-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-path-tests-${variant} PRIVATE -Wall -Wextra -Werror ${flags})
    target_link_options(kalandra-path-tests-${variant} PRIVATE ${flags})
    add_test(NAME native-path-runtime-${variant} COMMAND kalandra-path-tests-${variant})
    set_tests_properties(native-path-runtime-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-path-tests-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-path-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/PathSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/path-source-${variant}"
        --variant "${variant}")
    set_tests_properties(native-path-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io")
    if(KALANDRA_BUILD_METAL_RUNTIME AND KALANDRA_BUILD_METAL_PROOF)
      add_test(NAME native-metal-path-source-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/PathSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/metal-path-source-${variant}"
          --target metal --variant "${variant}")
      set_tests_properties(native-metal-path-source-${variant} PROPERTIES TIMEOUT 900 LABELS "feature-io;feature-gpu" RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
    endif()
  endforeach()
  foreach(variant IN ITEMS optimized sanitized)
    add_test(NAME native-path-oracle-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/PathOracleTests.py"
        --driver "$<TARGET_FILE:kalandra-path-tests-${variant}>")
    set_tests_properties(native-path-oracle-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io")
  endforeach()
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-path-capabilities-${variant} tests/PathCapabilityTests.cpp)
    target_include_directories(kalandra-path-capabilities-${variant} PRIVATE include)
    target_compile_options(kalandra-path-capabilities-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "sanitized")
      target_compile_options(kalandra-path-capabilities-${variant} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-path-capabilities-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_test(NAME native-path-capabilities-${variant} COMMAND kalandra-path-capabilities-${variant})
    set_tests_properties(native-path-capabilities-${variant} PROPERTIES TIMEOUT 30 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-path-capabilities-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    set(flags -O0)
    if(variant STREQUAL "optimized")
      set(runtime kalandra-task-core-optimized)
      set(flags -O2)
    elseif(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
      set(flags -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
      set(flags -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    endif()
    add_executable(kalandra-environment-tests-${variant} tests/EnvironmentRuntimeTests.cpp)
    target_link_libraries(kalandra-environment-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-environment-tests-${variant} PRIVATE -Wall -Wextra -Werror ${flags})
    target_link_options(kalandra-environment-tests-${variant} PRIVATE ${flags})
    add_test(NAME native-environment-runtime-${variant} COMMAND kalandra-environment-tests-${variant})
    set_tests_properties(native-environment-runtime-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-environment-tests-${variant})
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-environment-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/EnvironmentSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/environment-source-${variant}"
        --variant "${variant}")
    set_tests_properties(native-environment-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io")
    if(KALANDRA_BUILD_METAL_RUNTIME AND KALANDRA_BUILD_METAL_PROOF)
      add_test(NAME native-metal-environment-source-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/EnvironmentSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/metal-environment-source-${variant}"
          --target metal --variant "${variant}")
      set_tests_properties(native-metal-environment-source-${variant} PROPERTIES TIMEOUT 900 LABELS "feature-io;feature-gpu" RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
    endif()
  endforeach()
endif()

if(BUILD_TESTING)
  foreach(variant IN ITEMS debug optimized sanitized thread-sanitized)
    set(runtime kalandra-native-runtime)
    set(flags -O0)
    if(variant STREQUAL "optimized")
      set(runtime kalandra-task-core-optimized)
      set(flags -O2)
    elseif(variant STREQUAL "sanitized")
      set(runtime kalandra-native-runtime-sanitized)
      set(flags -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    elseif(variant STREQUAL "thread-sanitized")
      set(runtime kalandra-native-runtime-thread-sanitized)
      set(flags -O1 -g -fsanitize=thread -fno-omit-frame-pointer)
    endif()
    add_executable(kalandra-time-tests-${variant} tests/TimeRuntimeTests.cpp)
    target_link_libraries(kalandra-time-tests-${variant} PRIVATE ${runtime})
    target_compile_options(kalandra-time-tests-${variant} PRIVATE -Wall -Wextra -Werror ${flags})
    target_link_options(kalandra-time-tests-${variant} PRIVATE ${flags})
    add_test(NAME native-time-runtime-${variant} COMMAND kalandra-time-tests-${variant})
    set_tests_properties(native-time-runtime-${variant} PROPERTIES TIMEOUT 120 LABELS "feature-io;feature-tasks")
    add_dependencies(native-io-qualification kalandra-time-tests-${variant})
    add_test(NAME native-time-source-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/TimeSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/time-source-${variant}"
        --variant "${variant}")
    set_tests_properties(native-time-source-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io;feature-tasks")
    add_test(NAME native-resumable-calls-${variant}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ResumableCallSourceTests.py"
        --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/resumable-calls-${variant}"
        --variant "${variant}")
    set_tests_properties(native-resumable-calls-${variant} PROPERTIES TIMEOUT 600 LABELS "feature-io;feature-tasks")
    if(KALANDRA_BUILD_METAL_RUNTIME AND KALANDRA_BUILD_METAL_PROOF)
      add_test(NAME native-metal-resumable-calls-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ResumableCallSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/metal-resumable-calls-${variant}"
          --target metal --variant "${variant}")
      set_tests_properties(native-metal-resumable-calls-${variant} PROPERTIES TIMEOUT 900 LABELS "feature-io;feature-gpu" RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
    endif()
    if(KALANDRA_BUILD_METAL_RUNTIME AND KALANDRA_BUILD_METAL_PROOF)
      add_test(NAME native-metal-time-source-${variant}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/TimeSourceTests.py"
          --compiler "$<TARGET_FILE:kalandra>" --output "${CMAKE_CURRENT_BINARY_DIR}/metal-time-source-${variant}"
          --target metal --variant "${variant}")
      set_tests_properties(native-metal-time-source-${variant} PROPERTIES TIMEOUT 900 LABELS "feature-io;feature-gpu" RESOURCE_LOCK apple-metal-gpu ENVIRONMENT "MTL_DEBUG_LAYER=1")
    endif()
  endforeach()
endif()
if(BUILD_TESTING)
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-time-capabilities-${variant} tests/TimeCapabilityTests.cpp)
    target_include_directories(kalandra-time-capabilities-${variant} PRIVATE include)
    target_compile_options(kalandra-time-capabilities-${variant} PRIVATE -Wall -Wextra -Werror)
    if(variant STREQUAL "sanitized")
      target_compile_options(kalandra-time-capabilities-${variant} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-time-capabilities-${variant} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    add_test(NAME native-time-capabilities-${variant} COMMAND kalandra-time-capabilities-${variant})
    set_tests_properties(native-time-capabilities-${variant} PROPERTIES TIMEOUT 30 LABELS "feature-io")
    add_dependencies(native-io-qualification kalandra-time-capabilities-${variant})
  endforeach()
endif()
