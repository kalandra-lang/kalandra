if(BUILD_TESTING)
  add_executable(kalandra-check-sanitized tools/check.cpp)
  target_link_libraries(kalandra-check-sanitized PRIVATE kalandra-semantics-sanitized kalandra-packages-sanitized)
  target_compile_options(kalandra-check-sanitized PRIVATE -Wall -Wextra -Werror
    -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(kalandra-check-sanitized PRIVATE -fsanitize=address,undefined)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-diagnostics-tests-${VARIANT} tests/DiagnosticsTests.cpp)
    target_compile_options(kalandra-diagnostics-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      set(CHECKER_TARGET kalandra-check-sanitized)
      target_link_libraries(kalandra-diagnostics-tests-${VARIANT} PRIVATE kalandra-syntax-sanitized)
      target_compile_options(kalandra-diagnostics-tests-${VARIANT} PRIVATE -O1 -g
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-diagnostics-tests-${VARIANT} PRIVATE -fsanitize=address,undefined)
    else()
      set(CHECKER_TARGET kalandra-check)
      target_link_libraries(kalandra-diagnostics-tests-${VARIANT} PRIVATE kalandra-syntax)
    endif()
    add_test(NAME native-diagnostics-${VARIANT} COMMAND kalandra-diagnostics-tests-${VARIANT})
    add_test(NAME native-diagnostics-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCHECKER=$<TARGET_FILE:${CHECKER_TARGET}>"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/diagnostics-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyDiagnostics.cmake")
    set_tests_properties(native-diagnostics-${VARIANT} native-diagnostics-cli-${VARIANT} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
