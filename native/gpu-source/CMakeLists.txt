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

if(KALANDRA_BUILD_VULKAN_RUNTIME AND NOT KALANDRA_VULKAN_SANITIZERS AND NOT KALANDRA_VULKAN_THREAD_SANITIZER)
  # Development command uses configured host tools. Installed resources are a
  # separate qualification step; the CPU-only installed compiler stays intact.
  set(_vulkan_opt "${KALANDRA_LLVM_ROOT}/bin/mlir-opt${CMAKE_EXECUTABLE_SUFFIX}")
  set(_vulkan_translate "${KALANDRA_LLVM_ROOT}/bin/mlir-translate${CMAKE_EXECUTABLE_SUFFIX}")
  if(WIN32)
    foreach(_kind IN ITEMS opt translate)
      if(_kind STREQUAL "opt")
        set(_source OptimizerMain.cpp)
      else()
        set(_source TranslateMain.cpp)
      endif()
      add_executable(kalandra-vulkan-${_kind} "${_source}")
      set_target_properties(kalandra-vulkan-${_kind} PROPERTIES OUTPUT_NAME mlir-${_kind})
      target_include_directories(kalandra-vulkan-${_kind} SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
      target_link_libraries(kalandra-vulkan-${_kind} PRIVATE MLIR LLVM)
      target_compile_options(kalandra-vulkan-${_kind} PRIVATE -Wall -Wextra -Werror)
      if(NOT LLVM_ENABLE_RTTI)
        target_compile_options(kalandra-vulkan-${_kind} PRIVATE -fno-rtti)
      endif()
      add_dependencies(kalandra kalandra-vulkan-${_kind})
    endforeach()
    set(_vulkan_opt "$<TARGET_FILE:kalandra-vulkan-opt>")
    set(_vulkan_translate "$<TARGET_FILE:kalandra-vulkan-translate>")
  endif()
  set(_vulkan_build_config "#pragma once\n")
  foreach(_pair IN ITEMS
      "CLANG|${CMAKE_CXX_COMPILER}" "OPT|${_vulkan_opt}"
      "TRANSLATE|${_vulkan_translate}"
      "LOWER|$<TARGET_FILE:kalandra-vulkan-lower>" "EXTRACT|$<TARGET_FILE:kalandra-vulkan-extract>"
      "VALIDATE|${KALANDRA_SPIRV_VAL}" "INCLUDE|${CMAKE_CURRENT_SOURCE_DIR}/../vulkan"
      "PUBLIC_INCLUDE|${CMAKE_CURRENT_SOURCE_DIR}/../include"
      "LANGUAGE|$<TARGET_FILE:kalandra-vulkan-language>" "ASYNC|$<TARGET_FILE:kalandra-vulkan-async>"
      "PROGRAM|$<TARGET_FILE:kalandra-vulkan-program>" "DRIVER|$<TARGET_FILE:kalandra-vulkan>"
      "CORE|$<TARGET_FILE:kalandra-native-runtime>"
      "EXE_SUFFIX|${CMAKE_EXECUTABLE_SUFFIX}")
    string(REPLACE "|" ";" _fields "${_pair}")
    list(GET _fields 0 _key)
    list(GET _fields 1 _value)
    string(APPEND _vulkan_build_config "#define KALANDRA_VULKAN_${_key} R\"cfg(${_value})cfg\"\n")
  endforeach()
  string(APPEND _vulkan_build_config "inline const char *const kalandraVulkanHostLibraries[] = {")
  foreach(_library IN LISTS CMAKE_DL_LIBS)
    string(APPEND _vulkan_build_config "\"-l${_library}\",")
  endforeach()
  string(APPEND _vulkan_build_config "nullptr};\n")
  if(TARGET kalandra-vulkan-language-sanitized)
    foreach(_pair IN ITEMS
        "LANGUAGE|kalandra-vulkan-language-sanitized" "ASYNC|kalandra-vulkan-async-sanitized"
        "PROGRAM|kalandra-vulkan-program-sanitized" "DRIVER|kalandra-vulkan-sanitized"
        "CORE|kalandra-native-runtime-sanitized")
      string(REPLACE "|" ";" _fields "${_pair}")
      list(GET _fields 0 _key)
      list(GET _fields 1 _target)
      string(APPEND _vulkan_build_config "#define KALANDRA_VULKAN_${_key}_SANITIZED R\"cfg($<TARGET_FILE:${_target}>)cfg\"\n")
    endforeach()
    string(APPEND _vulkan_build_config "#define KALANDRA_VULKAN_HAS_ASAN 1\n")
    add_dependencies(kalandra kalandra-vulkan-language-sanitized)
  else()
    string(APPEND _vulkan_build_config "#define KALANDRA_VULKAN_HAS_ASAN 0\n")
  endif()
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

if(KALANDRA_BUILD_CUDA_SOURCE)
  add_executable(kalandra-cuda-supervisor EXCLUDE_FROM_ALL SupervisorMain.cpp)
  target_compile_definitions(kalandra-cuda-supervisor PRIVATE KALANDRA_CUDA_SUPERVISOR=1)
  target_link_libraries(kalandra-cuda-supervisor PRIVATE kalandra-gpu-supervisor)
  target_compile_options(kalandra-cuda-supervisor PRIVATE -Wall -Wextra -Werror)
endif()
