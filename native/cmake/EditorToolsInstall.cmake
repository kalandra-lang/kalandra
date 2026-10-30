# Developer tools are independent of the adjacent CPU compiler component.
# GPU bundles retain separate distribution requirements.
include(GNUInstallDirs)
set(KALANDRA_EDITOR_TOOLS kalandra-check kalandra-fmt kalandra-parse kalandra-lsp)
add_custom_target(native-editor-tools DEPENDS ${KALANDRA_EDITOR_TOOLS})
install(TARGETS ${KALANDRA_EDITOR_TOOLS}
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT NativeEditorTools)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE"
  "${CMAKE_CURRENT_SOURCE_DIR}/third-party/tomlplusplus-LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/kalandra-native-tools"
  COMPONENT NativeEditorTools)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third-party/LLVM-${KALANDRA_LLVM_VERSION}-LICENSE.txt"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/kalandra-native-tools"
  RENAME LLVM-LICENSE.txt COMPONENT NativeEditorTools)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/README.md"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/kalandra-native-tools"
  COMPONENT NativeEditorTools)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tool-install/$<CONFIG>/toolchain.json"
  CONTENT "{\n  \"format\": 1,\n  \"version\": \"${KALANDRA_VERSION}\",\n  \"package\": \"kalandra-native-editor-tools\",\n  \"platform\": \"${KALANDRA_NATIVE_PLATFORM}\",\n  \"configuration\": \"$<CONFIG>\",\n  \"llvmVersion\": \"${KALANDRA_LLVM_VERSION}\",\n  \"llvmRuntime\": \"external\",\n  \"tools\": [\"kalandra-check\", \"kalandra-fmt\", \"kalandra-parse\", \"kalandra-lsp\"]\n}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tool-install/$<CONFIG>/toolchain.json"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/kalandra-native-tools" COMPONENT NativeEditorTools)
add_custom_target(native-editor-tools-package
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DCONFIG=$<CONFIG>" "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/native-editor-tools"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
  DEPENDS ${KALANDRA_EDITOR_TOOLS} VERBATIM)
add_custom_target(native-editor-tools-standalone-package
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DCONFIG=$<CONFIG>" "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/native-editor-tools-standalone"
    -DSTANDALONE=ON -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
  DEPENDS ${KALANDRA_EDITOR_TOOLS} VERBATIM)
if(BUILD_TESTING)
  add_test(NAME native-editor-tools-install COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/InstallTests.py"
    "${CMAKE_COMMAND}" "${PROJECT_BINARY_DIR}" "$<CONFIG>"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/lsp/ProtocolTests.py" "${KALANDRA_LLVM_ROOT}"
    "$<TARGET_FILE:kalandra>")
  set_tests_properties(native-editor-tools-install PROPERTIES TIMEOUT 90)
  add_test(NAME native-standalone-editor-install COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/StandaloneTests.py" --kind editor
    --build "${PROJECT_BINARY_DIR}" --config "$<CONFIG>" --cmake "${CMAKE_COMMAND}")
  set_tests_properties(native-standalone-editor-install PROPERTIES TIMEOUT 300 LABELS distribution)
  add_test(NAME native-standalone-dependency-rejections COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/BundleMachOTests.py"
    "$<TARGET_FILE:kalandra-check>" "${KALANDRA_LLVM_ROOT}")
  set_tests_properties(native-standalone-dependency-rejections PROPERTIES TIMEOUT 30 LABELS distribution)
  add_test(NAME native-distribution-archive COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ArchiveTests.py")
  set_tests_properties(native-distribution-archive PROPERTIES TIMEOUT 30 LABELS distribution)
endif()
