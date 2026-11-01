if(NOT KALANDRA_BUILD_METAL_RUNTIME OR NOT KALANDRA_BUILD_METAL_PROOF)
  return()
endif()
# Reuse source lowering, but supply the installed build driver as an executable
# object. The archive's build-tree Build.cpp object is not pulled into this link.
add_executable(kalandra-metal-installed tools/compile.cpp metal-source/Build.cpp)
set_target_properties(kalandra-metal-installed PROPERTIES OUTPUT_NAME kalandra
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/metal-install-bin")
get_target_property(CPU_INSTALL_DEFINITIONS kalandra-installed COMPILE_DEFINITIONS)
target_compile_definitions(kalandra-metal-installed PRIVATE ${CPU_INSTALL_DEFINITIONS}
  KALANDRA_HAS_METAL_SOURCE=1)
target_include_directories(kalandra-metal-installed SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(kalandra-metal-installed PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/metal-source")
target_link_libraries(kalandra-metal-installed PRIVATE kalandra-metal-source-backend kalandra-backend kalandra-packages LLVM MLIR)
target_compile_options(kalandra-metal-installed PRIVATE -Wall -Wextra -Werror)
add_dependencies(kalandra-metal-installed ${KALANDRA_CPU_RUNTIME_TARGETS}
  kalandra-metal-strict kalandra-metal-strict64 kalandra-metal-extract kalandra-task-core-optimized)
foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
  add_dependencies(kalandra-metal-installed kalandra-metal-${VARIANT})
  install(TARGETS kalandra-metal-${VARIANT} ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native"
    COMPONENT NativeMetalCompiler)
endforeach()
install(TARGETS kalandra-task-core-optimized ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native"
  COMPONENT NativeMetalCompiler)
install(TARGETS kalandra-metal-strict kalandra-metal-strict64 kalandra-metal-extract
  RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native" COMPONENT NativeMetalCompiler)
install(TARGETS kalandra-metal-installed RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT NativeMetalCompiler)
# Private generated-host headers retain their internal relative include layout.
install(FILES metal/Metal.h metal/ElementwiseLayout.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/metal" COMPONENT NativeMetalCompiler)
install(FILES runtime/Runtime.h runtime/Tasks.h runtime/Tensor.h runtime/NumericStorage.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/runtime" COMPONENT NativeMetalCompiler)
install(FILES include/kalandra/NumericType.h include/kalandra/TensorTransform.h include/kalandra/GpuProgram.h
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native/headers/include/kalandra" COMPONENT NativeMetalCompiler)
install(FILES packaging/Metal-README.md
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/kalandra-native-metal"
  RENAME README.md COMPONENT NativeMetalCompiler)
install(FILES third_party/mlx-steel/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/kalandra-native-metal"
  RENAME MLX-Steel-LICENSE.txt COMPONENT NativeMetalCompiler)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/metal-install/$<CONFIG>/toolchain.json"
  CONTENT "{\n  \"format\": 1,\n  \"version\": \"${KALANDRA_VERSION}\",\n  \"package\": \"kalandra-native-metal\",\n  \"configuration\": \"$<CONFIG>\",\n  \"llvmVersion\": \"${KALANDRA_LLVM_VERSION}\",\n  \"spirvCross\": \"${KALANDRA_SPIRV_CROSS_ROOT}\",\n  \"spirvTools\": \"${KALANDRA_SPIRV_TOOLS_ROOT}\",\n  \"metalToolchain\": \"${KALANDRA_METAL_TOOLCHAIN}\",\n  \"metalCompiler\": \"${KALANDRA_METAL_COMPILER}\",\n  \"macosSdk\": \"${KALANDRA_METAL_SDK}\",\n  \"externalDependencies\": true\n}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/metal-install/$<CONFIG>/toolchain.json"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/kalandra-native-metal" COMPONENT NativeMetalCompiler)
add_custom_target(native-metal-package
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
    "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/native-metal" -DPACKAGE_CPU=ON -DPACKAGE_METAL=ON
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
  DEPENDS kalandra-installed kalandra-metal-installed ${KALANDRA_EDITOR_TOOLS} VERBATIM)
add_custom_target(native-metal-standalone-package
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
    "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/native-metal-standalone" -DPACKAGE_CPU=ON -DPACKAGE_METAL=ON -DSTANDALONE=ON
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
  DEPENDS kalandra-installed kalandra-metal-installed ${KALANDRA_EDITOR_TOOLS} VERBATIM)
if(BUILD_TESTING)
  add_test(NAME native-standalone-metal-install COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/StandaloneTests.py" --kind metal
    --build "${PROJECT_BINARY_DIR}" --cmake "${CMAKE_COMMAND}" --config "$<CONFIG>")
  set_tests_properties(native-standalone-metal-install PROPERTIES TIMEOUT 1200 LABELS distribution
    ENVIRONMENT "KALANDRA_METAL_COMPILER=${KALANDRA_METAL_COMPILER}")
  add_test(NAME native-metal-install COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/MetalInstallTests.py" "${CMAKE_COMMAND}"
    "${PROJECT_BINARY_DIR}" "$<CONFIG>" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/metal-source")
  set_tests_properties(native-metal-install PROPERTIES TIMEOUT 1200 RESOURCE_LOCK apple-metal-gpu
    ENVIRONMENT "MTL_DEBUG_LAYER=1;KALANDRA_METAL_COMPILER=${KALANDRA_METAL_COMPILER}")
endif()
