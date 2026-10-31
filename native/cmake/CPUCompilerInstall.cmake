# A separately linked CPU driver has install-relative resources and no private
# Metal source-tool paths. The development driver retains its build-tree resources.
add_executable(kalandra-installed tools/compile.cpp)
set_target_properties(kalandra-installed PROPERTIES OUTPUT_NAME kalandra
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/install-bin")
target_include_directories(kalandra-installed SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(kalandra-installed PRIVATE kalandra-backend kalandra-packages LLVM MLIR)
target_compile_options(kalandra-installed PRIVATE -Wall -Wextra -Werror)
file(RELATIVE_PATH KALANDRA_CPU_RUNTIME_RELATIVE
  "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/kalandra/native")
target_compile_definitions(kalandra-installed PRIVATE KALANDRA_INSTALLED_COMPILER=1
  KALANDRA_CLANGXX="${CMAKE_CXX_COMPILER}"
  KALANDRA_RUNTIME_RELATIVE_DIR="${KALANDRA_CPU_RUNTIME_RELATIVE}")
set(KALANDRA_CPU_RUNTIME_TARGETS)
macro(kalandra_install_runtime MACRO TARGET)
  target_compile_definitions(kalandra-installed PRIVATE ${MACRO}="$<TARGET_FILE_NAME:${TARGET}>")
  if(NOT WIN32 OR (NOT VARIANT STREQUAL "thread-sanitized" AND
      (NOT KALANDRA_NATIVE_PLATFORM STREQUAL "windows-arm64" OR NOT VARIANT STREQUAL "sanitized")))
    add_dependencies(kalandra-installed ${TARGET})
    list(APPEND KALANDRA_CPU_RUNTIME_TARGETS ${TARGET})
    install(TARGETS ${TARGET} ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/kalandra/native"
      COMPONENT NativeCPUCompiler)
  endif()
endmacro()
foreach(VARIANT IN ITEMS debug sanitized thread-sanitized)
  if(VARIANT STREQUAL "debug")
    set(SUFFIX "")
    set(CORE kalandra-native-runtime)
  elseif(VARIANT STREQUAL "sanitized")
    set(SUFFIX "_SANITIZED")
    set(CORE kalandra-native-runtime-sanitized)
  else()
    set(SUFFIX "_THREAD_SANITIZED")
    set(CORE kalandra-native-runtime-thread-sanitized)
  endif()
  kalandra_install_runtime(KALANDRA${SUFFIX}_RUNTIME ${CORE})
  foreach(PAIR IN ITEMS "INT32|int32" "UINT32|uint32" "INT64|int64" "UINT64|uint64"
      "FLOAT64|float64" "STRING|string" "BOOL|bool" "COLLECTION|collection" "STRING_ARRAY|string-array")
    string(REPLACE "|" ";" FIELDS "${PAIR}")
    list(GET FIELDS 0 LABEL)
    list(GET FIELDS 1 STEM)
    kalandra_install_runtime(KALANDRA_${LABEL}${SUFFIX}_ENTRY kalandra-${STEM}-entry-${VARIANT})
  endforeach()
endforeach()
install(TARGETS kalandra-installed RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT NativeCPUCompiler)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/CPU-README.md"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/kalandra-native-cpu"
  RENAME README.md COMPONENT NativeCPUCompiler)
add_custom_target(native-cpu-toolchain DEPENDS kalandra-installed native-editor-tools)
add_custom_target(native-cpu-package
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
    "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/native-cpu" -DPACKAGE_CPU=ON
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
  DEPENDS kalandra-installed ${KALANDRA_EDITOR_TOOLS} VERBATIM)
add_custom_target(native-cpu-standalone-package
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
    "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}/native-cpu-standalone" -DPACKAGE_CPU=ON -DSTANDALONE=ON
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake"
  DEPENDS kalandra-installed ${KALANDRA_EDITOR_TOOLS} VERBATIM)
if(BUILD_TESTING)
  add_test(NAME native-cpu-install COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/CompilerInstallTests.py" "${CMAKE_COMMAND}"
    "${PROJECT_BINARY_DIR}" "$<CONFIG>" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageEditorTools.cmake")
  set_tests_properties(native-cpu-install PROPERTIES TIMEOUT 240)
  add_test(NAME native-standalone-cpu-install COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/StandaloneTests.py" --kind cpu
    --build "${PROJECT_BINARY_DIR}" --config "$<CONFIG>" --cmake "${CMAKE_COMMAND}")
  set_tests_properties(native-standalone-cpu-install PROPERTIES TIMEOUT 300 LABELS distribution)
endif()
