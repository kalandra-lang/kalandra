if(NOT TARGET kalandra-vulkan-source-backend)
  return()
endif()
# Keep the development-profile install guard authoritative. This target can be
# compiled there, but only production-pinned builds may stage an installation.
add_executable(kalandra-vulkan-installed tools/compile.cpp gpu-source/VulkanBuild.cpp)
set_target_properties(kalandra-vulkan-installed PROPERTIES OUTPUT_NAME kalandra
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vulkan-install-bin")
get_target_property(_cpu_definitions kalandra-installed COMPILE_DEFINITIONS)
file(RELATIVE_PATH _vulkan_tools_relative
  "${CMAKE_INSTALL_FULL_LIBDIR}/kalandra/native" "${CMAKE_INSTALL_FULL_LIBEXECDIR}/kalandra")
target_compile_definitions(kalandra-vulkan-installed PRIVATE ${_cpu_definitions}
  KALANDRA_HAS_VULKAN_SOURCE=1 KALANDRA_VULKAN_TOOLS_RELATIVE="${_vulkan_tools_relative}")
target_include_directories(kalandra-vulkan-installed SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(kalandra-vulkan-installed PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gpu-source/$<CONFIG>"
  "${CMAKE_CURRENT_SOURCE_DIR}/vulkan")
target_link_libraries(kalandra-vulkan-installed PRIVATE kalandra-vulkan-source-backend
  kalandra-backend kalandra-packages LLVM MLIR)
target_compile_options(kalandra-vulkan-installed PRIVATE -Wall -Wextra -Werror)
add_dependencies(kalandra-vulkan-installed ${KALANDRA_CPU_RUNTIME_TARGETS}
  kalandra-vulkan-lower kalandra-vulkan-extract kalandra-vulkan-language)
if(TARGET kalandra-vulkan-language-sanitized)
  add_dependencies(kalandra-vulkan-installed kalandra-vulkan-language-sanitized)
endif()
find_file(KALANDRA_VULKAN_SPIRV_NOTICE NAMES copyright LICENSE
  PATHS /usr/share/doc/spirv-tools /usr/share/doc/packages/spirv-tools
        /usr/share/licenses/spirv-tools NO_DEFAULT_PATH
  DOC "Copyright/license notice belonging to the configured SPIR-V validator")
if(NOT KALANDRA_VULKAN_SPIRV_NOTICE)
  message(STATUS "Vulkan packaging requires KALANDRA_VULKAN_SPIRV_NOTICE for the configured validator")
  return()
endif()
install(TARGETS kalandra-vulkan-installed RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT NativeVulkanCompiler)
foreach(_target IN ITEMS kalandra-vulkan kalandra-vulkan-program kalandra-vulkan-async
                         kalandra-vulkan-language kalandra-vulkan-sanitized
                         kalandra-vulkan-program-sanitized kalandra-vulkan-async-sanitized
                         kalandra-vulkan-language-sanitized)
  if(TARGET ${_target})
    install(TARGETS ${_target} ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native"
      COMPONENT NativeVulkanCompiler)
  endif()
endforeach()
install(TARGETS kalandra-vulkan-lower kalandra-vulkan-extract
  RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/kalandra" COMPONENT NativeVulkanCompiler)
foreach(_tool IN ITEMS mlir-opt mlir-translate)
  get_filename_component(_program "${KALANDRA_LLVM_ROOT}/bin/${_tool}${CMAKE_EXECUTABLE_SUFFIX}" REALPATH)
  install(PROGRAMS "${_program}" DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/kalandra"
    RENAME "${_tool}${CMAKE_EXECUTABLE_SUFFIX}" COMPONENT NativeVulkanCompiler)
endforeach()
get_filename_component(_validator "${KALANDRA_SPIRV_VAL}" REALPATH)
install(PROGRAMS "${_validator}" DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/kalandra"
  RENAME "spirv-val${CMAKE_EXECUTABLE_SUFFIX}" COMPONENT NativeVulkanCompiler)
install(FILES vulkan/Language.h vulkan/Async.h vulkan/Program.h vulkan/Vulkan.h
              vulkan/TensorBridge.h vulkan/ExecutablePath.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/vulkan" COMPONENT NativeVulkanCompiler)
# These declarations are shared with CUDA, but remain owned package resources.
install(FILES gpu-runtime/ProgramDecl.inc gpu-runtime/TensorBridgeDecl.inc
              gpu-runtime/AsyncDecl.inc gpu-runtime/LanguageDecl.inc gpu-runtime/ExecutablePath.inc
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/gpu-runtime" COMPONENT NativeVulkanCompiler)
install(FILES runtime/Runtime.h runtime/Tasks.h runtime/Tensor.h runtime/NumericStorage.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/runtime" COMPONENT NativeVulkanCompiler)
install(FILES include/kalandra/NumericType.h include/kalandra/TensorTransform.h
              include/kalandra/GpuProgram.h include/kalandra/GpuTransform.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/include/kalandra" COMPONENT NativeVulkanCompiler)
install(FILES vulkan/third_party/ggml/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/kalandra-vulkan"
  RENAME ggml-LICENSE COMPONENT NativeVulkanCompiler)
install(FILES "${KALANDRA_VULKAN_SPIRV_NOTICE}"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/kalandra-vulkan"
  RENAME SPIRV-Tools-COPYRIGHT COMPONENT NativeVulkanCompiler)
install(FILES packaging/Vulkan-README.md
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/kalandra-native-vulkan"
  RENAME README.md COMPONENT NativeVulkanCompiler)
file(SHA256 "${KALANDRA_SPIRV_VAL}" _validator_sha)
file(SHA256 "${KALANDRA_VULKAN_SPIRV_NOTICE}" _notice_sha)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vulkan-install/$<CONFIG>/toolchain.json"
  CONTENT "{\n  \"format\": 1,\n  \"version\": \"${KALANDRA_VERSION}\",\n  \"package\": \"kalandra-native-vulkan\",\n  \"llvmVersion\": \"${KALANDRA_LLVM_VERSION}\",\n  \"validatorSourceSHA256\": \"${_validator_sha}\",\n  \"validatorNoticeSHA256\": \"${_notice_sha}\",\n  \"externalDependencies\": [\"system C++ development headers and runtime\", \"system Vulkan loader and GPU driver\"]\n}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vulkan-install/$<CONFIG>/toolchain.json"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/kalandra-native-vulkan" COMPONENT NativeVulkanCompiler)

foreach(_standalone IN ITEMS OFF ON)
  set(_name native-vulkan-package)
  set(_output native-vulkan)
  if(_standalone)
    set(_name native-vulkan-standalone-package)
    set(_output native-vulkan-standalone)
  endif()
  add_custom_target(${_name}
    COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
      "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/${_output}" -DPACKAGE_CPU=ON -DPACKAGE_VULKAN=ON
      "-DSTANDALONE=${_standalone}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
    DEPENDS kalandra-installed kalandra-vulkan-installed ${KALANDRA_EDITOR_TOOLS} VERBATIM)
endforeach()
