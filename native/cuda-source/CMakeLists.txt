add_executable(kalandra-cuda-lower Lower.cpp)
target_link_libraries(kalandra-cuda-lower PRIVATE kalandra-strict-float kalandra-gpu-numeric MLIR LLVM)
target_compile_options(kalandra-cuda-lower PRIVATE -Wall -Wextra -Werror)
# PassOptions instantiates LLVM virtual option types. Match the linked SDK's
# RTTI ABI rather than relying on the host compiler's default (official Linux
# LLVM archives disable RTTI; Homebrew's SDK may enable it).
if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    target_compile_options(kalandra-cuda-lower PRIVATE /GR-)
  else()
    target_compile_options(kalandra-cuda-lower PRIVATE -fno-rtti)
  endif()
endif()
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME native-cuda-artifacts COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/artifacts.py" $<TARGET_FILE:kalandra-cuda-lower>)
  set_tests_properties(native-cuda-artifacts PROPERTIES TIMEOUT 120)
endif()
if(BUILD_TESTING)
  add_executable(kalandra-cuda-artifact-tests tests/ArtifactTests.cpp)
  target_include_directories(kalandra-cuda-artifact-tests PRIVATE ../include)
  target_compile_options(kalandra-cuda-artifact-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-cuda-artifact-contract COMMAND kalandra-cuda-artifact-tests)
endif()
if(BUILD_TESTING)
  add_test(NAME native-cuda-upstream-audit COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cuda/third_party/sgemm-cuda/adapt.py")
  set_tests_properties(native-cuda-upstream-audit PROPERTIES TIMEOUT 30)
endif()
if(BUILD_TESTING)
  add_test(NAME native-gpu-shared-installed-headers COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../gpu-runtime/InstalledHeadersTests.py" --compiler "${CMAKE_CXX_COMPILER}")
  set_tests_properties(native-gpu-shared-installed-headers PROPERTIES TIMEOUT 60)
endif()
add_executable(kalandra-cuda-wrap Wrap.cpp)
target_include_directories(kalandra-cuda-wrap PRIVATE ../include)
target_compile_options(kalandra-cuda-wrap PRIVATE -Wall -Wextra -Werror)
# The public development target is opt-in. CPU-only compilers retain no CUDA
# runtime/import dependency; installed delivery is a separate qualification gate.
if(KALANDRA_BUILD_CUDA_RUNTIME AND NOT KALANDRA_CUDA_SANITIZERS)
  set(_cuda_config "#pragma once\n")
  foreach(_pair IN ITEMS "CLANG|${CMAKE_CXX_COMPILER}"
      "LOWER|$<TARGET_FILE:kalandra-cuda-lower>"
      "INCLUDE|${CMAKE_CURRENT_SOURCE_DIR}/../cuda"
      "PUBLIC_INCLUDE|${CMAKE_CURRENT_SOURCE_DIR}/../include"
      "TRANSFORMS|${CMAKE_CURRENT_BINARY_DIR}/../cuda/transforms"
      "MATMUL|${CMAKE_CURRENT_BINARY_DIR}/../cuda/kernels/matmul-warp64.ptx"
      "LANGUAGE|$<TARGET_FILE:kalandra-cuda-language>"
      "PROGRAM|$<TARGET_FILE:kalandra-cuda-program>"
      "DRIVER|$<TARGET_FILE:kalandra-cuda>"
      "CORE|$<TARGET_FILE:kalandra-native-runtime>")
    string(REPLACE "|" ";" _fields "${_pair}")
    list(GET _fields 0 _name)
    list(GET _fields 1 _value)
    string(APPEND _cuda_config "#define KALANDRA_CUDA_${_name} R\"cfg(${_value})cfg\"\n")
  endforeach()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../cuda/third_party/sgemm-cuda/LICENSE" _cuda_mit_notice)
  string(APPEND _cuda_config "inline const char kalandraCudaNotice[] = R\"notice(CUDA device programs use project-owned algorithms and standard compiler scalar math. No CUDA SDK libraries are included.\nOptional optimized SGEMM: Simon Boehm SGEMM_CUDA 5a7dcc513d951ba764d51bc9d587b3163f3a894d\n${_cuda_mit_notice})notice\";\n")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/CudaBuildConfig.h" CONTENT "${_cuda_config}")
  add_library(kalandra-cuda-source-backend STATIC Command.cpp Build.cpp)
  target_include_directories(kalandra-cuda-source-backend PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
  target_link_libraries(kalandra-cuda-source-backend PUBLIC kalandra-semantics PRIVATE
    kalandra-gpu-supervisor kalandra-packages kalandra-backend LLVM MLIR)
  target_compile_options(kalandra-cuda-source-backend PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(kalandra PRIVATE kalandra-cuda-source-backend)
  target_compile_definitions(kalandra PRIVATE KALANDRA_HAS_CUDA_SOURCE=1)
  add_dependencies(kalandra kalandra-cuda-lower kalandra-cuda-language kalandra-cuda-transforms kalandra-cuda-matmul-kernels)
endif()
