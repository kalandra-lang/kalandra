cmake_minimum_required(VERSION 3.28)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(KalandraCudaBringup LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  include(CTest)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32)
  message(FATAL_ERROR "CUDA runtime targets Linux and Windows")
endif()
option(KALANDRA_CUDA_SANITIZERS "Instrument CUDA host ownership/runtime with ASan and UBSan" OFF)
if(KALANDRA_CUDA_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
find_path(KALANDRA_CUDA_HEADERS cuda.h HINTS /usr/local/cuda/include REQUIRED)
add_library(kalandra-cuda STATIC Cuda.cpp)
target_include_directories(kalandra-cuda PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${KALANDRA_CUDA_HEADERS}" ../include)
target_link_libraries(kalandra-cuda PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(kalandra-cuda PRIVATE -Wall -Wextra -Werror)
if(BUILD_TESTING)
  add_executable(kalandra-cuda-runtime-tests tests/RuntimeTests.cpp)
  target_link_libraries(kalandra-cuda-runtime-tests PRIVATE kalandra-cuda)
  target_compile_options(kalandra-cuda-runtime-tests PRIVATE -Wall -Wextra -Werror)
  # Hardware execution is explicit, never a surprise during CPU-only CTest.
  set(KALANDRA_CUDA_TEST_ARTIFACTS "" CACHE PATH "Explicit generated CUDA arithmetic artifacts")
  if(KALANDRA_CUDA_TEST_ARTIFACTS)
    add_test(NAME native-cuda-runtime COMMAND kalandra-cuda-runtime-tests "${KALANDRA_CUDA_TEST_ARTIFACTS}")
    set_tests_properties(native-cuda-runtime PROPERTIES TIMEOUT 180)
  endif()
endif()
add_library(kalandra-cuda-program STATIC Program.cpp TensorBridge.cpp)
target_include_directories(kalandra-cuda-program PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" ../include)
target_link_libraries(kalandra-cuda-program PUBLIC kalandra-cuda)
target_compile_options(kalandra-cuda-program PRIVATE -Wall -Wextra -Werror)
add_library(kalandra-cuda-language STATIC Async.cpp Language.cpp)
target_include_directories(kalandra-cuda-language PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" ../include)
target_link_libraries(kalandra-cuda-language PUBLIC kalandra-cuda-program)
target_compile_options(kalandra-cuda-language PRIVATE -Wall -Wextra -Werror)
find_package(Threads REQUIRED)
if(TARGET kalandra-native-runtime)
  if(KALANDRA_CUDA_SANITIZERS)
    target_link_libraries(kalandra-cuda-program PUBLIC kalandra-native-runtime-sanitized)
  else()
    target_link_libraries(kalandra-cuda-program PUBLIC kalandra-native-runtime)
  endif()
else()
  add_library(kalandra-cuda-task-core STATIC ../runtime/Runtime.cpp ../runtime/Tasks.cpp)
  target_compile_options(kalandra-cuda-task-core PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(kalandra-cuda-task-core PUBLIC Threads::Threads)
  target_link_libraries(kalandra-cuda-program PUBLIC kalandra-cuda-task-core)
endif()
if(BUILD_TESTING)
  add_executable(kalandra-cuda-async-program-tests tests/AsyncProgramTests.cpp)
  target_link_libraries(kalandra-cuda-async-program-tests PRIVATE kalandra-cuda-language)
  target_compile_options(kalandra-cuda-async-program-tests PRIVATE -Wall -Wextra -Werror)
  set(KALANDRA_CUDA_TEST_STEP_KERNEL "" CACHE FILEPATH "Explicit source-generated CUDA step kernel")
  if(KALANDRA_CUDA_TEST_STEP_KERNEL)
    add_test(NAME native-cuda-async-program COMMAND kalandra-cuda-async-program-tests "${KALANDRA_CUDA_TEST_STEP_KERNEL}")
    set_tests_properties(native-cuda-async-program PROPERTIES TIMEOUT 180)
  endif()
endif()
if(BUILD_TESTING)
  add_executable(kalandra-cuda-failure-tests tests/FailureTests.cpp)
  target_link_libraries(kalandra-cuda-failure-tests PRIVATE kalandra-cuda)
  target_compile_options(kalandra-cuda-failure-tests PRIVATE -Wall -Wextra -Werror)
  if(KALANDRA_CUDA_TEST_ARTIFACTS)
    add_test(NAME native-cuda-failures COMMAND kalandra-cuda-failure-tests "${KALANDRA_CUDA_TEST_ARTIFACTS}")
    set_tests_properties(native-cuda-failures PROPERTIES TIMEOUT 180)
  endif()
endif()
set(KALANDRA_GPU_PREBUILT_RESOURCES "" CACHE PATH "Verified same-source portable GPU resources")
if(KALANDRA_GPU_PREBUILT_RESOURCES)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../gpu-runtime/qualification/portable_resources.py"
    --directory "${KALANDRA_GPU_PREBUILT_RESOURCES}" COMMAND_ERROR_IS_FATAL ANY)
  file(COPY "${KALANDRA_GPU_PREBUILT_RESOURCES}/cuda/transforms" "${KALANDRA_GPU_PREBUILT_RESOURCES}/cuda/kernels"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
  add_custom_target(kalandra-cuda-transforms)
  add_custom_target(kalandra-cuda-matmul-kernels)
endif()
# Build-time compiler primitives only. Applications load the system driver and
# compiler-owned PTX; they never import NVRTC, cuBLAS or a graph library.
if(TARGET kalandra-cuda-wrap AND NOT KALANDRA_GPU_PREBUILT_RESOURCES)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_library(KALANDRA_CUDA_NVRTC NAMES nvrtc HINTS /usr/local/cuda/lib64 REQUIRED)
  set(_cuda_transforms)
  foreach(_type IN ITEMS int8 uint8 int16 uint16 int32 uint32 int64 uint64 float32 float64)
    list(APPEND _cuda_transforms "transpose|${_type}")
  endforeach()
  list(APPEND _cuda_transforms "softmaxRows|float32" "rmsNormRows|float32")
  set(_cuda_transform_outputs)
  foreach(_item IN LISTS _cuda_transforms)
    string(REPLACE "|" ";" _fields "${_item}")
    list(GET _fields 0 _kind)
    list(GET _fields 1 _type)
    set(_base "${CMAKE_CURRENT_BINARY_DIR}/transforms/${_kind}-${_type}")
    add_custom_command(OUTPUT "${_base}.ptx"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/GenerateTransform.py"
        --kind "${_kind}" --numeric "${_type}" --output "${_base}.cu"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../cuda-source/qualification/nvrtc.py"
        --source "${_base}.cu" --output "${_base}.raw.ptx" --library "${KALANDRA_CUDA_NVRTC}"
      COMMAND $<TARGET_FILE:kalandra-cuda-wrap> "${_base}.raw.ptx" "${_base}.ptx" "${_kind}" "${_type}" 80
      DEPENDS GenerateTransform.py ../metal-source/TensorTransformShader.h
        ../cuda-source/qualification/nvrtc.py kalandra-cuda-wrap
      BYPRODUCTS "${_base}.cu" "${_base}.raw.ptx" "${_base}.raw.json" "${_base}.raw.compile.log"
      VERBATIM)
    list(APPEND _cuda_transform_outputs "${_base}.ptx")
  endforeach()
  add_custom_target(kalandra-cuda-transforms DEPENDS ${_cuda_transform_outputs})
endif()
if(BUILD_TESTING)
  add_executable(kalandra-cuda-transform-tests tests/TransformTests.cpp)
  target_link_libraries(kalandra-cuda-transform-tests PRIVATE kalandra-cuda-program)
  target_compile_options(kalandra-cuda-transform-tests PRIVATE -Wall -Wextra -Werror)
  add_executable(kalandra-cuda-transform-oracle tests/TransformOracleDriver.cpp)
  target_link_libraries(kalandra-cuda-transform-oracle PRIVATE kalandra-cuda-program)
  target_compile_options(kalandra-cuda-transform-oracle PRIVATE -Wall -Wextra -Werror)
endif()
if(TARGET kalandra-cuda-wrap AND NOT KALANDRA_GPU_PREBUILT_RESOURCES)
  set(_cuda_warp "${CMAKE_CURRENT_BINARY_DIR}/kernels/sgemm-warp.raw.ptx")
  set(_cuda_warp64 "${CMAKE_CURRENT_BINARY_DIR}/kernels/matmul-warp64.ptx")
  set(_cuda_warp128 "${CMAKE_CURRENT_BINARY_DIR}/kernels/matmul-warp128.ptx")
  add_custom_command(OUTPUT "${_cuda_warp64}" "${_cuda_warp128}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../cuda-source/qualification/compile_sgemm.py"
      --nvrtc "${KALANDRA_CUDA_NVRTC}" --output "${_cuda_warp}"
    COMMAND $<TARGET_FILE:kalandra-cuda-wrap> "${_cuda_warp}" "${_cuda_warp64}" matmul64 float32 80
    COMMAND $<TARGET_FILE:kalandra-cuda-wrap> "${_cuda_warp}" "${_cuda_warp128}" matmul128 float32 80
    DEPENDS third_party/sgemm-cuda/adapt.py third_party/sgemm-cuda/upstream.json
      third_party/sgemm-cuda/10_kernel_warptiling.cuh third_party/sgemm-cuda/LICENSE
      ../cuda-source/qualification/compile_sgemm.py ../cuda-source/qualification/nvrtc.py kalandra-cuda-wrap
    BYPRODUCTS "${_cuda_warp}" "${CMAKE_CURRENT_BINARY_DIR}/kernels/sgemm-warp.raw.json"
      "${CMAKE_CURRENT_BINARY_DIR}/kernels/sgemm-warp.raw.compile.log"
    VERBATIM)
  add_custom_target(kalandra-cuda-matmul-kernels DEPENDS "${_cuda_warp64}" "${_cuda_warp128}")
endif()
if(BUILD_TESTING)
  add_executable(kalandra-cuda-matmul-tests tests/MatmulTests.cpp)
  target_link_libraries(kalandra-cuda-matmul-tests PRIVATE kalandra-cuda-program)
  target_compile_options(kalandra-cuda-matmul-tests PRIVATE -Wall -Wextra -Werror)
endif()
target_link_libraries(kalandra-cuda PUBLIC Threads::Threads)
if(TARGET kalandra-cuda-wrap AND NOT KALANDRA_GPU_PREBUILT_RESOURCES)
  set(KALANDRA_CUDA_CRT_HEADERS "${KALANDRA_CUDA_HEADERS}" CACHE PATH "Build-only CUDA compiler headers")
  set(KALANDRA_CUDA_CCCL_HEADERS "${KALANDRA_CUDA_HEADERS}/cccl" CACHE PATH "Build-only CUDA C++ core headers")
  set(KALANDRA_CUTLASS_ARCHIVE "" CACHE FILEPATH "Optional offline pinned CUTLASS source archive")
  set(_cutlass_archive_args)
  if(KALANDRA_CUTLASS_ARCHIVE)
    list(APPEND _cutlass_archive_args --archive "${KALANDRA_CUTLASS_ARCHIVE}")
  endif()
  set(_cuda_catalog "${CMAKE_CURRENT_BINARY_DIR}/kernels/matmul-catalog.ptx")
  set(_cuda_catalog_raw "${CMAKE_CURRENT_BINARY_DIR}/kernels/matmul-catalog.raw.ptx")
  add_custom_command(OUTPUT "${_cuda_catalog}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../cuda-source/qualification/compile_cutlass.py"
      --nvrtc "${KALANDRA_CUDA_NVRTC}" --output "${_cuda_catalog_raw}"
      --cache "${CMAKE_CURRENT_BINARY_DIR}/cutlass-headers" --sdk-include "${KALANDRA_CUDA_HEADERS}" --cccl-include "${KALANDRA_CUDA_CCCL_HEADERS}" --crt-include "${KALANDRA_CUDA_CRT_HEADERS}"
      ${_cutlass_archive_args}
    COMMAND $<TARGET_FILE:kalandra-cuda-wrap> "${_cuda_catalog_raw}" "${_cuda_catalog}" matmul-catalog float32 80
    DEPENDS third_party/cutlass/Matmul.cu third_party/cutlass/provision.py
      third_party/cutlass/upstream.json third_party/cutlass/LICENSE
      ../cuda-source/qualification/compile_cutlass.py ../cuda-source/qualification/nvrtc.py kalandra-cuda-wrap
    BYPRODUCTS "${_cuda_catalog_raw}" "${CMAKE_CURRENT_BINARY_DIR}/kernels/matmul-catalog.raw.json"
      "${CMAKE_CURRENT_BINARY_DIR}/kernels/matmul-catalog.raw.compile.log"
    VERBATIM)
  add_custom_target(kalandra-cuda-matmul-catalog DEPENDS "${_cuda_catalog}")
  if(BUILD_TESTING)
    add_test(NAME native-cuda-cutlass-audit COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cutlass/provision.py"
      --output "${CMAKE_CURRENT_BINARY_DIR}/cutlass-headers" --audit-only)
  endif()
  add_dependencies(kalandra-cuda-matmul-kernels kalandra-cuda-matmul-catalog)
endif()
if(BUILD_TESTING)
  add_executable(kalandra-cuda-catalog-tests tests/CatalogTests.cpp)
  target_link_libraries(kalandra-cuda-catalog-tests PRIVATE kalandra-cuda-language)
  target_compile_options(kalandra-cuda-catalog-tests PRIVATE -Wall -Wextra -Werror)
endif()
