if(NOT TARGET kalandra-cuda-source-backend)
  return()
endif()
# The root development-toolchain guard disables all install rules. Never stage a
# different LLVM version under the production manifest.
add_executable(kalandra-cuda-installed tools/compile.cpp cuda-source/Build.cpp)
set_target_properties(kalandra-cuda-installed PROPERTIES OUTPUT_NAME kalandra
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cuda-install-bin")
get_target_property(_cpu_definitions kalandra-installed COMPILE_DEFINITIONS)
file(RELATIVE_PATH _cuda_tools_relative
  "${CMAKE_INSTALL_FULL_LIBDIR}/kalandra/native" "${CMAKE_INSTALL_FULL_LIBEXECDIR}/kalandra")
target_compile_definitions(kalandra-cuda-installed PRIVATE ${_cpu_definitions}
  KALANDRA_HAS_CUDA_SOURCE=1 KALANDRA_CUDA_TOOLS_RELATIVE="${_cuda_tools_relative}")
target_include_directories(kalandra-cuda-installed SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(kalandra-cuda-installed PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/cuda-source/$<CONFIG>")
target_link_libraries(kalandra-cuda-installed PRIVATE kalandra-cuda-source-backend
  kalandra-backend kalandra-packages LLVM MLIR)
target_compile_options(kalandra-cuda-installed PRIVATE -Wall -Wextra -Werror)
add_dependencies(kalandra-cuda-installed ${KALANDRA_CPU_RUNTIME_TARGETS}
  kalandra-cuda-lower kalandra-cuda-language kalandra-cuda-transforms kalandra-cuda-matmul-kernels)
install(TARGETS kalandra-cuda-installed RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT NativeCudaCompiler)
install(TARGETS kalandra-cuda kalandra-cuda-program kalandra-cuda-language
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native" COMPONENT NativeCudaCompiler)
install(TARGETS kalandra-cuda-lower RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/kalandra"
  COMPONENT NativeCudaCompiler)
install(FILES cuda/Language.h cuda/Async.h cuda/Program.h cuda/CudaRuntime.h cuda/TensorBridge.h cuda/ExecutablePath.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/cuda" COMPONENT NativeCudaCompiler)
install(FILES gpu-runtime/ProgramDecl.inc gpu-runtime/TensorBridgeDecl.inc
              gpu-runtime/AsyncDecl.inc gpu-runtime/LanguageDecl.inc gpu-runtime/ExecutablePath.inc
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/gpu-runtime" COMPONENT NativeCudaCompiler)
install(FILES runtime/Runtime.h runtime/Tasks.h runtime/Tensor.h runtime/NumericStorage.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/runtime" COMPONENT NativeCudaCompiler)
install(FILES include/kalandra/NumericType.h include/kalandra/TensorTransform.h
              include/kalandra/GpuProgram.h include/kalandra/GpuTransform.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/include/kalandra" COMPONENT NativeCudaCompiler)
# Only project-owned PTX and audited MIT/BSD-3-derived kernels are runtime resources.
# No CUDA headers, NVRTC, cuBLAS, driver, CRT or system SDK libraries are staged.
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cuda/transforms/"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/cuda/transforms"
  COMPONENT NativeCudaCompiler FILES_MATCHING PATTERN "*.ptx" PATTERN "*.raw.ptx" EXCLUDE)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cuda/kernels/matmul-warp64.ptx"
  "${CMAKE_CURRENT_BINARY_DIR}/cuda/kernels/matmul-catalog.ptx"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/cuda/kernels" COMPONENT NativeCudaCompiler)
install(FILES cuda/third_party/sgemm-cuda/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/kalandra-cuda"
  RENAME SGEMM_CUDA-LICENSE COMPONENT NativeCudaCompiler)
install(FILES cuda/third_party/cutlass/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/kalandra-cuda"
  RENAME CUTLASS-CXX-LICENSE COMPONENT NativeCudaCompiler)
install(FILES packaging/Cuda-README.md
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/kalandra-native-cuda"
  RENAME README.md COMPONENT NativeCudaCompiler)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cuda-install/$<CONFIG>/toolchain.json"
  CONTENT "{\n  \"format\": 1,\n  \"version\": \"${KALANDRA_VERSION}\",\n  \"package\": \"kalandra-native-cuda\",\n  \"llvmVersion\": \"${KALANDRA_LLVM_VERSION}\",\n  \"externalDependencies\": [\"system C++ development headers and runtime\", \"system NVIDIA driver with PTX 9.2 support\"],\n  \"cudaSDKIncluded\": false,\n  \"optimizedMatmulCatalog\": 1\n}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cuda-install/$<CONFIG>/toolchain.json"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/kalandra-native-cuda" COMPONENT NativeCudaCompiler)
foreach(_standalone IN ITEMS OFF ON)
  set(_name native-cuda-package)
  set(_output native-cuda)
  if(_standalone)
    set(_name native-cuda-standalone-package)
    set(_output native-cuda-standalone)
  endif()
  add_custom_target(${_name}
    COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
      "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/${_output}" -DPACKAGE_CPU=ON -DPACKAGE_CUDA=ON
      "-DSTANDALONE=${_standalone}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
    DEPENDS kalandra-installed kalandra-cuda-installed ${KALANDRA_EDITOR_TOOLS} VERBATIM)
endforeach()
