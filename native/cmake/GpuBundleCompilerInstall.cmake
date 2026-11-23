if(NOT TARGET native-vulkan-package OR NOT TARGET native-cuda-package)
  return()
endif()

# Reuse the single-backend runtime components. Only the installed driver and
# the backend-neutral host headers differ from their union.
add_executable(kalandra-gpu-installed tools/compile.cpp gpu-source/VulkanBuild.cpp cuda-source/Build.cpp)
set_target_properties(kalandra-gpu-installed PROPERTIES OUTPUT_NAME kalandra
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gpu-install-bin")
get_target_property(_cpu_definitions kalandra-installed COMPILE_DEFINITIONS)
file(RELATIVE_PATH _gpu_tools "${CMAKE_INSTALL_FULL_LIBDIR}/kalandra/native"
  "${CMAKE_INSTALL_FULL_LIBEXECDIR}/kalandra")
target_compile_definitions(kalandra-gpu-installed PRIVATE ${_cpu_definitions}
  KALANDRA_HAS_CUDA_SOURCE=1 KALANDRA_HAS_VULKAN_SOURCE=1 KALANDRA_BUNDLE_COMPILER=1
  KALANDRA_CUDA_TOOLS_RELATIVE="${_gpu_tools}" KALANDRA_VULKAN_TOOLS_RELATIVE="${_gpu_tools}")
target_include_directories(kalandra-gpu-installed SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(kalandra-gpu-installed PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gpu-source/$<CONFIG>"
  "${CMAKE_CURRENT_BINARY_DIR}/cuda-source/$<CONFIG>" "${CMAKE_CURRENT_SOURCE_DIR}/vulkan")
target_compile_options(kalandra-gpu-installed PRIVATE -Wall -Wextra -Werror)
target_link_libraries(kalandra-gpu-installed PRIVATE kalandra-vulkan-source-backend
  kalandra-cuda-source-backend kalandra-backend kalandra-packages LLVM MLIR)
install(TARGETS kalandra-gpu-installed RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT NativeBundleCompiler)
install(FILES gpu-bundle/Language.h gpu-bundle/Selection.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/gpu-bundle" COMPONENT NativeBundleCompiler)
add_custom_target(native-gpu-standalone-package
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
    "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/native-gpu-standalone" -DPACKAGE_CPU=ON
    -DPACKAGE_VULKAN=ON -DPACKAGE_CUDA=ON -DSTANDALONE=ON
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
  DEPENDS kalandra-installed kalandra-cuda-installed kalandra-vulkan-installed
    kalandra-gpu-installed ${KALANDRA_EDITOR_TOOLS} VERBATIM)
