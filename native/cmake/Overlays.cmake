if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-overlay-tests-${VARIANT} tests/OverlayTests.cpp)
    target_compile_options(kalandra-overlay-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      set(CHECKER_TARGET kalandra-check-sanitized)
      target_link_libraries(kalandra-overlay-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_options(kalandra-overlay-tests-${VARIANT} PRIVATE -O1 -g
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-overlay-tests-${VARIANT} PRIVATE -fsanitize=address,undefined)
    else()
      set(CHECKER_TARGET kalandra-check)
      target_link_libraries(kalandra-overlay-tests-${VARIANT} PRIVATE kalandra-semantics)
    endif()
    add_test(NAME native-overlays-${VARIANT} COMMAND kalandra-overlay-tests-${VARIANT}
      "${CMAKE_CURRENT_BINARY_DIR}/overlays-${VARIANT}")
    add_test(NAME native-overlays-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCHECKER=$<TARGET_FILE:${CHECKER_TARGET}>"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/overlays-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyOverlays.cmake")
    set_tests_properties(native-overlays-${VARIANT} native-overlays-cli-${VARIANT} PROPERTIES TIMEOUT 90)
  endforeach()
endif()
