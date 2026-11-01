# One typed map/program emitter for all GPU backends. Historical source paths
# are retained while the host-supervisor/runtime interfaces are generalized.
add_library(kalandra-gpu-source-ir STATIC
  ../metal-source/NumericShader.cpp ../metal-source/NumericProgram.cpp)
target_include_directories(kalandra-gpu-source-ir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../metal-source")
target_include_directories(kalandra-gpu-source-ir SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_link_libraries(kalandra-gpu-source-ir PUBLIC kalandra-semantics PRIVATE kalandra-gpu-numeric MLIR LLVM)
target_compile_options(kalandra-gpu-source-ir PRIVATE -Wall -Wextra -Werror)
add_library(kalandra-gpu-supervisor STATIC ../metal-source/Compiler.cpp)
target_link_libraries(kalandra-gpu-supervisor PUBLIC kalandra-semantics PRIVATE
  kalandra-gpu-source-ir kalandra-packages kalandra-backend kalandra-gpu-numeric MLIR LLVM)
target_include_directories(kalandra-gpu-supervisor SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_options(kalandra-gpu-supervisor PRIVATE -Wall -Wextra -Werror)
add_executable(kalandra-vulkan-lower EXCLUDE_FROM_ALL Lower.cpp)
target_link_libraries(kalandra-vulkan-lower PRIVATE kalandra-strict-float kalandra-gpu-numeric MLIR LLVM)
target_compile_options(kalandra-vulkan-lower PRIVATE -Wall -Wextra -Werror)
add_executable(kalandra-vulkan-extract EXCLUDE_FROM_ALL ../metal-proof/Extract.cpp)
target_include_directories(kalandra-vulkan-extract SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_link_libraries(kalandra-vulkan-extract PRIVATE MLIR LLVM)
target_compile_options(kalandra-vulkan-extract PRIVATE -Wall -Wextra -Werror)
add_executable(kalandra-vulkan-kernels EXCLUDE_FROM_ALL Kernels.cpp)
target_link_libraries(kalandra-vulkan-kernels PRIVATE kalandra-gpu-source-ir MLIR LLVM)
target_include_directories(kalandra-vulkan-kernels SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_options(kalandra-vulkan-kernels PRIVATE -Wall -Wextra -Werror)
add_executable(kalandra-vulkan-supervisor EXCLUDE_FROM_ALL SupervisorMain.cpp)
target_link_libraries(kalandra-vulkan-supervisor PRIVATE kalandra-gpu-supervisor)
target_compile_options(kalandra-vulkan-supervisor PRIVATE -Wall -Wextra -Werror)

# Independent all-pair conversion qualification; Metal's generator defaults stay unchanged.
add_executable(kalandra-vulkan-conversion-kernels EXCLUDE_FROM_ALL ../metal-proof/Conversion.cpp)
target_link_libraries(kalandra-vulkan-conversion-kernels PRIVATE kalandra-gpu-numeric MLIR LLVM)
target_compile_options(kalandra-vulkan-conversion-kernels PRIVATE -Wall -Wextra -Werror)

if(KALANDRA_BUILD_VULKAN_RUNTIME)
  # Development command uses configured host tools. Installed resources are a
  # separate qualification step; the CPU-only installed compiler stays intact.
  set(_vulkan_build_config "#pragma once\n")
  foreach(_pair IN ITEMS
      "CLANG|${CMAKE_CXX_COMPILER}" "OPT|${KALANDRA_LLVM_ROOT}/bin/mlir-opt${CMAKE_EXECUTABLE_SUFFIX}"
      "TRANSLATE|${KALANDRA_LLVM_ROOT}/bin/mlir-translate${CMAKE_EXECUTABLE_SUFFIX}"
      "LOWER|$<TARGET_FILE:kalandra-vulkan-lower>" "EXTRACT|$<TARGET_FILE:kalandra-vulkan-extract>"
      "VALIDATE|${KALANDRA_SPIRV_VAL}" "INCLUDE|${CMAKE_CURRENT_SOURCE_DIR}/../vulkan"
      "PUBLIC_INCLUDE|${CMAKE_CURRENT_SOURCE_DIR}/../include"
      "LANGUAGE|$<TARGET_FILE:kalandra-vulkan-language>" "ASYNC|$<TARGET_FILE:kalandra-vulkan-async>"
      "PROGRAM|$<TARGET_FILE:kalandra-vulkan-program>" "DRIVER|$<TARGET_FILE:kalandra-vulkan>"
      "CORE|$<TARGET_FILE:kalandra-native-runtime>" "LOADER|${Vulkan_LIBRARY}"
      "EXE_SUFFIX|${CMAKE_EXECUTABLE_SUFFIX}")
    string(REPLACE "|" ";" _fields "${_pair}")
    list(GET _fields 0 _key)
    list(GET _fields 1 _value)
    string(APPEND _vulkan_build_config "#define KALANDRA_VULKAN_${_key} R\"cfg(${_value})cfg\"\n")
  endforeach()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/third_party/ggml/LICENSE" _vulkan_notice)
  string(APPEND _vulkan_build_config
    "inline const char kalandraVulkanNotice[] = R\"notice(${_vulkan_notice})notice\";\n")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/VulkanBuildConfig.h"
    CONTENT "${_vulkan_build_config}")
  add_library(kalandra-vulkan-source-backend STATIC VulkanCommand.cpp VulkanBuild.cpp)
  target_include_directories(kalandra-vulkan-source-backend PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
  target_include_directories(kalandra-vulkan-source-backend SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_options(kalandra-vulkan-source-backend PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(kalandra-vulkan-source-backend PRIVATE kalandra-gpu-supervisor
    kalandra-packages kalandra-backend kalandra-vulkan-resources LLVM MLIR)
  target_link_libraries(kalandra PRIVATE kalandra-vulkan-source-backend)
  target_compile_definitions(kalandra PRIVATE KALANDRA_HAS_VULKAN_SOURCE=1)
  add_dependencies(kalandra kalandra-vulkan-lower kalandra-vulkan-extract kalandra-vulkan-language)
endif()
